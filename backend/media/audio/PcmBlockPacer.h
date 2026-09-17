#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace screenshare::media {
// Device-free 10 ms pacing, with cancellation and no catch-up bursts.
class PcmBlockPacer {
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::chrono::steady_clock::time_point next_;
public:
    void Start() { next_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(10); }
    bool Wait(std::stop_token stop) {
        std::unique_lock lock(mutex_);
        wake_.wait_until(lock, stop, next_, [] { return false; });
        if (stop.stop_requested()) return false;
        next_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
        return true;
    }
};
}
