#pragma once
#include "PeerNegotiation.h"
#include "media/IceCandidateHandoff.h"
#include "media/RoomPeerSignal.h"
#include <chrono>
#include <functional>

namespace screenshare::media {
// Private media boundary. All calls/callbacks run on signaling. Transport must
// authenticate identities before Receive and enqueue Send without blocking.
// Negotiation completions/deadlines advance on the signaling thread automatically.
// Send must not synchronously destroy/reenter this adapter.
class RoomPeerNegotiation final {
public:
    using Send = std::function<bool(RoomPeerSignal)>;
    RoomPeerNegotiation(webrtc::scoped_refptr<webrtc::PeerConnectionInterface>, PeerNegotiation&, uint64_t peerGeneration, bool host, Send);
    ~RoomPeerNegotiation();
    RoomPeerNegotiation(const RoomPeerNegotiation&) = delete;
    RoomPeerNegotiation& operator=(const RoomPeerNegotiation&) = delete;
    bool Offer(std::string freshConnectionId, bool restart = false);
    bool Receive(RoomPeerSignal);
    void LocalCandidate(const webrtc::IceCandidate*);
    void Close();
    bool ready() const;
    bool closed() const { return stage_ == Stage::Closed; }
    const std::string& connectionId() const { return id_; }
private:
    enum class Stage { Idle, CreatingOffer, AwaitAnswer, ApplyingOffer, CreatingAnswer, ApplyingAnswer, Ready, Closed };
    bool Begin(std::string);
    bool Fail();
    bool Poll();
    void Schedule();
    std::shared_ptr<int> scheduleLifetime_;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_;
    PeerNegotiation& negotiation_;
    uint64_t peerGeneration_, iceGeneration_ = 0;
    bool host_;
    Send send_;
    Stage stage_ = Stage::Idle;
    std::string id_;
    std::future<NegotiationResult> pending_;
    std::shared_ptr<IceCandidateHandoff> incoming_, outgoing_;
    std::chrono::steady_clock::time_point deadline_;
};
}
