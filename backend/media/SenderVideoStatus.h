#pragma once
#include <cstdint>
#include <optional>
#include "ReceiverVideoStatus.h"

namespace screenshare::media {
enum class VideoLimitReason { Unknown, None, Cpu, Bandwidth, Other };
struct SenderVideoObservation {
    std::optional<uint64_t> payloadBps;
    std::optional<double> encodedFps, rttMs, jitterMs, lossFraction, availableOutgoingBps;
    VideoLimitReason limitingReason = VideoLimitReason::Unknown;
    CodecImplementation encoder = CodecImplementation::Unknown;
    std::optional<double> meanEncodeMs;
    std::optional<uint64_t> retransmittedPackets;
    std::optional<uint32_t> nackCount, pliCount;
};
}
