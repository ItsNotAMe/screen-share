#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include "media/audio/WasapiPcmEndpoint.h"

#include "api/audio/audio_device.h"
#include "api/audio/create_audio_device_module.h"
#include "api/audio_codecs/audio_decoder_factory_template.h"
#include "api/audio_codecs/audio_encoder_factory_template.h"
#include "api/audio_codecs/opus/audio_decoder_opus.h"
#include "api/audio_codecs/opus/audio_encoder_opus.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/enable_media.h"
#include "api/environment/environment_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/peer_connection_interface.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
#include "rtc_base/win32_socket_init.h"
#include "rtc_base/time_utils.h"
#include "rtc_base/logging.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_broadcaster.h"
#include "pc/video_track_source.h"
#include "media/webrtc/MfVideoDecoderFactory.h"
#include "media/webrtc/MfVideoEncoderFactory.h"
#include "media/webrtc/MfHardwareSession.h"
#include "media/webrtc/PcmAudioDeviceModule.h"
#include "SyntheticAudio.h"
#include "CaptureTestWindow.h"
#include "LiveCaptureSource.h"
#include "PresentationTestWindow.h"

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Predicate>
void Wait(Predicate predicate, const char* failure) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!predicate()) {
        Require(std::chrono::steady_clock::now() < deadline, failure);
        webrtc::Thread::Current()->ProcessMessages(10);
    }
}

class CreatedDescription : public webrtc::CreateSessionDescriptionObserver {
public:
    void OnSuccess(webrtc::SessionDescriptionInterface* value) override {
        description.reset(value);
        done = true;
    }
    void OnFailure(webrtc::RTCError) override { done = true; }
    bool done = false;
    std::unique_ptr<webrtc::SessionDescriptionInterface> description;
};

class AppliedDescription : public webrtc::SetSessionDescriptionObserver {
public:
    void OnSuccess() override { success = true; done = true; }
    void OnFailure(webrtc::RTCError) override { done = true; }
    bool done = false;
    bool success = false;
};

class Channel : public webrtc::DataChannelObserver {
public:
    explicit Channel(webrtc::scoped_refptr<webrtc::DataChannelInterface> value)
        : channel(std::move(value)), expected("screenshare-proof:" + channel->label()) { channel->RegisterObserver(this); }
    ~Channel() override { channel->UnregisterObserver(); }
    void OnStateChange() override {}
    void OnMessage(const webrtc::DataBuffer& buffer) override {
        const std::string actual(buffer.data.cdata<char>(), buffer.data.size());
        received = actual == expected;
    }
    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel;
    std::string expected;
    bool received = false;
};

class SyntheticVideo : public webrtc::VideoTrackSource {
public:
    void PushBuffer(webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer) {
        broadcaster_.OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(std::move(buffer))
            .set_timestamp_us(webrtc::TimeMicros()).build());
    }
    explicit SyntheticVideo(std::shared_ptr<screenshare::media::D3dVideoDevice> device = {})
        : VideoTrackSource(false), device_(std::move(device)) {}
    void Push(int index) {
        if (device_) {
            std::vector<uint8_t> pixels(640 * 360 * 3 / 2, 128);
            std::fill_n(pixels.begin(), 640 * 360, uint8_t(50 + index % 100));
            auto buffer = device_->UploadNv12(640, 360, pixels);
            broadcaster_.OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer)
                .set_timestamp_us(webrtc::TimeMicros()).build());
            return;
        }
        auto buffer = webrtc::I420Buffer::Create(640, 360);
        for (int y = 0; y < 360; ++y)
            std::fill_n(buffer->MutableDataY() + y * buffer->StrideY(), 640, uint8_t(50 + index % 100));
        for (int y = 0; y < 180; ++y) {
            std::fill_n(buffer->MutableDataU() + y * buffer->StrideU(), 320, uint8_t(128));
            std::fill_n(buffer->MutableDataV() + y * buffer->StrideV(), 320, uint8_t(128));
        }
        broadcaster_.OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer)
            .set_timestamp_us(webrtc::TimeMicros()).build());
    }
protected:
    webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override { return &broadcaster_; }
private:
    webrtc::VideoBroadcaster broadcaster_;
    std::shared_ptr<screenshare::media::D3dVideoDevice> device_;
};

