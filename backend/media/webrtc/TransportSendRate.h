#pragma once
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtcstats_objects.h"
#include "media/SenderVideoStatus.h"
#include <cmath>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

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
    SenderVideoObservation sender;
    std::string videoId;
    uint64_t videoBytes = 0;
    int64_t videoTimestampUs = 0;
    std::chrono::steady_clock::time_point sampled{}, next{};
    struct Snapshot { std::optional<uint64_t> bitsPerSecond; bool stale; SenderVideoObservation sender; };
    Snapshot Read(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        std::lock_guard lock(mutex);
        const bool stale = sampled != std::chrono::steady_clock::time_point{} && now - sampled >= std::chrono::seconds(3);
        return {stale ? std::nullopt : bitsPerSecond, stale, stale ? SenderVideoObservation{} : sender};
    }
};
class TransportSendRateCallback : public webrtc::RTCStatsCollectorCallback {
    std::shared_ptr<TransportSendRate> state_;
public:
    explicit TransportSendRateCallback(std::shared_ptr<TransportSendRate> state) : state_(std::move(state)) {}
    void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
        auto rate = [](uint64_t bytes, uint64_t previousBytes, int64_t at, int64_t previousAt) -> std::optional<uint64_t> {
            if (bytes < previousBytes || previousAt < 0 || at <= previousAt) return {};
            const double value = double(bytes - previousBytes) * 8000000.0 / (double(at) - double(previousAt));
            // Reject counter/timestamp discontinuities instead of overflowing a
            // numeric conversion or fabricating an implausible rate.
            return std::isfinite(value) && value <= 1e12 ? std::optional(uint64_t(value)) : std::nullopt;
        };
        uint64_t bytes = 0; std::string transports;
        for (const auto* transport : report->GetStatsOfType<webrtc::RTCTransportStats>()) {
            if (!transport->bytes_sent) continue;
            bytes += *transport->bytes_sent; transports += transport->id() + ";";
        }
        const auto at = report->timestamp().us();
        std::lock_guard lock(state_->mutex);
        state_->pending = false; state_->bitsPerSecond.reset();
        if (!transports.empty() && transports == state_->transports && at > state_->timestampUs && bytes >= state_->bytes)
            state_->bitsPerSecond = rate(bytes, state_->bytes, at, state_->timestampUs);
        state_->transports = std::move(transports); state_->bytes = bytes; state_->timestampUs = at;
        state_->sampled = std::chrono::steady_clock::now();
        state_->sender = {};
        auto finite = [](std::optional<double> value, double maximum, double scale = 1.0) -> std::optional<double> {
            return value && std::isfinite(*value) && *value >= 0 && *value <= maximum ? std::optional(*value * scale) : std::nullopt;
        };
        // Only the selected transport pair is relevant. Never publish candidate
        // IDs/addresses or accidentally report a faster unused candidate pair.
        const webrtc::RTCIceCandidatePairStats* selected = nullptr;
        unsigned selectedCount = 0;
        for (const auto* transport : report->GetStatsOfType<webrtc::RTCTransportStats>()) {
            if (!transport->selected_candidate_pair_id) continue;
            const auto* stats = report->Get(*transport->selected_candidate_pair_id);
            if (!stats || std::string_view(stats->type()) != webrtc::RTCIceCandidatePairStats::kType) continue;
            selected = &stats->cast_to<webrtc::RTCIceCandidatePairStats>(); ++selectedCount;
        }
        if (selectedCount == 1) {
            state_->sender.rttMs = finite(selected->current_round_trip_time, 3600, 1000);
            state_->sender.availableOutgoingBps = finite(selected->available_outgoing_bitrate, 1e12);
        }
        const webrtc::RTCOutboundRtpStreamStats* video = nullptr;
        unsigned videos = 0;
        for (const auto* outbound : report->GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>()) {
            if (outbound->kind && *outbound->kind == "video") { video = outbound; ++videos; }
        }
        if (videos == 1) {
            state_->sender.encodedFps = finite(video->frames_per_second, 240);
            if (video->quality_limitation_reason) {
                const auto& reason = *video->quality_limitation_reason;
                state_->sender.limitingReason = reason == "none" ? VideoLimitReason::None : reason == "cpu" ? VideoLimitReason::Cpu :
                    reason == "bandwidth" ? VideoLimitReason::Bandwidth : reason == "other" ? VideoLimitReason::Other : VideoLimitReason::Unknown;
            }
            if (video->remote_id) {
                const auto* stats = report->Get(*video->remote_id);
                if (stats && std::string_view(stats->type()) == webrtc::RTCRemoteInboundRtpStreamStats::kType) {
                    const auto& remote = stats->cast_to<webrtc::RTCRemoteInboundRtpStreamStats>();
                    state_->sender.lossFraction = finite(remote.fraction_lost, 1);
                    state_->sender.jitterMs = finite(remote.jitter, 3600, 1000);
                }
            }
            if (video->bytes_sent && state_->videoId == video->id() && at > state_->videoTimestampUs && *video->bytes_sent >= state_->videoBytes)
                state_->sender.payloadBps = rate(*video->bytes_sent, state_->videoBytes, at, state_->videoTimestampUs);
            state_->videoId = video->bytes_sent ? video->id() : "";
            state_->videoBytes = video->bytes_sent.value_or(0); state_->videoTimestampUs = at;
        } else { state_->videoId.clear(); state_->videoBytes = 0; state_->videoTimestampUs = 0; }
    }
};
}
