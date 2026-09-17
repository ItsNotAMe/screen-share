#pragma once
#include "PcmAudioEndpoint.h"
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace screenshare::media {
// Device-free source for an audio track that can resume without renegotiation.
// Pace each block from the actual wake time: a delayed worker never catches up
// by emitting a burst of old silence. Cancellation also interrupts the wait.
class SilentPcmCapture final : public PcmCaptureEndpoint {
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::chrono::steady_clock::time_point next_;
public:
    void Start() override { next_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(10); }
    bool Read(PcmBlock& block, std::stop_token stop) override {
        std::unique_lock lock(mutex_);
        wake_.wait_until(lock, stop, next_, [] { return false; });
        if (stop.stop_requested()) return false;
        next_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
        block.fill(0);
        return true;
    }
    uint32_t DelayMs() const override { return 0; }
};
}
