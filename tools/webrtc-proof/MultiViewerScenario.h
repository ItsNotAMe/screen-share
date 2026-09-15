#pragma once
#include "ProofPeer.h"
#include "media/webrtc/ViewerStreamSettings.h"

namespace proofmedia {
struct MediaLink {
    Peer host, viewer;
    webrtc::scoped_refptr<SyntheticVideo> source = webrtc::make_ref_counted<SyntheticVideo>();
    webrtc::scoped_refptr<webrtc::RtpSenderInterface> sender;
};
std::unique_ptr<MediaLink> ConnectViewer(webrtc::PeerConnectionFactoryInterface& factory,
                                        webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio) {
    auto link = std::make_unique<MediaLink>();
    for (auto* peer : {&link->host, &link->viewer}) {
        webrtc::PeerConnectionInterface::RTCConfiguration config;
        config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
        config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
        auto result = factory.CreatePeerConnectionOrError(config, webrtc::PeerConnectionDependencies(peer));
        Require(result.ok(), "Multi-viewer connection creation failed");
        peer->connection = result.MoveValue();
    }
    for (const auto& label : {"control", "input-state", "telemetry"}) {
        webrtc::DataChannelInit init;
        if (std::string(label) != "control") { init.ordered = false; init.maxRetransmits = 0; }
        auto channel = link->host.connection->CreateDataChannelOrError(label, &init);
        Require(channel.ok(), "Multi-viewer channel creation failed");
        link->host.channels.push_back(std::make_unique<Channel>(channel.MoveValue()));
    }
    auto video = factory.CreateVideoTrack(link->source, "shared-capture-video");
    auto sender = link->host.connection->AddTrack(video, {"shared-stream"});
    Require(sender.ok(), "Multi-viewer video track failed");
    link->sender = sender.MoveValue();
    Require(link->host.connection->AddTrack(audio, {"shared-stream"}).ok(), "Multi-viewer audio track failed");
    TransferDescription(link->host, link->viewer, true);
    TransferDescription(link->viewer, link->host, false);
    Wait([&] {
        if (link->viewer.channels.size() != 3) return false;
        for (const auto& channel : link->host.channels)
            if (channel->channel->state() != webrtc::DataChannelInterface::kOpen) return false;
        return true;
    }, "Multi-viewer channels did not open");
    for (const auto& channel : link->host.channels)
        Require(channel->channel->Send(webrtc::DataBuffer("screenshare-proof:" + channel->channel->label())),
                "Multi-viewer channel send failed");
    Wait([&] {
        for (const auto& channel : link->viewer.channels) if (!channel->received) return false;
        return true;
    }, "Multi-viewer channel delivery failed");
    return link;
}
void RunMultiViewer() {
    using namespace screenshare::media;
    webrtc::AutoThread mainThread;
    // The source bridge must not erase queue/conversion age by stamping frames
    // at delivery, and must preserve each wrapper's independent dimensions.
    struct SourceSink : webrtc::VideoSinkInterface<webrtc::VideoFrame> {
        int width = 0, height = 0;
        int64_t timestamp = 0;
        void OnFrame(const webrtc::VideoFrame& frame) override {
            width = frame.width(); height = frame.height(); timestamp = frame.timestamp_us();
        }
    } sink;
    auto bridge = webrtc::make_ref_counted<CaptureVideoSource>();
    bridge->AddOrUpdateSink(&sink, webrtc::VideoSinkWants());
    SyntheticCaptureResource fixtureFrame;
    fixtureFrame.width = 320; fixtureFrame.height = 180; fixtureFrame.luma.resize(320 * 180, 80);
    bridge->Push(fixtureFrame, std::chrono::steady_clock::now() - std::chrono::milliseconds(100));
    bridge->RemoveSink(&sink);
    Require(sink.width == 320 && sink.height == 180 && webrtc::TimeMicros() - sink.timestamp >= 99000,
            "Video source lost dimensions or acquisition timestamp");
    Require(bridge->is_screencast() && bridge->needs_denoising() == false, "Incorrect screen source metadata");
    auto network = webrtc::Thread::CreateWithSocketServer();
    auto worker = webrtc::Thread::Create();
    Require(network->Start() && worker->Start(), "Multi-viewer threads failed");
    auto audioEvidence = std::make_shared<proof::AudioEvidence>();
    auto audioDiagnostics = std::make_shared<PcmAudioDiagnostics>();
    webrtc::PeerConnectionFactoryDependencies dependencies;
    dependencies.env = webrtc::CreateEnvironment();
    dependencies.network_thread = network.get();
    dependencies.worker_thread = worker.get();
    dependencies.signaling_thread = webrtc::Thread::Current();
    dependencies.adm = CreatePcmAudioDeviceModule(proof::SyntheticAudio(audioEvidence), audioDiagnostics);
    dependencies.audio_encoder_factory = webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus>();
    dependencies.audio_decoder_factory = webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus>();
    dependencies.video_encoder_factory = std::make_unique<MfVideoEncoderFactory>();
    dependencies.video_decoder_factory = std::make_unique<MfVideoDecoderFactory>();
    webrtc::EnableMedia(dependencies);
    auto factory = webrtc::CreateModularPeerConnectionFactory(std::move(dependencies));
    Require(factory != nullptr, "Multi-viewer factory failed");
    webrtc::AudioOptions options;
    options.echo_cancellation = options.auto_gain_control = options.noise_suppression = false;
    auto audioSource = factory->CreateAudioSource(options);
    auto audioTrack = factory->CreateAudioTrack("shared-audio", audioSource.get());
    std::array<std::unique_ptr<MediaLink>, 4> links;
    for (auto& link : links) link = ConnectViewer(*factory, audioTrack);
    std::atomic<bool> slow{true};
    CaptureDistributor distribution(91);
    for (size_t i = 0; i < links.size(); ++i) {
        distribution.Add(i + 1, [source = links[i]->source, &slow, i](auto sample) {
            if (i == 3 && slow) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            source->Push(*std::static_pointer_cast<SyntheticCaptureResource>(sample.resource), sample.capturedAt);
        });
    }
    CaptureSession capture(91, [] { return std::make_unique<SyntheticCaptureSource>(640, 360, 30); },
                           [&](auto sample) { distribution.Publish(std::move(sample)); });
    capture.EnableDelivery();
    auto healthy = [&](unsigned target) {
        for (size_t i = 0; i < 3; ++i) if (links[i]->viewer.decodedFrames < target) return false;
        return true;
    };
    Wait([&] { return healthy(60) && links[3]->viewer.decodedFrames >= 10; }, "Healthy viewers stalled beside slow viewer");
    const auto slowFrames = links[3]->viewer.decodedFrames.load();
    Require(distribution.stats(4).replaced > 10, "Slow viewer did not exercise bounded replacement");
    for (size_t i = 0; i < 3; ++i)
        Require(links[i]->viewer.decodedFrames > slowFrames * 2, "Slow source throttled a healthy connection");

    // Exercise one sender's supported limit independently, without adding an
    // application congestion controller or changing other sender parameters.
    ViewerStreamSettings settings;
    StreamPreferences preferences;
    preferences.resolution = ResolutionMode::Fixed;
    preferences.width = 640; preferences.height = 360; preferences.fps = 30;
    preferences.bitrateMode = SettingMode::Manual; preferences.bitrateLimitBps = 200000;
    Require(settings.Apply(*links[3]->sender, *links[3]->source, preferences, 1) == SettingsApplyError::None,
            "Per-viewer settings rejected");
    auto invalidPreferences = preferences; invalidPreferences.fps = 0;
    Require(settings.Apply(*links[3]->sender, *links[3]->source, invalidPreferences, 2) == SettingsApplyError::Invalid && settings.revision() == 1,
            "Invalid settings replaced the applied revision");
    Require(settings.Apply(*links[3]->sender, *links[3]->source, preferences, 1) == SettingsApplyError::StaleRevision,
            "Stale viewer settings were accepted");
    slow = false;
    const auto recoveredTarget = slowFrames + 30;
    Wait([&] { return links[3]->viewer.decodedFrames >= recoveredTarget && healthy(90); }, "Slow viewer did not recover");
    for (size_t i = 0; i < 3; ++i)
        Require(links[i]->sender->GetParameters().encodings[0].max_bitrate_bps != 200000,
                "One viewer's bitrate setting leaked to another");
    Require(links[3]->sender->GetParameters().encodings[0].max_bitrate_bps == 200000,
            "Per-viewer bitrate setting was lost");
    Require(!links[3]->sender->GetParameters().encodings[0].min_bitrate_bps &&
            links[3]->sender->GetParameters().degradation_preference == webrtc::DegradationPreference::MAINTAIN_FRAMERATE_AND_RESOLUTION &&
            links[3]->source->settingsStats().observedRevision == 1,
            "Manual settings imposed a bitrate floor, enabled adaptation or failed to reach the source");
    const auto replaced = distribution.stats(4).replaced;
    Require(links[3]->viewer.invalidFrames == 0, "Limited viewer changed dimensions or produced invalid pixels");

    // Tear down one complete pair, then attach a fresh connection/source while
    // the other three continue to receive the same capture session.
    distribution.Remove(4);
    links[3].reset();
    const auto beforeRejoin = links[0]->viewer.decodedFrames.load();
    links[3] = ConnectViewer(*factory, audioTrack);
    distribution.Add(4, [source = links[3]->source](auto sample) {
        source->Push(*std::static_pointer_cast<SyntheticCaptureResource>(sample.resource), sample.capturedAt);
    });
    Wait([&] { return links[3]->viewer.decodedFrames >= 30 && links[0]->viewer.decodedFrames >= beforeRejoin + 30; },
         "Viewer rejoin interrupted healthy media");
    capture.Stop(); distribution.Stop();
    Require(capture.status().state == CaptureState::Stopped, "Shared capture failed");
    Require(audioEvidence->audibleBlocks >= 30 && audioDiagnostics->captureErrors == 0 && audioDiagnostics->playoutErrors == 0,
            "Multi-viewer shared audio failed");
    std::cout << "{\"passed\":true,\"mode\":\"four-peer-headless\",\"viewers\":4,\"rejoins\":1,"
              << "\"slow_pending_replacements\":" << replaced << ",\"decoded_frames\":[";
    for (size_t i = 0; i < links.size(); ++i) {
        Require(links[i]->viewer.invalidFrames == 0 && !distribution.stats(i + 1).failed, "Viewer media failed");
        if (i) std::cout << ',';
        std::cout << links[i]->viewer.decodedFrames.load();
    }
    std::cout << "],\"limited_viewer_bps\":200000,\"audible_blocks\":" << audioEvidence->audibleBlocks.load() << "}\n";
}
}
