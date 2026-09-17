#pragma once
#include "media/ReceiverTelemetry.h"
#include "api/data_channel_interface.h"
#include "api/make_ref_counted.h"
#include "api/peer_connection_interface.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtcstats_objects.h"
#include <cmath>
#include <memory>
#include <mutex>

namespace screenshare::media {
struct ReceiverStatsMailbox {
    std::mutex mutex;
    bool pending = false;
    uint64_t serial = 0;
    std::optional<ReceiverVideoObservation> video;
    std::chrono::steady_clock::time_point sampled{}, next{};
};
class ReceiverStatsCallback : public webrtc::RTCStatsCollectorCallback {
    std::shared_ptr<ReceiverStatsMailbox> mailbox_;
public:
    explicit ReceiverStatsCallback(std::shared_ptr<ReceiverStatsMailbox> mailbox) : mailbox_(std::move(mailbox)) {}
    void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
        std::optional<ReceiverVideoObservation> video;
        unsigned videoReceivers = 0;
        for (const auto* inbound : report->GetStatsOfType<webrtc::RTCInboundRtpStreamStats>()) {
            if (!inbound->kind || *inbound->kind != "video") continue;
            if (++videoReceivers > 1) { video.reset(); break; }
            if (!inbound->frame_width || !inbound->frame_height || !inbound->frames_decoded) continue;
            // The room contract has one video track. Never silently select an
            // arbitrary receiver if the runtime unexpectedly supplies several.
            if (video) { video.reset(); break; }
            ReceiverVideoObservation value{*inbound->frame_width, *inbound->frame_height, *inbound->frames_decoded};
            value.decoderDrops = inbound->frames_dropped;
            if (inbound->jitter_buffer_delay && inbound->jitter_buffer_emitted_count && *inbound->jitter_buffer_emitted_count) {
                const double mean = *inbound->jitter_buffer_delay * 1000 / *inbound->jitter_buffer_emitted_count;
                if (std::isfinite(mean) && mean >= 0 && mean <= 60000) value.jitterBufferMeanMs = uint32_t(std::lround(mean));
            }
            if (inbound->decoder_implementation == "Media Foundation H264 (CPU NV12)") value.decoder = CodecImplementation::MfH264Software;
            if (inbound->decoder_implementation == "Media Foundation H264 (D3D11 NV12)") value.decoder = CodecImplementation::MfH264Hardware;
            if (inbound->frames_per_second && std::isfinite(*inbound->frames_per_second) &&
                *inbound->frames_per_second >= 0 && *inbound->frames_per_second <= 240)
                value.fpsMilli = uint32_t(std::lround(*inbound->frames_per_second * 1000));
            if (ValidReceiverVideo(value)) video = value;
        }
        std::lock_guard lock(mailbox_->mutex);
        mailbox_->pending = false; mailbox_->video = video; ++mailbox_->serial;
        mailbox_->sampled = std::chrono::steady_clock::now();
    }
};
// Owned by one native Entry. All methods and observer callbacks run on signaling;
// stats callbacks retain only a mailbox, never this observer or its native peer.
class ReceiverTelemetryChannel final : public webrtc::DataChannelObserver {
    bool host_;
    std::string connection_;
    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel_;
    ReceiverTelemetryInbox inbox_;
    std::shared_ptr<ReceiverStatsMailbox> mailbox_ = std::make_shared<ReceiverStatsMailbox>();
    uint64_t sent_ = 0;
    std::shared_ptr<PresentationTelemetry> presentation_;
public:
    explicit ReceiverTelemetryChannel(bool host, std::shared_ptr<PresentationTelemetry> presentation = {})
        : host_(host), presentation_(std::move(presentation)) {}
    ~ReceiverTelemetryChannel() override { if (channel_) channel_->UnregisterObserver(); }
    void Attach(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
        if (channel_) channel_->UnregisterObserver();
        channel_ = std::move(channel); channel_->RegisterObserver(this);
    }
    void OnStateChange() override {}
    void OnMessage(const webrtc::DataBuffer& buffer) override {
        if (host_ && buffer.binary) inbox_.Receive({buffer.data.cdata<uint8_t>(), buffer.data.size()});
    }
    ReceiverVideoStatus Status() const { return inbox_.Read(); }
    void Advance(const std::string& connection, webrtc::PeerConnectionInterface& peer, bool ready) {
        using namespace std::chrono_literals;
        if (connection != connection_) {
            connection_ = connection; inbox_.Bind(connection); sent_ = 0;
            // A late callback from an earlier ICE negotiation cannot publish
            // into the new generation's mailbox or send under its identifier.
            mailbox_ = std::make_shared<ReceiverStatsMailbox>();
        }
        if (host_ || !ready || connection.empty() || !channel_ || channel_->state() != webrtc::DataChannelInterface::kOpen) return;
        const auto now = std::chrono::steady_clock::now();
        bool request = false;
        std::optional<ReceiverVideoObservation> video;
        uint64_t serial;
        {
            std::lock_guard lock(mailbox_->mutex);
            if (!mailbox_->pending && now >= mailbox_->next) {
                mailbox_->pending = true; mailbox_->next = now + 1s; request = true;
            }
            serial = mailbox_->serial;
            if (now - mailbox_->sampled < 3s) video = mailbox_->video;
        }
        if (request) peer.GetStats(webrtc::make_ref_counted<ReceiverStatsCallback>(mailbox_).get());
        // No application send queue or retransmission. Under SCTP pressure keep
        // only the newest local sample; stale samples are never sent later.
        if (video && serial > sent_ && channel_->buffered_amount() == 0) {
            if (presentation_) video->presentation = presentation_->Read();
            const auto bytes = EncodeReceiverTelemetry({connection_, serial, *video});
            sent_ = serial; // A rejected send is dropped, not retried every tick.
            if (!bytes.empty()) channel_->Send(webrtc::DataBuffer(webrtc::CopyOnWriteBuffer(bytes.data(), bytes.size()), true));
        }
    }
};
}