class Peer : public webrtc::PeerConnectionObserver, public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    ~Peer() override {
        if (video) video->RemoveSink(this);
        channels.clear();
        if (connection) connection->Close();
    }
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
    void OnIceCandidate(const webrtc::IceCandidate*) override {}
    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
        auto track = transceiver->receiver()->track();
        if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
            video = static_cast<webrtc::VideoTrackInterface*>(track.get());
            video->AddOrUpdateSink(this, webrtc::VideoSinkWants());
        }
    }
    void OnFrame(const webrtc::VideoFrame& frame) override {
        auto buffer = frame.video_frame_buffer();
        auto pixels = buffer->type() == webrtc::VideoFrameBuffer::Type::kNV12 ? nullptr : buffer->ToI420();
        auto nv12 = buffer->type() == webrtc::VideoFrameBuffer::Type::kNV12 ? buffer->GetNV12() : nullptr;
        const auto* y = nv12 ? nv12->DataY() : (pixels ? pixels->DataY() : nullptr);
        const int stride = nv12 ? nv12->StrideY() : (pixels ? pixels->StrideY() : 0);
        if (frame.width() != 640 || frame.height() != 360 || !y || y[checkCenter ? stride * 180 + 320 : 0] < 35)
            invalidFrames.fetch_add(1);
        if (checkCenter && nv12 && frame.width() == 640 && frame.height() == 360) {
            const auto* uv = nv12->DataUV() + nv12->StrideUV() * 90 + 320;
            if (std::abs(int(uv[0]) - 128) > 12 || std::abs(int(uv[1]) - 128) > 12) ++invalidFrames;
        }
        if (presentationSink) presentationSink->OnFrame(frame);
        decodedFrames.fetch_add(1);
    }
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> value) override {
        channels.push_back(std::make_unique<Channel>(std::move(value)));
    }
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> connection;
    std::vector<std::unique_ptr<Channel>> channels;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> video;
    std::atomic<unsigned> decodedFrames{0};
    std::atomic<unsigned> invalidFrames{0};
    bool checkCenter = false;
    screenshare::media::LatestVideoFrameSink* presentationSink = nullptr;
};

void TransferDescription(Peer& from, Peer& to, bool offer) {
    auto created = webrtc::make_ref_counted<CreatedDescription>();
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    if (offer) from.connection->CreateOffer(created.get(), options);
    else from.connection->CreateAnswer(created.get(), options);
    Wait([&] { return created->done; }, "SDP creation timed out");
    Require(created->description != nullptr, "SDP creation failed");
    auto applied = webrtc::make_ref_counted<AppliedDescription>();
    from.connection->SetLocalDescription(applied.get(), created->description.release());
    Wait([&] { return applied->done; }, "Local description timed out");
    Require(applied->success, "Local description failed");
    // This local proof bundles candidates in SDP. Production uses targeted trickle ICE.
    Wait([&] { return from.connection->ice_gathering_state() ==
        webrtc::PeerConnectionInterface::kIceGatheringComplete; }, "ICE gathering timed out");
    std::string sdp;
    Require(from.connection->local_description()->ToString(&sdp), "SDP serialization failed");
    auto remote = webrtc::CreateSessionDescription(offer ? webrtc::SdpType::kOffer : webrtc::SdpType::kAnswer, sdp);
    Require(remote != nullptr, "SDP parsing failed");
    auto received = webrtc::make_ref_counted<AppliedDescription>();
    to.connection->SetRemoteDescription(received.get(), remote.release());
    Wait([&] { return received->done; }, "Remote description timed out");
    Require(received->success, "Remote description failed");
}

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
        live = std::make_unique<proof::LiveCaptureSource>(window->handle(), [source](auto buffer) { source->PushBuffer(std::move(buffer)); });
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
    Wait([&] {
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
    std::jthread capture([source, &captureFailed, &live](std::stop_token stop) {
        if (live) return;
        try {
            int index = 0;
            auto next = std::chrono::steady_clock::now();
            while (!stop.stop_requested()) {
                source->Push(index++);
                next += std::chrono::milliseconds(33);
                std::this_thread::sleep_until(next);
            }
        } catch (...) { captureFailed = true; }
    });
    bool presentationResized = false;
    try { Wait([&] {
        if (presentation && !presentationResized && viewer.decodedFrames >= 25) {
            presentation->Resize(); presentationResized = true;
        }
        if (presentation) presentation->Drain(presentationSink);
        return captureFailed || (viewer.decodedFrames.load() >= 60 && audioEvidence->audibleBlocks >= 30 &&
            (!presentation || presentation->presented >= 30));
    }, "Audio/video media delivery timed out"); }
    catch (...) {
        std::cerr << "video_frames=" << viewer.decodedFrames << " audio_capture=" << audioDiagnostics->capturedBlocks
                  << " audio_playout=" << audioDiagnostics->playedBlocks << " audible=" << audioEvidence->audibleBlocks
                  << " capture_errors=" << audioDiagnostics->captureErrors << " playout_errors=" << audioDiagnostics->playoutErrors << '\n';
        throw;
    }
    capture.request_stop();
    capture.join();
    if (live) {
        live->Stop();
        Require(!live->failed && live->frames >= 60, "Live capture source failed");
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
        Run(mode == "--hardware" || mode == "--live-capture", mode == "--wasapi", mode == "--live-capture");
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    webrtc::CleanupSSL();
    return result;
}
