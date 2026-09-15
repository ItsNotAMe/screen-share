#include "PeerNegotiation.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include <optional>
#include <utility>

namespace screenshare::media {
struct PeerNegotiation::State : std::enable_shared_from_this<State> {
    struct Pending {
        uint64_t operation;
        std::promise<NegotiationResult> reply;
        std::string sdp;
    };
    struct Ticket {
        uint64_t operation;
        bool accepted;
        std::future<NegotiationResult> reply;
    };
    class Created;
    class Applied;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> connection;
    uint64_t generation, nextOperation = 0;
    std::optional<Pending> pending;
    std::string username;
    bool closed = false;
    State(webrtc::scoped_refptr<webrtc::PeerConnectionInterface> value, uint64_t id)
        : connection(std::move(value)), generation(id) {}
    static bool ValidSdp(const std::string& sdp) {
        return !sdp.empty() && sdp.size() <= 60 * 1024 && sdp.find('\0') == std::string::npos;
    }
    bool Active(uint64_t operation) const { return !closed && pending && pending->operation == operation; }
    Ticket Begin(uint64_t requestedGeneration) {
        Pending request{++nextOperation, {}, {}};
        auto reply = request.reply.get_future();
        auto error = requestedGeneration != generation ? NegotiationError::StaleGeneration :
            (closed || !connection || !generation) ? NegotiationError::Closed :
            pending ? NegotiationError::Busy : NegotiationError::None;
        const auto operation = request.operation;
        if (error == NegotiationError::None) pending.emplace(std::move(request));
        else request.reply.set_value({operation, generation, error, {}});
        return {operation, error == NegotiationError::None, std::move(reply)};
    }
    void Complete(uint64_t operation, NegotiationError error) {
        if (!Active(operation)) return;
        auto request = std::move(*pending);
        pending.reset();
        request.reply.set_value({operation, generation, error,
                                error == NegotiationError::None ? std::move(request.sdp) : std::string{}});
    }
    void ApplyCreated(uint64_t operation, std::unique_ptr<webrtc::SessionDescriptionInterface> description);
};

class PeerNegotiation::State::Applied : public webrtc::SetSessionDescriptionObserver {
public:
    Applied(std::weak_ptr<State> state, uint64_t operation) : state_(std::move(state)), operation_(operation) {}
    void OnSuccess() override { if (auto state = state_.lock()) state->Complete(operation_, NegotiationError::None); }
    void OnFailure(webrtc::RTCError) override {
        if (auto state = state_.lock()) state->Complete(operation_, NegotiationError::ApplyFailed);
    }
private:
    std::weak_ptr<State> state_;
    uint64_t operation_;
};
class PeerNegotiation::State::Created : public webrtc::CreateSessionDescriptionObserver {
public:
    Created(std::weak_ptr<State> state, uint64_t operation) : state_(std::move(state)), operation_(operation) {}
    void OnSuccess(webrtc::SessionDescriptionInterface* value) override {
        std::unique_ptr<webrtc::SessionDescriptionInterface> description(value);
        if (auto state = state_.lock()) state->ApplyCreated(operation_, std::move(description));
    }
    void OnFailure(webrtc::RTCError) override {
        if (auto state = state_.lock()) state->Complete(operation_, NegotiationError::CreateFailed);
    }
private:
    std::weak_ptr<State> state_;
    uint64_t operation_;
};
void PeerNegotiation::State::ApplyCreated(uint64_t operation,
                                        std::unique_ptr<webrtc::SessionDescriptionInterface> description) {
    if (!Active(operation)) return;
    if (!description || !description->ToString(&pending->sdp) || !ValidSdp(pending->sdp)) {
        Complete(operation, NegotiationError::CreateFailed); return;
    }
    // Capture credentials before SetLocalDescription can emit ICE callbacks.
    // SDP delivery to signaling occurs only after local application succeeds.
    const auto start = pending->sdp.find("a=ice-ufrag:");
    const auto end = start == std::string::npos ? start : pending->sdp.find("\r\n", start);
    if (start == std::string::npos || end == std::string::npos || end <= start + 12) {
        Complete(operation, NegotiationError::CreateFailed); return;
    }
    username = pending->sdp.substr(start + 12, end - start - 12);
    auto observer = webrtc::make_ref_counted<Applied>(weak_from_this(), operation);
    connection->SetLocalDescription(observer.get(), description.release());
}
PeerNegotiation::PeerNegotiation(webrtc::scoped_refptr<webrtc::PeerConnectionInterface> connection, uint64_t generation)
    : state_(std::make_shared<State>(std::move(connection), generation)) {}
PeerNegotiation::~PeerNegotiation() { Close(); }
std::future<NegotiationResult> PeerNegotiation::CreateLocal(uint64_t generation, bool offer, bool iceRestart) {
    auto ticket = state_->Begin(generation);
    if (ticket.accepted) {
        if (!offer && iceRestart) state_->Complete(ticket.operation, NegotiationError::Invalid);
        else {
            state_->username.clear(); // Retire old ICE callbacks before creating the next local description.
            auto observer = webrtc::make_ref_counted<State::Created>(state_, ticket.operation);
            webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
            options.ice_restart = iceRestart;
            if (offer) state_->connection->CreateOffer(observer.get(), options);
            else state_->connection->CreateAnswer(observer.get(), options);
        }
    }
    return std::move(ticket.reply);
}
std::future<NegotiationResult> PeerNegotiation::ApplyRemote(uint64_t generation, bool offer, std::string sdp) {
    auto ticket = state_->Begin(generation);
    if (ticket.accepted) {
        auto description = State::ValidSdp(sdp) ? webrtc::CreateSessionDescription(
            offer ? webrtc::SdpType::kOffer : webrtc::SdpType::kAnswer, sdp) : nullptr;
        if (!description) state_->Complete(ticket.operation, NegotiationError::Invalid);
        else {
            auto observer = webrtc::make_ref_counted<State::Applied>(state_, ticket.operation);
            state_->connection->SetRemoteDescription(observer.get(), description.release());
        }
    }
    return std::move(ticket.reply);
}
const std::string& PeerNegotiation::localUsername() const { return state_->username; }
void PeerNegotiation::Close() {
    if (state_->closed) return;
    if (state_->pending) state_->Complete(state_->pending->operation, NegotiationError::Cancelled);
    state_->closed = true;
    state_->username.clear();
    state_->connection = nullptr;
}
}
