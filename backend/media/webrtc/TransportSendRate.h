#pragma once
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtcstats_objects.h"
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace screenshare::media {
// Each peer generation owns an independent mailbox. Late callbacks only retain
// this mailbox, never native peers/runtime/UI. At most one stats request is pending.
struct TransportSendRate {
    std::mutex mutex;
    bool pending = false;
    std::string transports;
    uint64_t bytes = 0;
    int64_t timestampUs = 0;
    std::optional<uint64_t> bitsPerSecond;
    std::chrono::steady_clock::time_point sampled{}, next{};
};
class TransportSendRateCallback : public webrtc::RTCStatsCollectorCallback {
    std::shared_ptr<TransportSendRate> state_;
public:
    explicit TransportSendRateCallback(std::shared_ptr<TransportSendRate> state) : state_(std::move(state)) {}
    void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
        uint64_t bytes = 0; std::string transports;
        for (const auto* transport : report->GetStatsOfType<webrtc::RTCTransportStats>()) {
            if (!transport->bytes_sent) continue;
            bytes += *transport->bytes_sent; transports += transport->id() + ";";
        }
        const auto at = report->timestamp().us();
        std::lock_guard lock(state_->mutex);
        state_->pending = false; state_->bitsPerSecond.reset();
        if (!transports.empty() && transports == state_->transports && at > state_->timestampUs && bytes >= state_->bytes)
            state_->bitsPerSecond = uint64_t(double(bytes - state_->bytes) * 8000000.0 / double(at - state_->timestampUs));
        state_->transports = std::move(transports); state_->bytes = bytes; state_->timestampUs = at;
        state_->sampled = std::chrono::steady_clock::now();
    }
};
}
