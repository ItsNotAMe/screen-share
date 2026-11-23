#pragma once
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace screenshare::media {
// Capture-owner cadence. Sample at the highest active viewer rate within the
// source cap. Keep up to five health samples per second even at very low rates.
class CaptureCadence {
public:
    using Clock = std::chrono::steady_clock;
    explicit CaptureCadence(int fps) : sourceFps_(fps), fps_(fps) {
        if (fps < 1 || fps > 240) throw std::invalid_argument("Invalid capture cadence");
    }
    bool Due(Clock::time_point now) { return Due(now, sourceFps_); }
    bool Due(Clock::time_point now, int requestedFps) {
        const bool active = requestedFps > 0;
        const auto fps = std::clamp(requestedFps, std::min(sourceFps_, 5), sourceFps_);
        if (fps > fps_ || (active && !active_)) next_ = now;
        else if (active && fps < fps_ && next_ != Clock::time_point{}) next_ += Period(fps) - Period(fps_);
        fps_ = fps;
        active_ = active;
        return now >= next_;
    }
    void Sampled(Clock::time_point now) {
        const auto period = Period(fps_);
        next_ += period;
        if (next_ <= now) next_ = now + period;
    }
    void Reset() { next_ = {}; }
private:
    static std::chrono::nanoseconds Period(int fps) { return std::chrono::nanoseconds(1'000'000'000 / fps); }
    const int sourceFps_;
    int fps_;
    Clock::time_point next_{};
    bool active_ = true;
};
}
