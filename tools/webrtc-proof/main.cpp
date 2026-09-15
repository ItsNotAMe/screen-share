#include "ProofPeer.h"
#include "MultiViewerScenario.h"
using namespace proofmedia;

namespace {
void Run(bool useHardware, bool useWasapi, bool useLiveCapture) {
    webrtc::AutoThread mainThread;
    auto network = webrtc::Thread::CreateWithSocketServer();
    auto worker = webrtc::Thread::Create();
    Require(network->Start() && worker->Start(), "Thread startup failed");
    webrtc::PeerConnectionFactoryDependencies dependencies;
    dependencies.env = webrtc::CreateEnvironment();
    dependencies.network_thread = network.get();
    dependencies.worker_thread = worker.get();
    dependencies.signaling_thread = webrtc::Thread::Current();
    auto audioEvidence = std::make_shared<proof::AudioEvidence>();
    auto audioDiagnostics = std::make_shared<screenshare::media::PcmAudioDiagnostics>();
    auto endpoints = proof::SyntheticAudio(audioEvidence);
    std::atomic<bool> toneFailed{false};
    std::jthread tone;
    if (useWasapi) {
        screenshare::AudioCaptureConfig config;
        config.source = screenshare::AudioCaptureSource::ProcessOutput;
        config.processId = GetCurrentProcessId();
        auto physical = screenshare::media::WasapiPcmEndpoints(config);
        endpoints.capture = physical.capture;
        // Render a quiet source tone; received PCM is measured without replay,
        // so process loopback cannot recursively capture its own received audio.
        tone = std::jthread([create = physical.playout, &toneFailed](std::stop_token stop) {
            try {
                auto output = create(); output->Start();
                proof::ToneCapture source; source.Start();
                screenshare::media::PcmBlock block;
                while (source.Read(block, stop)) {
                    for (auto& sample : block) sample /= 2;
                    output->Write(block, stop);
                }
            } catch (...) { toneFailed = true; }
        });
    }
    dependencies.adm = screenshare::media::CreatePcmAudioDeviceModule(std::move(endpoints), audioDiagnostics);
    dependencies.audio_encoder_factory = webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus>();
    dependencies.audio_decoder_factory = webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus>();
    std::unique_ptr<proof::TestWindow> window;
    auto source = webrtc::make_ref_counted<SyntheticVideo>();
    std::unique_ptr<proof::LiveCaptureSource> live;
    if (useLiveCapture) {
        window = std::make_unique<proof::TestWindow>();
        live = std::make_unique<proof::LiveCaptureSource>(window->handle(), [source](auto sample) {
            auto resource = std::static_pointer_cast<screenshare::media::WindowsCaptureResource>(sample.resource);
            source->PushBuffer(resource->buffer, sample.capturedAt);
        });
    }
    auto device = live ? live->device() : (useHardware ? std::make_shared<screenshare::media::D3dVideoDevice>() : nullptr);
    auto hardware = useHardware ? std::make_shared<screenshare::media::MfHardwareSession>(device) : nullptr;
    auto encoders = std::make_unique<screenshare::media::MfVideoEncoderFactory>(hardware);
    auto decoders = std::make_unique<screenshare::media::MfVideoDecoderFactory>();
    Require(!encoders->GetSupportedFormats().empty() && !decoders->GetSupportedFormats().empty(), "H264 factories unavailable");
    dependencies.video_encoder_factory = std::move(encoders);
    dependencies.video_decoder_factory = std::move(decoders);
    webrtc::EnableMedia(dependencies);
    auto factory = webrtc::CreateModularPeerConnectionFactory(std::move(dependencies));
    Require(factory != nullptr, "Factory creation failed");
    screenshare::media::LatestVideoFrameSink presentationSink;
    auto presentation = useLiveCapture ? std::make_unique<proof::PresentationTestWindow>() : nullptr;
    Peer host;
    Peer viewer;
    viewer.checkCenter = useLiveCapture;
    if (presentation) viewer.presentationSink = &presentationSink;
    for (Peer* peer : {&host, &viewer}) {
        webrtc::PeerConnectionInterface::RTCConfiguration config;
        config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
        config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
        auto result = factory->CreatePeerConnectionOrError(config, webrtc::PeerConnectionDependencies(peer));
        Require(result.ok(), "PeerConnection creation failed");
        peer->connection = result.MoveValue();
    }
    for (const auto& label : {"control", "input-state", "telemetry"}) {
        webrtc::DataChannelInit init;
        if (std::string(label) != "control") { init.ordered = false; init.maxRetransmits = 0; }
        auto result = host.connection->CreateDataChannelOrError(label, &init);
        Require(result.ok(), "Data channel creation failed");
        host.channels.push_back(std::make_unique<Channel>(result.MoveValue()));
    }
    if (!live) source = webrtc::make_ref_counted<SyntheticVideo>(device);
    auto videoTrack = factory->CreateVideoTrack(source, "synthetic-video");
    Require(host.connection->AddTrack(videoTrack, {"proof-stream"}).ok(), "Video track creation failed");
    webrtc::AudioOptions audioOptions;
    audioOptions.echo_cancellation = false;
    audioOptions.auto_gain_control = false;
    audioOptions.noise_suppression = false;
    auto audioSource = factory->CreateAudioSource(audioOptions);
    auto audioTrack = factory->CreateAudioTrack("synthetic-audio", audioSource.get());
    Require(host.connection->AddTrack(audioTrack, {"proof-stream"}).ok(), "Audio track creation failed");
    TransferDescription(host, viewer, true);
    TransferDescription(viewer, host, false);
    WaitForPeer(host, [&] {
        if (host.lifecycle.state() != screenshare::media::PeerLifecycleState::Connected ||
            viewer.lifecycle.state() != screenshare::media::PeerLifecycleState::Connected) return false;
        if (viewer.channels.size() != 3) return false;
        for (const auto& channel : host.channels)
            if (channel->channel->state() != webrtc::DataChannelInterface::kOpen) return false;
        return true;
    }, "Direct data channels did not open");
    for (const auto& channel : host.channels)
        Require(channel->channel->Send(webrtc::DataBuffer("screenshare-proof:" + channel->channel->label())), "Send failed");
    Wait([&] {
        for (const auto& channel : viewer.channels) if (!channel->received) return false;
        return true;
    }, "Data channel delivery timed out");
    // Capture must not block the WebRTC signaling thread on D3D work.
    std::atomic<bool> captureFailed{false};
    if (live) live->StartDelivery();
    std::unique_ptr<screenshare::media::CaptureDistributor> distribution;
    std::unique_ptr<screenshare::media::CaptureSession> capture;
    if (!live) {
        distribution = std::make_unique<screenshare::media::CaptureDistributor>(1);
        distribution->Add(1, [source](auto sample) {
            source->Push(*std::static_pointer_cast<screenshare::media::SyntheticCaptureResource>(sample.resource), sample.capturedAt);
        });
        capture = std::make_unique<screenshare::media::CaptureSession>(1,
            [] { return std::make_unique<screenshare::media::SyntheticCaptureSource>(640, 360, 30); },
            [&](auto sample) { distribution->Publish(std::move(sample)); });
        capture->EnableDelivery();
    }
    bool presentationResized = false;
    try { Wait([&] {
        if (presentation && !presentationResized && viewer.decodedFrames >= 25) {
            presentation->Resize(); presentationResized = true;
        }
        if (presentation) presentation->Drain(presentationSink);
        captureFailed = capture && (capture->status().state == screenshare::media::CaptureState::Failed || distribution->stats(1).failed);
        return captureFailed || (viewer.decodedFrames.load() >= 60 && audioEvidence->audibleBlocks >= 30 &&
            (!presentation || presentation->presented >= 30));
    }, "Audio/video media delivery timed out"); }
    catch (...) {
        std::cerr << "video_frames=" << viewer.decodedFrames << " audio_capture=" << audioDiagnostics->capturedBlocks
                  << " audio_playout=" << audioDiagnostics->playedBlocks << " audible=" << audioEvidence->audibleBlocks
                  << " capture_errors=" << audioDiagnostics->captureErrors << " playout_errors=" << audioDiagnostics->playoutErrors << '\n';
        throw;
    }
    if (capture) capture->Stop();
    if (distribution) distribution->Stop();
    if (live) {
        live->Stop();
        Require(!live->HasFailed() && live->frames >= 60, "Live capture source failed");
    }
    Require(!captureFailed, "Synthetic capture failed");
    Require(!toneFailed, "Physical source tone failed");
    Require(audioDiagnostics->captureErrors == 0 && audioDiagnostics->playoutErrors == 0, "Audio device transport failed");
    if (presentation) {
        presentation->ValidateVisiblePixels();
        Require(presentation->conversions == 0 && presentation->repacks == 0, "NV12 presentation unnecessarily converted/repacked decoder output");
        std::cout << "gpu_present_submissions=" << presentation->presented << " replaced_pending_frames=" << presentationSink.replaced()
                  << " presentation_conversions=" << presentation->conversions << '\n';
        presentationSink.Stop();
    }
    std::cout << "opus_audible_blocks=" << audioEvidence->audibleBlocks << '\n';
    Require(viewer.invalidFrames.load() == 0, "MF received incorrect visible dimensions or pixels");
    if (hardware) {
        Require(hardware->hardwareFrames >= 60 && !hardware->quarantined, "Hardware path failed or fell back");
        Require(device->readbackCount() == 0, "Hardware path unexpectedly read back GPU pixels");
        std::cout << "hardware_frames=" << hardware->hardwareFrames << " max_hardware_frame_us="
                  << hardware->maxFrameMicroseconds << " gpu_readbacks=" << device->readbackCount() << '\n';
    }
    std::cout << "Three direct DTLS data channels delivered messages; " << viewer.decodedFrames.load()
              << " H264 frames decoded through Media Foundation.\n"
              << (useWasapi ? "WASAPI process-loopback" : "Synthetic")
              << " Opus PCM received through the ADM. End-to-end latency remains unmeasured.\n";
}
} // namespace

int main(int argc, char** argv) {
    // Upstream connection logs contain ICE credentials and addresses. Keep the
    // diagnostic's own explicit results, not raw transport logs.
    webrtc::LoggingConfig logging;
    logging.set_min_severity(webrtc::LS_NONE);
    logging.set_debug_severity(webrtc::LS_NONE);
    logging.set_log_to_stderr(false);
    if (!webrtc::InitializeLogging(std::move(logging))) return 1;
    webrtc::WinsockInitializer winsock;
    if (winsock.error() != 0) return 1;
    if (!webrtc::InitializeSSL()) return 1;
    int result = 0;
    try {
        const std::string mode = argc == 2 ? argv[1] : "";
        if (mode == "--multi-viewer") RunMultiViewer();
        else if (mode == "--negotiation-only") RunMultiViewer(true);
        else {
            Require(mode.empty() || mode == "--hardware" || mode == "--live-capture" || mode == "--wasapi",
                    "Unknown proof mode");
            Require(argc <= 2, "Unexpected proof arguments");
            Run(mode == "--hardware" || mode == "--live-capture", mode == "--wasapi", mode == "--live-capture");
        }
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    webrtc::CleanupSSL();
    return result;
}
