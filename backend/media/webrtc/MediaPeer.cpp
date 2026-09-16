#include "media/webrtc/MediaPeer.h"
#include "api/rtp_transceiver_interface.h"
#include <algorithm>
#include <stdexcept>

namespace screenshare::media {
MediaPeer::MediaPeer(MediaEngine& engine, uint64_t generation,
    webrtc::VideoSinkInterface<webrtc::VideoFrame>* frames, ChannelReady channelReady,
    webrtc::PeerConnectionInterface::RTCConfiguration config)
    : lifecycle(generation, PeerConnectionLifecycle::Clock::now()),
      signaling_(webrtc::Thread::Current()), frames_(frames),
      channelReady_(std::move(channelReady)) {
    if (!generation) throw std::invalid_argument("Peer generation must be nonzero");
    connection = engine.CreatePeer(*this, std::move(config));
    try { negotiation_ = std::make_unique<PeerNegotiation>(connection, generation); }
    catch (...) { connection->Close(); throw; }
}
MediaPeer::~MediaPeer() { Close(); }
void MediaPeer::Close() noexcept {
    if (webrtc::Thread::Current() != signaling_) std::terminate();
    if (closed_) return;
    closed_ = true;
    lifecycle.Close();
    candidateObserver = {};
    channelReady_ = {};
    if (negotiation_) negotiation_->Close();
    if (video_ && frames_) video_->RemoveSink(frames_);
    video_ = nullptr;
    for (auto& channel : channels_) channel->Close();
    channels_.clear();
    if (connection) connection->Close();
}
void MediaPeer::OpenHostChannels(MediaEngine& engine) {
    if (webrtc::Thread::Current() != signaling_)
        throw std::logic_error("Peer requires its signaling executor");
    if (closed_ || !channels_.empty()) throw std::logic_error("Peer channels already initialized or closed");
    for (auto& channel : engine.CreateHostChannels(*connection)) OnDataChannel(std::move(channel));
}
void MediaPeer::OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state) {
    if (closed_) return;
    using Peer = webrtc::PeerConnectionInterface;
    if (state == Peer::kIceConnectionConnected || state == Peer::kIceConnectionCompleted)
        lifecycle.Connected(lifecycle.generation(), PeerConnectionLifecycle::Clock::now());
    else if (state == Peer::kIceConnectionDisconnected || state == Peer::kIceConnectionFailed)
        lifecycle.Disconnected(lifecycle.generation(), PeerConnectionLifecycle::Clock::now());
    else if (state == Peer::kIceConnectionClosed) lifecycle.RemoteClosed(lifecycle.generation());
}
void MediaPeer::OnIceCandidate(const webrtc::IceCandidate* candidate) {
    if (!closed_ && candidateObserver) candidateObserver(candidate);
}
void MediaPeer::OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
    if (closed_ || !frames_) return;
    auto track = transceiver->receiver()->track();
    if (!track || track->kind() != webrtc::MediaStreamTrackInterface::kVideoKind) return;
    // A replacement track cannot leave an old decoder feeding the presentation sink.
    if (video_) video_->RemoveSink(frames_);
    video_ = static_cast<webrtc::VideoTrackInterface*>(track.get());
    video_->AddOrUpdateSink(frames_, webrtc::VideoSinkWants());
}
void MediaPeer::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
    const auto label = channel->label();
    const bool control = label == "control";
    const bool transient = label == "input-state" || label == "telemetry";
    const bool policy = control ? channel->ordered() && !channel->maxRetransmitsOpt()
        && !channel->maxPacketLifeTime() : transient && !channel->ordered()
        && channel->maxRetransmitsOpt() == 0 && !channel->maxPacketLifeTime();
    if (closed_ || !policy || channels_.size() >= 3 || std::any_of(channels_.begin(), channels_.end(),
        [&](const auto& existing) { return existing->label() == label; })) {
        channel->Close();
        return;
    }
    channels_.push_back(channel);
    if (channelReady_) channelReady_(std::move(channel));
}
}
