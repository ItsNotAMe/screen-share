#pragma once
#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace screenshare::media {
// Capture-owner thread only. No sleeping, frame retention or driver retries in
// this policy. Stop can interrupt the caller's backoff immediately.
class CaptureRecovery {
public:
    using Clock = std::chrono::steady_clock;
    template<class Retire>
    void Lost(Retire&& retire, Clock::time_point now = Clock::now()) {
        retire(); // Invalidate even the terminal generation before returning.
        if (terminal_ || pending_ || attempts_ == 3) {
            terminal_ = true;
            throw std::runtime_error("Capture recovery exhausted");
        }
        ++attempts_;
        pending_ = true;
        readyAt_ = now + std::chrono::milliseconds(250);
    }
    template<class Rebuild>
    bool Poll(Rebuild&& rebuild, Clock::time_point now = Clock::now()) {
        if (terminal_) throw std::runtime_error("Capture recovery is terminal");
        if (!pending_) return true;
        if (now < readyAt_) return false;
        try { rebuild(); } // Reconstruction errors are terminal, not retried in a loop.
        catch (...) { terminal_ = true; throw; }
        pending_ = false;
        ++generation_;
        return true;
    }
    uint64_t generation() const noexcept { return generation_; }
private:
    Clock::time_point readyAt_{};
    unsigned attempts_ = 0;
    uint64_t generation_ = 1;
    bool pending_ = false;
    bool terminal_ = false;
};
}
