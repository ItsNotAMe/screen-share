#include "media/webrtc/MediaPeer.h"
#include "api/rtp_transceiver_interface.h"
#include <algorithm>
#include <stdexcept>

namespace screenshare::media {
MediaPeer::MediaPeer(MediaEngine& engine, uint64_t generation,
    webrtc::VideoSinkInterface<webrtc::VideoFrame>* frames, ChannelReady channelReady,
    webrtc::PeerConnectionInterface::RTCConfiguration config, std::shared_ptr<DiagnosticHistory> diagnostics)
    : lifecycle(generation, PeerConnectionLifecycle::Clock::now()),
      signaling_(webrtc::Thread::Current()), frames_(frames),
      channelReady_(std::move(channelReady)), diagnostics_(diagnostics ? std::move(diagnostics) : std::make_shared<DiagnosticHistory>()) {
    if (!generation) throw std::invalid_argument("Peer generation must be nonzero");
    observed_.numbers["iceServerCount"] = double(config.servers.size());
    observed_.labels["icePolicy"] = config.type == webrtc::PeerConnectionInterface::kRelay ? "relay-only" : "all";
    unsigned stun = 0, turn = 0;
    for (const auto& server : config.servers) for (const auto& url : server.urls) {
        stun += url.starts_with("stun:") || url.starts_with("stuns:");
        turn += url.starts_with("turn:") || url.starts_with("turns:");
    }
    observed_.numbers["stunUrls"] = stun; observed_.numbers["turnUrls"] = turn;
    diagnostics_->Event("peer-create", generation);
    connection = engine.CreatePeer(*this, std::move(config));
    try { negotiation_ = std::make_unique<PeerNegotiation>(connection, generation); }
    catch (...) { connection->Close(); throw; }
}
MediaPeer::~MediaPeer() { Close(); }
void MediaPeer::Close() noexcept {
    if (webrtc::Thread::Current() != signaling_) std::terminate();
    if (closed_) return;
    closed_ = true;
    diagnostics_->Event("peer-close");
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
    observed_.labels["iceState"] = std::string(webrtc::PeerConnectionInterface::AsString(state));
    diagnostics_->Add({0, {{"code", int(state)}}, {{"event", "ice-state"}, {"state", observed_.labels["iceState"]}}});
    // A successful ICE restart can leave the aggregate transport connected
    // throughout, so no second OnConnectionChange(kConnected) is guaranteed.
    // Require the secure transport too before releasing the recovery gate.
    if ((state == webrtc::PeerConnectionInterface::kIceConnectionConnected ||
         state == webrtc::PeerConnectionInterface::kIceConnectionCompleted) &&
        connection && connection->peer_connection_state() ==
            webrtc::PeerConnectionInterface::PeerConnectionState::kConnected)
        lifecycle.Connected(lifecycle.generation(), PeerConnectionLifecycle::Clock::now());
}
void MediaPeer::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) {
    if (closed_) return;
    using State = webrtc::PeerConnectionInterface::PeerConnectionState;
    observed_.labels["connectionState"] = std::string(webrtc::PeerConnectionInterface::AsString(state));
    diagnostics_->Add({0, {{"code", int(state)}}, {{"event", "connection-state"}, {"state", observed_.labels["connectionState"]}}});
    // ICE alone does not establish DTLS/SRTP. Keep the startup deadline until
    // the complete transport is connected, and recover on DTLS failure too.
    if (state == State::kConnected)
        lifecycle.Connected(lifecycle.generation(), PeerConnectionLifecycle::Clock::now());
    else if (state == State::kDisconnected || state == State::kFailed)
        lifecycle.Disconnected(lifecycle.generation(), PeerConnectionLifecycle::Clock::now());
    else if (state == State::kClosed) lifecycle.RemoteClosed(lifecycle.generation());
}
void MediaPeer::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState state) {
    if (closed_) return;
    observed_.labels["signalingState"] = std::string(webrtc::PeerConnectionInterface::AsString(state));
    diagnostics_->Add({0, {{"code", int(state)}}, {{"event", "signaling-state"}, {"state", observed_.labels["signalingState"]}}});
}
void MediaPeer::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) {
    if (closed_) return;
    observed_.labels["gatheringState"] = std::string(webrtc::PeerConnectionInterface::AsString(state));
    diagnostics_->Add({0, {{"code", int(state)}}, {{"event", "gathering-state"}, {"state", observed_.labels["gatheringState"]}}});
}
void MediaPeer::OnIceCandidateError(const std::string&, int, const std::string& url, int code, const std::string&) {
    if (closed_) return;
    diagnostics_->Event(url.starts_with("turn") ? "turn-error" : "stun-error", code);
    observed_.numbers["lastIceError"] = code;
}
DiagnosticRecord MediaPeer::Diagnostics() const {
    auto value = observed_;
    for (const auto& channel : channels_) {
        // Only our three validated channel labels can enter the report.
        value.numbers[channel->label() + "State"] = int(channel->state());
        value.numbers[channel->label() + "BufferedBytes"] = double(channel->buffered_amount());
    }
    return value;
}
void MediaPeer::OnIceCandidate(const webrtc::IceCandidate* candidate) {
    if (!closed_ && candidate) {
        const auto type = std::string(webrtc::IceCandidateTypeToString(candidate->candidate().type()));
        ++observed_.numbers["localCandidate_" + type];
    }
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
