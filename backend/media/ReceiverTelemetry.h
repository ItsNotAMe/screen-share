#pragma once
#include "ReceiverVideoStatus.h"
#include <chrono>
#include <algorithm>
#include <span>
#include <string>
#include <vector>

namespace screenshare::media {
// Network byte order. V1: 26 + ID bytes. V2: 53 + ID bytes. V3: 57 + ID bytes (max 185).
// No clocks, names, addresses, SDP or credentials cross this telemetry boundary.
struct ReceiverTelemetryMessage {
    std::string connection;
    uint64_t sequence = 0;
    ReceiverVideoObservation video;
};
inline bool ValidReceiverVideo(const ReceiverVideoObservation& value) {
    return value.width >= 2 && value.width <= 3840 && value.height >= 2 && value.height <= 2160 &&
        value.width % 2 == 0 && value.height % 2 == 0 && (!value.fpsMilli || *value.fpsMilli <= 240000) &&
        (!value.presentation || (value.presentation->queued <= 1 && value.presentation->outcome <= 7 &&
            value.presentation->presented <= INT64_MAX && value.presentation->dropped <= INT64_MAX)) &&
        (!value.jitterBufferMeanMs || *value.jitterBufferMeanMs <= 60000) &&
        (!value.jitterBufferRecentMs || *value.jitterBufferRecentMs <= 60000) && uint8_t(value.decoder) <= 2;
}
inline std::vector<uint8_t> EncodeReceiverTelemetry(const ReceiverTelemetryMessage& value) {
    if (value.connection.empty() || value.connection.size() > 128 || !value.sequence || !ValidReceiverVideo(value.video)) return {};
    const auto& v = value.video;
    const bool extended = v.presentation || v.decoderDrops || v.jitterBufferMeanMs || v.jitterBufferRecentMs || v.decoder != CodecImplementation::Unknown;
    const uint8_t flags = uint8_t(v.fpsMilli.has_value()) | (v.presentation ? 2 : 0) |
        (v.decoderDrops ? 4 : 0) | (v.jitterBufferMeanMs ? 8 : 0) | (v.jitterBufferRecentMs ? 16 : 0);
    std::vector<uint8_t> bytes{'S', 'V', 'T', uint8_t(v.jitterBufferRecentMs ? 3 : extended ? 2 : 1), uint8_t(value.connection.size()), flags};
    auto put = [&](uint64_t number, unsigned count) {
        for (unsigned i = count; i; --i) bytes.push_back(uint8_t(number >> ((i - 1) * 8)));
    };
    put(value.sequence, 8); put(value.video.width, 2); put(value.video.height, 2);
    put(value.video.framesDecoded, 4); put(value.video.fpsMilli.value_or(0), 4);
    if (extended) {
        const auto p = v.presentation.value_or(ReceiverPresentationObservation{});
        put(p.presented, 8); put(p.dropped, 8); put(p.queued, 1); put(p.outcome, 1);
        put(v.decoderDrops.value_or(0), 4); put(v.jitterBufferMeanMs.value_or(0), 4); put(uint8_t(v.decoder), 1);
        if (v.jitterBufferRecentMs) put(*v.jitterBufferRecentMs, 4);
    }
    bytes.insert(bytes.end(), value.connection.begin(), value.connection.end());
    return bytes;
}
inline std::optional<ReceiverTelemetryMessage> DecodeReceiverTelemetry(std::span<const uint8_t> bytes) {
    if (bytes.size() < 27 || bytes.size() > 185 || bytes[0] != 'S' || bytes[1] != 'V' || bytes[2] != 'T' ||
        (bytes[3] < 1 || bytes[3] > 3) || !bytes[4] || bytes[4] > 128 ||
        bytes.size() != (bytes[3] == 1 ? 26u : bytes[3] == 2 ? 53u : 57u) + bytes[4] ||
        bytes[5] > (bytes[3] == 1 ? 1 : bytes[3] == 2 ? 15 : 31) ||
        (bytes[3] == 3 && !(bytes[5] & 16))) return {};
    size_t at = 6;
    auto get = [&](unsigned count) {
        uint64_t number = 0; while (count--) number = (number << 8) | bytes[at++]; return number;
    };
    ReceiverTelemetryMessage result; result.sequence = get(8);
    result.video.width = uint32_t(get(2)); result.video.height = uint32_t(get(2));
    result.video.framesDecoded = uint32_t(get(4)); const auto fps = uint32_t(get(4));
    if (bytes[5] & 1) result.video.fpsMilli = fps; else if (fps) return {};
    if (bytes[3] >= 2) {
        ReceiverPresentationObservation p; p.presented = get(8); p.dropped = get(8);
        p.queued = uint8_t(get(1)); p.outcome = uint8_t(get(1));
        if (bytes[5] & 2) result.video.presentation = p;
        else if (p.presented || p.dropped || p.queued || p.outcome) return {};
        const auto drops = uint32_t(get(4)), jitter = uint32_t(get(4));
        if (bytes[5] & 4) result.video.decoderDrops = drops; else if (drops) return {};
        if (bytes[5] & 8) result.video.jitterBufferMeanMs = jitter; else if (jitter) return {};
        result.video.decoder = CodecImplementation(get(1));
        if (bytes[3] == 3) result.video.jitterBufferRecentMs = uint32_t(get(4));
    }
    result.connection.assign(reinterpret_cast<const char*>(bytes.data() + at), bytes[4]);
    if (!result.sequence || !ValidReceiverVideo(result.video)) return {};
    return result;
}
// Signaling-owner only. Validation precedes sequence advancement. Receipt time
// uses the local monotonic clock; duplicate/reordered packets cannot renew data.
class ReceiverTelemetryInbox {
public:
    using Clock = std::chrono::steady_clock;
    void Bind(const std::string& connection) {
        if (connection_ == connection) return;
        connection_ = connection; sequence_ = 0; value_.reset(); sampled_ = {}; nextWindow_ = {}; acceptedInWindow_ = 0;
    }
    bool Receive(std::span<const uint8_t> bytes, Clock::time_point now = Clock::now()) {
        if (connection_.empty()) return false;
        if (now >= nextWindow_) { nextWindow_ = now + std::chrono::seconds(1); acceptedInWindow_ = 0; }
        if (acceptedInWindow_++ >= 4) { acceptedInWindow_ = 4; return false; }
        auto message = DecodeReceiverTelemetry(bytes);
        if (!message || message->connection != connection_ || message->sequence <= sequence_) return false;
        sequence_ = message->sequence; value_ = message->video; sampled_ = now; return true;
    }
    ReceiverVideoStatus Read(Clock::time_point now = Clock::now()) const {
        const bool stale = value_ && now - sampled_ >= std::chrono::seconds(3);
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - sampled_).count();
        return {stale ? std::nullopt : value_, stale, value_ ? std::optional(uint32_t(std::clamp<int64_t>(age, 0, UINT32_MAX))) : std::nullopt};
    }
private:
    std::string connection_;
    uint64_t sequence_ = 0;
    unsigned acceptedInWindow_ = 0;
    std::optional<ReceiverVideoObservation> value_;
    Clock::time_point sampled_{}, nextWindow_{};
};
}
