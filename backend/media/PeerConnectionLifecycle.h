#pragma once
#include <chrono>
#include <cstdint>
#include <deque>

namespace screenshare::media {
enum class PeerLifecycleState { Connecting, Connected, Backoff, Restarting, Failed, Closed };
enum class PeerLifecycleFailure { None, DirectConnectTimeout, RestartLimit, RemoteClosed };
enum class PeerLifecycleAction { None, RestartIce, Close };
// One peer incarnation, called only on its control/signaling executor. Time is
// supplied so deadline/rate-limit tests need no sleeps. Replacing the connection
// requires a new instance and generation; an ICE restart retains this instance.
class PeerConnectionLifecycle final {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    PeerConnectionLifecycle(uint64_t generation, Time now)
        : generation_(generation), due_(now + std::chrono::seconds(20)) {}
    bool Connected(uint64_t generation, Time now) {
        if (!Accept(generation)) return false;
        // Late success cannot revive an expired initial connection.
        if (state_ == PeerLifecycleState::Connecting && now >= due_) {
            Fail(PeerLifecycleFailure::DirectConnectTimeout); return false;
        }
        state_ = PeerLifecycleState::Connected;
        return true;
    }
    bool Disconnected(uint64_t generation, Time now) {
        if (!Accept(generation)) return false;
        // Repeated notifications never postpone an existing deadline/backoff.
        if (state_ == PeerLifecycleState::Connected) Schedule(now);
        return true;
    }
    bool RequestRestart(uint64_t generation, Time now) { return Disconnected(generation, now); }
    bool RemoteClosed(uint64_t generation) {
        if (!Accept(generation)) return false;
        Fail(PeerLifecycleFailure::RemoteClosed); return true;
    }
    void Close() { state_ = PeerLifecycleState::Closed; }
    PeerLifecycleAction Tick(Time now) {
        if (state_ == PeerLifecycleState::Failed) return PeerLifecycleAction::Close;
        if (state_ == PeerLifecycleState::Closed || state_ == PeerLifecycleState::Connected || now < due_)
            return PeerLifecycleAction::None;
        if (state_ == PeerLifecycleState::Connecting) {
            Fail(PeerLifecycleFailure::DirectConnectTimeout); return PeerLifecycleAction::Close;
        }
        if (state_ == PeerLifecycleState::Restarting) {
            Schedule(now);
            return state_ == PeerLifecycleState::Failed ? PeerLifecycleAction::Close : PeerLifecycleAction::None;
        }
        Prune(now);
        if (attempts_.size() >= 3) {
            Fail(PeerLifecycleFailure::RestartLimit); return PeerLifecycleAction::Close;
        }
        attempts_.push_back(now); ++restartRevision_;
        state_ = PeerLifecycleState::Restarting;
        due_ = now + std::chrono::seconds(20);
        return PeerLifecycleAction::RestartIce;
    }
    PeerLifecycleState state() const { return state_; }
    PeerLifecycleFailure failure() const { return failure_; }
    uint64_t generation() const { return generation_; }
    uint64_t restartRevision() const { return restartRevision_; }
private:
    bool Accept(uint64_t generation) const {
        return generation == generation_ && state_ != PeerLifecycleState::Closed && state_ != PeerLifecycleState::Failed;
    }
    void Prune(Time now) {
        while (!attempts_.empty() && now - attempts_.front() >= std::chrono::minutes(1)) attempts_.pop_front();
    }
    void Schedule(Time now) {
        Prune(now);
        if (attempts_.size() >= 3) { Fail(PeerLifecycleFailure::RestartLimit); return; }
        state_ = PeerLifecycleState::Backoff;
        // Initial defaults: 500 ms grace, then 1/2 second backoff. A successful
        // reconnect does not reset the rolling per-peer attempt budget.
        due_ = now + std::chrono::milliseconds(500 * (1 << attempts_.size()));
    }
    void Fail(PeerLifecycleFailure failure) { failure_ = failure; state_ = PeerLifecycleState::Failed; }
    uint64_t generation_, restartRevision_ = 0;
    Time due_;
    std::deque<Time> attempts_;
    PeerLifecycleState state_ = PeerLifecycleState::Connecting;
    PeerLifecycleFailure failure_ = PeerLifecycleFailure::None;
};
}
