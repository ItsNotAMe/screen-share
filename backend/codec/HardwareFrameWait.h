#pragma once
#include "codec/H264StreamEncoder.h"
#include "core/ShortWait.h"
#include <chrono>
#include <stdexcept>

namespace screenshare {
class HardwareFrameCancelled : public std::runtime_error {
public: HardwareFrameCancelled() : std::runtime_error("Hardware frame cancelled") {}
};
class HardwareFrameTimeout : public std::runtime_error {
public: HardwareFrameTimeout() : std::runtime_error("Hardware frame exceeded 500 ms output deadline") {}
};

// No internal input queue: one submission is outstanding during this operation.
// Poll must be nonblocking; callbacks make missing-output/cancellation testable
// without depending on a broken GPU driver. MF calls themselves cannot be preempted.
template<class Poll, class Submit, class Cancelled>
EncodedPacket WaitForHardwareFrame(int64_t timestamp, Poll poll, Submit submit, Cancelled cancelled) {
    thread_local ShortWait timer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    bool submitted = false;
    for (;;) {
        if (cancelled()) throw HardwareFrameCancelled();
        if (std::chrono::steady_clock::now() >= deadline) throw HardwareFrameTimeout();
        auto packets = poll();
        if (cancelled()) throw HardwareFrameCancelled();
        if (std::chrono::steady_clock::now() >= deadline) throw HardwareFrameTimeout();
        if (!packets.empty()) {
            if (!submitted || packets.size() != 1 || packets.front().timestamp100ns != timestamp)
                throw std::runtime_error("Hardware output lost submission association");
            return std::move(packets.front());
        }
        if (!submitted) submitted = submit();
        timer.Wait();
    }
}
}
