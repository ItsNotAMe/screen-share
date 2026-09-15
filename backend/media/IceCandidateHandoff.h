#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>

namespace screenshare::media {
struct IceCandidateMessage {
    std::string candidate, mid;
    int line = 0;
};
enum class IceHandoffError { None, Stale, Closed, Invalid, Capacity, Delivery };
// One connection/negotiation generation. All methods run on its signaling
// executor. The delivery callback must not reenter this object. Membership
// authorization and wire-schema validation belong to the room transport.
class IceCandidateHandoff final {
public:
    using Deliver = std::function<bool(const IceCandidateMessage&)>;
    IceCandidateHandoff(uint64_t generation, Deliver deliver)
        : generation_(generation), deliver_(std::move(deliver)) {
        if (!generation_ || !deliver_) error_ = IceHandoffError::Invalid;
    }
    IceHandoffError Push(uint64_t generation, IceCandidateMessage candidate) {
        if (generation != generation_) return IceHandoffError::Stale;
        if (error_ != IceHandoffError::None) return error_;
        if (candidate.candidate.size() > 4096 || candidate.mid.empty() || candidate.mid.size() > 64 ||
            candidate.line < 0 || candidate.line > 31 ||
            candidate.candidate.find('\0') != std::string::npos || candidate.mid.find('\0') != std::string::npos)
            return Fail(IceHandoffError::Invalid);
        // Bound the whole negotiation, not just the pre-description queue.
        if (accepted_ >= 64) return Fail(IceHandoffError::Capacity);
        ++accepted_;
        pending_.push_back(std::move(candidate));
        return Drain();
    }
    IceHandoffError LocalDescriptionReady(uint64_t generation) {
        if (generation != generation_) return IceHandoffError::Stale;
        localReady_ = true; return Drain();
    }
    IceHandoffError RemoteDescriptionReady(uint64_t generation) {
        if (generation != generation_) return IceHandoffError::Stale;
        remoteReady_ = true; return Drain();
    }
    void Close() { Fail(IceHandoffError::Closed); }
    IceHandoffError error() const { return error_; }
    size_t pending() const { return pending_.size(); }
    size_t delivered() const { return delivered_; }
private:
    IceHandoffError Fail(IceHandoffError error) {
        error_ = error; pending_.clear(); deliver_ = {}; return error_;
    }
    IceHandoffError Drain() {
        if (error_ != IceHandoffError::None) return error_;
        if (!localReady_ || !remoteReady_) return IceHandoffError::None;
        while (!pending_.empty()) {
            bool accepted = false;
            try { accepted = deliver_(pending_.front()); } catch (...) {}
            if (!accepted) return Fail(IceHandoffError::Delivery);
            pending_.pop_front(); ++delivered_;
        }
        return IceHandoffError::None;
    }
    uint64_t generation_;
    Deliver deliver_;
    std::deque<IceCandidateMessage> pending_;
    size_t accepted_ = 0, delivered_ = 0;
    bool localReady_ = false, remoteReady_ = false;
    IceHandoffError error_ = IceHandoffError::None;
};
}
