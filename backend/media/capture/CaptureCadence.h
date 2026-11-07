#pragma once
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace screenshare::media {
// Capture-owner cadence. Idle samples maintain source health/geometry; renewed
// viewer demand wakes immediately instead of waiting for the next idle slot.
class CaptureCadence {
public:
    using Clock = std::chrono::steady_clock;
    explicit CaptureCadence(int fps) {
        if (fps < 1 || fps > 240) throw std::invalid_argument("Invalid capture cadence");
        activePeriod_ = std::chrono::nanoseconds(1'000'000'000 / fps);
        idlePeriod_ = std::chrono::nanoseconds(1'000'000'000 / std::min(fps, 5));
    }
    bool Due(Clock::time_point now, bool active = true) {
        if (active && !active_) next_ = now;
        active_ = active;
        return now >= next_;
    }
    void Sampled(Clock::time_point now) {
        const auto period = active_ ? activePeriod_ : idlePeriod_;
        next_ += period;
        if (next_ <= now) next_ = now + period;
    }
    void Reset() { next_ = {}; }
private:
    Clock::time_point next_{};
    std::chrono::nanoseconds activePeriod_{}, idlePeriod_{};
    bool active_ = true;
};
}
