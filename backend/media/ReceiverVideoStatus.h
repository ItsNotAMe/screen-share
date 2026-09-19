#pragma once
#include <cstdint>
#include <optional>
#include <chrono>
#include <mutex>

namespace screenshare::media {
enum class CodecImplementation : uint8_t { Unknown, MfH264Software, MfH264Hardware };
inline const char* CodecImplementationName(CodecImplementation value) {
    switch (value) {
    case CodecImplementation::MfH264Software: return "mf-h264-software";
    case CodecImplementation::MfH264Hardware: return "mf-h264-hardware";
    default: return "unknown";
    }
}
struct ReceiverPresentationObservation {
    uint64_t presented = 0, dropped = 0;
    uint8_t queued = 0, outcome = 0; // PresentationOutcome vocabulary; no platform types on the wire.
};
// One replaceable frontend snapshot. No callbacks into a UI from signaling.
class PresentationTelemetry {
    mutable std::mutex mutex_;
    std::optional<ReceiverPresentationObservation> value_;
    std::chrono::steady_clock::time_point at_{};
public:
    using Clock = std::chrono::steady_clock;
    void Publish(ReceiverPresentationObservation value, Clock::time_point now = Clock::now()) {
        if (value.queued > 1 || value.outcome > 7 || value.presented > INT64_MAX || value.dropped > INT64_MAX) return;
        std::lock_guard lock(mutex_); value_ = value; at_ = now;
    }
    std::optional<ReceiverPresentationObservation> Read(Clock::time_point now = Clock::now()) const {
        std::lock_guard lock(mutex_);
        return now - at_ < std::chrono::seconds(3) ? value_ : std::nullopt;
    }
};
// Receiver-reported decoder statistics. Never a presentation acknowledgement.
struct ReceiverVideoObservation {
    uint32_t width = 0, height = 0, framesDecoded = 0;
    std::optional<uint32_t> fpsMilli;
    std::optional<ReceiverPresentationObservation> presentation;
    std::optional<uint32_t> decoderDrops, jitterBufferMeanMs;
    CodecImplementation decoder = CodecImplementation::Unknown;
    std::optional<uint32_t> jitterBufferRecentMs; // Mean over the last fresh stats interval; not end-to-end latency.
};
struct ReceiverVideoStatus {
    std::optional<ReceiverVideoObservation> observation;
    bool stale = false;
    std::optional<uint32_t> ageSeconds;
};
}
