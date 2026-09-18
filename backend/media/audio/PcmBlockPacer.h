#pragma once
#include <chrono>
#include <algorithm>
#include "core/ShortWait.h"
#include <stop_token>

namespace screenshare::media {
// Device-free 10 ms pacing, with cancellation and no catch-up bursts.
class PcmBlockPacer {
    ShortWait timer_;
    std::chrono::steady_clock::time_point next_;
public:
    void Start() { next_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(10); }
    bool Wait(std::stop_token stop) {
        // At most one 10 ms block of cancellation latency. Unlike an ordinary
        // timed condition-variable wait, this cadence survives Windows occlusion.
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < next_) {
            const auto remaining = next_ - std::chrono::steady_clock::now();
            if (remaining > std::chrono::steady_clock::duration::zero())
                timer_.Wait(std::min(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining),
                                    std::chrono::nanoseconds(std::chrono::milliseconds(10))));
        }
        if (stop.stop_requested()) return false;
        next_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
        return true;
    }
};
}
