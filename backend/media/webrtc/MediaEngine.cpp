#include "media/webrtc/MediaEngine.h"
#include "api/audio_codecs/audio_decoder_factory_template.h"
#include "api/audio_codecs/audio_encoder_factory_template.h"
#include "api/audio_codecs/opus/audio_decoder_opus.h"
#include "api/audio_codecs/opus/audio_encoder_opus.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/enable_media.h"
#include "api/environment/environment_factory.h"
#include "api/rtc_event_log/rtc_event_log_factory.h"
#include <stdexcept>

namespace screenshare::media {
MediaEngine::MediaEngine(webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio,
    std::unique_ptr<webrtc::VideoEncoderFactory> encoder,
    std::unique_ptr<webrtc::VideoDecoderFactory> decoder, PacketFactory packetFactory, EventLogFactory eventLogFactory)
    : signaling_(webrtc::Thread::Current()),
      network_(webrtc::Thread::CreateWithSocketServer()), worker_(webrtc::Thread::Create()),
      eventLogFactory_(std::move(eventLogFactory)) {
    if (!signaling_ || !audio || !encoder || !decoder)
        throw std::invalid_argument("Media engine requires signaling and media dependencies");
    if (!network_->Start() || !worker_->Start())
        throw std::runtime_error("Media engine threads failed");
    webrtc::PeerConnectionFactoryDependencies dependencies;
    dependencies.env = webrtc::CreateEnvironment();
    if (eventLogFactory_) dependencies.event_log_factory = std::make_unique<webrtc::RtcEventLogFactory>();
    dependencies.network_thread = network_.get();
    dependencies.worker_thread = worker_.get();
    dependencies.signaling_thread = signaling_;
    dependencies.adm = std::move(audio);
    dependencies.audio_encoder_factory = webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus>();
    dependencies.audio_decoder_factory = webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus>();
    dependencies.video_encoder_factory = std::move(encoder);
    dependencies.video_decoder_factory = std::move(decoder);
    if (packetFactory) {
        dependencies.packet_socket_factory = network_->BlockingCall([&] { return packetFactory(network_->socketserver()); });
        if (!dependencies.packet_socket_factory) throw std::invalid_argument("Missing packet socket factory");
    }
    webrtc::EnableMedia(dependencies);
    factory_ = webrtc::CreateModularPeerConnectionFactory(std::move(dependencies));
    if (!factory_) throw std::runtime_error("Media factory failed");
}
void MediaEngine::CheckThread() const {
    if (webrtc::Thread::Current() != signaling_)
        throw std::logic_error("Media engine requires its signaling executor");
}
MediaEngine::~MediaEngine() {
    if (webrtc::Thread::Current() != signaling_) std::terminate();
    factory_ = nullptr;
    worker_->Stop();
    network_->Stop();
}
webrtc::scoped_refptr<webrtc::PeerConnectionInterface> MediaEngine::CreatePeer(
    webrtc::PeerConnectionObserver& observer, webrtc::PeerConnectionInterface::RTCConfiguration config) {
    CheckThread();
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
    auto result = factory_->CreatePeerConnectionOrError(config, webrtc::PeerConnectionDependencies(&observer));
    if (!result.ok()) throw std::runtime_error(result.error().message());
    auto peer = result.MoveValue();
    if (eventLogFactory_) {
        try {
            auto output = eventLogFactory_();
            if (!output || !output->IsActive() || !peer->StartRtcEventLog(std::move(output), 100))
                throw std::runtime_error("Requested RTC event logging unavailable");
        } catch (...) {
            peer->Close();
            throw;
        }
    }
    return peer;
}
webrtc::scoped_refptr<webrtc::AudioTrackInterface> MediaEngine::CreateAudioTrack() {
    CheckThread();
    webrtc::AudioOptions options;
    options.echo_cancellation = options.auto_gain_control = options.noise_suppression = false;
    auto source = factory_->CreateAudioSource(options);
    auto track = factory_->CreateAudioTrack("room-audio", source.get());
    if (!track) throw std::runtime_error("Audio track creation failed");
    return track;
}
webrtc::scoped_refptr<webrtc::RtpSenderInterface> MediaEngine::AttachHostMedia(
    webrtc::PeerConnectionInterface& peer, webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> video,
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio) {
    CheckThread();
    if (!video || !audio) throw std::invalid_argument("Host media requires video and audio");
    auto track = factory_->CreateVideoTrack(video, "shared-capture-video");
    auto sender = peer.AddTrack(track, {"shared-stream"});
    if (!sender.ok()) throw std::runtime_error(sender.error().message());
    auto result = sender.MoveValue();
    auto audioSender = peer.AddTrack(audio, {"shared-stream"});
    if (!audioSender.ok()) {
        if (!peer.RemoveTrackOrError(result).ok()) peer.Close();
        throw std::runtime_error(audioSender.error().message());
    }
    return result;
}
std::array<webrtc::scoped_refptr<webrtc::DataChannelInterface>, 3>
MediaEngine::CreateHostChannels(webrtc::PeerConnectionInterface& peer) {
    CheckThread();
    std::array<webrtc::scoped_refptr<webrtc::DataChannelInterface>, 3> channels;
    const char* labels[] = {"control", "input-state", "telemetry"};
    for (size_t i = 0; i < channels.size(); ++i) {
        webrtc::DataChannelInit options;
        if (i) { options.ordered = false; options.maxRetransmits = 0; }
        auto result = peer.CreateDataChannelOrError(labels[i], &options);
        if (!result.ok()) {
            for (auto& channel : channels) if (channel) channel->Close();
            throw std::runtime_error(result.error().message());
        }
        channels[i] = result.MoveValue();
    }
    return channels;
}
}
