#pragma once
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace screenshare::media {
// Difference cumulative WebRTC counters on the receiver's monotonic clock.
// This measures buffering for emitted frames, not frame age or network latency.
class ReceiverBufferInterval {
public:
    using Clock = std::chrono::steady_clock;
    std::optional<uint32_t> Sample(const std::string& stream, double delaySeconds,
        uint64_t emitted, Clock::time_point now = Clock::now()) {
        if (stream.empty() || !std::isfinite(delaySeconds) || delaySeconds < 0) { Reset(); return {}; }
        std::optional<uint32_t> result;
        if (previous_ && stream == stream_ && now > at_ && now - at_ < std::chrono::seconds(3) &&
            delaySeconds >= delay_ && emitted > emitted_) {
            const double mean = (delaySeconds - delay_) * 1000 / (emitted - emitted_);
            if (std::isfinite(mean) && mean >= 0 && mean <= 60000) result = uint32_t(std::lround(mean));
        }
        stream_ = stream; delay_ = delaySeconds; emitted_ = emitted; at_ = now; previous_ = true;
        return result;
    }
    void Reset() { previous_ = false; stream_.clear(); }
private:
    bool previous_ = false;
    std::string stream_;
    double delay_ = 0;
    uint64_t emitted_ = 0;
    Clock::time_point at_{};
};
}
