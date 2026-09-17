#pragma once
#include "ReceiverVideoStatus.h"
#include <chrono>
#include <span>
#include <string>
#include <vector>

namespace screenshare::media {
// Version 1, network byte order, exact size 26 + connection-id bytes (max 154).
// No clocks, names, addresses, SDP or credentials cross this telemetry boundary.
struct ReceiverTelemetryMessage {
    std::string connection;
    uint64_t sequence = 0;
    ReceiverVideoObservation video;
};
inline bool ValidReceiverVideo(const ReceiverVideoObservation& value) {
    return value.width >= 2 && value.width <= 3840 && value.height >= 2 && value.height <= 2160 &&
        value.width % 2 == 0 && value.height % 2 == 0 && (!value.fpsMilli || *value.fpsMilli <= 240000);
}
inline std::vector<uint8_t> EncodeReceiverTelemetry(const ReceiverTelemetryMessage& value) {
    if (value.connection.empty() || value.connection.size() > 128 || !value.sequence || !ValidReceiverVideo(value.video)) return {};
    std::vector<uint8_t> bytes{'S', 'V', 'T', 1, uint8_t(value.connection.size()), uint8_t(value.video.fpsMilli.has_value())};
    auto put = [&](uint64_t number, unsigned count) {
        for (unsigned i = count; i; --i) bytes.push_back(uint8_t(number >> ((i - 1) * 8)));
    };
    put(value.sequence, 8); put(value.video.width, 2); put(value.video.height, 2);
    put(value.video.framesDecoded, 4); put(value.video.fpsMilli.value_or(0), 4);
    bytes.insert(bytes.end(), value.connection.begin(), value.connection.end());
    return bytes;
}
inline std::optional<ReceiverTelemetryMessage> DecodeReceiverTelemetry(std::span<const uint8_t> bytes) {
    if (bytes.size() < 27 || bytes.size() > 154 || bytes[0] != 'S' || bytes[1] != 'V' || bytes[2] != 'T' ||
        bytes[3] != 1 || !bytes[4] || bytes[4] > 128 || bytes.size() != 26u + bytes[4] || bytes[5] > 1) return {};
    size_t at = 6;
    auto get = [&](unsigned count) {
        uint64_t number = 0; while (count--) number = (number << 8) | bytes[at++]; return number;
    };
    ReceiverTelemetryMessage result; result.sequence = get(8);
    result.video.width = uint32_t(get(2)); result.video.height = uint32_t(get(2));
    result.video.framesDecoded = uint32_t(get(4)); const auto fps = uint32_t(get(4));
    if (bytes[5]) result.video.fpsMilli = fps; else if (fps) return {};
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
        return {stale ? std::nullopt : value_, stale};
    }
private:
    std::string connection_;
    uint64_t sequence_ = 0;
    unsigned acceptedInWindow_ = 0;
    std::optional<ReceiverVideoObservation> value_;
    Clock::time_point sampled_{}, nextWindow_{};
};
}
