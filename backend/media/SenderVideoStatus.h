#pragma once
#include <cstdint>
#include <optional>

namespace screenshare::media {
enum class VideoLimitReason { Unknown, None, Cpu, Bandwidth, Other };
struct SenderVideoObservation {
    std::optional<uint64_t> payloadBps;
    std::optional<double> encodedFps, rttMs, jitterMs, lossFraction, availableOutgoingBps;
    VideoLimitReason limitingReason = VideoLimitReason::Unknown;
};
}
