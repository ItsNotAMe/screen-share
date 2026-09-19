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
    // WebRTC's encoder target and completed-packet delay; neither is a queue-age
    // bound or an end-to-end latency measurement.
    std::optional<double> targetVideoBps, meanPacketSendDelayMs;
    std::optional<uint32_t> framesEncoded, keyFramesEncoded;
};
}
