#include "RoomPeerNegotiation.h"
#include "api/jsep.h"
#include <algorithm>

namespace screenshare::media {
RoomPeerNegotiation::RoomPeerNegotiation(webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer,
    PeerNegotiation& negotiation, uint64_t generation, bool host, Send send)
    : peer_(std::move(peer)), negotiation_(negotiation), peerGeneration_(generation), host_(host), send_(std::move(send)) {}
RoomPeerNegotiation::~RoomPeerNegotiation() { Close(); }
bool RoomPeerNegotiation::ready() const { return stage_ == Stage::Ready; }
bool RoomPeerNegotiation::Fail() { Close(); return false; }
void RoomPeerNegotiation::Close() {
    if (stage_ == Stage::Closed) return;
    stage_ = Stage::Closed;
    if (incoming_) incoming_->Close();
    if (outgoing_) outgoing_->Close();
    negotiation_.Close(); peer_->Close(); send_ = {};
}
bool RoomPeerNegotiation::Begin(std::string id) {
    if (stage_ != Stage::Idle && stage_ != Stage::Ready) return false;
    if (id.empty() || id.size() > 128 || id == id_ || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; })) return false;
    if (incoming_) incoming_->Close();
    if (outgoing_) outgoing_->Close();
    id_ = std::move(id); ++iceGeneration_;
    deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    incoming_ = std::make_shared<IceCandidateHandoff>(iceGeneration_, [this](const auto& ice) {
        std::unique_ptr<webrtc::IceCandidate> value(webrtc::CreateIceCandidate(ice.mid, ice.line, ice.candidate, nullptr));
        return value && peer_->AddIceCandidate(value.get());
    });
    outgoing_ = std::make_shared<IceCandidateHandoff>(iceGeneration_, [this](const auto& ice) {
        return send_({RoomPeerSignal::Kind::Candidate, id_, {}, ice});
    });
    incoming_->LocalDescriptionReady(iceGeneration_); // Only remote apply gates native candidate insertion.
    outgoing_->RemoteDescriptionReady(iceGeneration_); // Only local SDP send gates outbound trickle.
    return true;
}
bool RoomPeerNegotiation::Offer(std::string id, bool restart) {
    if (!host_ || !Begin(std::move(id))) return false;
    pending_ = negotiation_.CreateLocal(peerGeneration_, true, restart);
    stage_ = Stage::CreatingOffer; return true;
}
bool RoomPeerNegotiation::Receive(RoomPeerSignal message) {
    if (stage_ == Stage::Closed) return false;
    if (message.kind == RoomPeerSignal::Kind::Offer) {
        if (host_ || !Begin(std::move(message.connectionId))) return false;
        pending_ = negotiation_.ApplyRemote(peerGeneration_, true, std::move(message.sdp));
        stage_ = Stage::ApplyingOffer; return true;
    }
    if (message.connectionId != id_) return true; // Retired ICE/answers never reach native state.
    if (message.kind == RoomPeerSignal::Kind::Candidate)
        return incoming_ && (incoming_->Push(iceGeneration_, std::move(message.ice)) == IceHandoffError::None || Fail());
    if (message.kind == RoomPeerSignal::Kind::Answer && host_ && stage_ == Stage::AwaitAnswer) {
        pending_ = negotiation_.ApplyRemote(peerGeneration_, false, std::move(message.sdp));
        stage_ = Stage::ApplyingAnswer; return true;
    }
    // Restart requests are handled by the host lifecycle/recovery budget.
    return false;
}
void RoomPeerNegotiation::LocalCandidate(const webrtc::IceCandidate* candidate) {
    if (!outgoing_ || stage_ == Stage::Closed || stage_ == Stage::ApplyingOffer || candidate->candidate().username() != negotiation_.localUsername()) return;
    IceCandidateMessage message;
    if (!candidate->ToString(&message.candidate)) { Fail(); return; }
    message.mid = candidate->sdp_mid(); message.line = candidate->sdp_mline_index();
    if (outgoing_->Push(iceGeneration_, std::move(message)) != IceHandoffError::None) Fail();
}
bool RoomPeerNegotiation::Poll() {
    if (stage_ == Stage::Closed) return false;
    if (stage_ == Stage::Idle || stage_ == Stage::Ready) return true;
    if (std::chrono::steady_clock::now() >= deadline_) return Fail();
    if (!pending_.valid() || pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return true;
    auto result = pending_.get();
    if (result.error != NegotiationError::None) return Fail();
    if (stage_ == Stage::ApplyingOffer || stage_ == Stage::ApplyingAnswer) {
        if (incoming_->RemoteDescriptionReady(iceGeneration_) != IceHandoffError::None) return Fail();
        if (stage_ == Stage::ApplyingAnswer) { stage_ = Stage::Ready; return true; }
        pending_ = negotiation_.CreateLocal(peerGeneration_, false); stage_ = Stage::CreatingAnswer; return true;
    }
    const bool offer = stage_ == Stage::CreatingOffer;
    if (!send_({offer ? RoomPeerSignal::Kind::Offer : RoomPeerSignal::Kind::Answer, id_, std::move(result.sdp), {}})) return Fail();
    stage_ = offer ? Stage::AwaitAnswer : Stage::Ready;
    return outgoing_->LocalDescriptionReady(iceGeneration_) == IceHandoffError::None || Fail();
}
}
