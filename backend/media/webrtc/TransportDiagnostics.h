#pragma once
#include "media/DiagnosticHistory.h"
#include "api/stats/rtcstats_objects.h"

namespace screenshare::media {
inline DiagnosticRecord TransportDiagnostics(const webrtc::RTCStatsReport& report) {
    DiagnosticRecord result;
    auto number = [&](const char* key, const auto& value, double scale = 1) { DiagnosticNumber(result, key, value, scale); };
    auto label = [&](const char* key, const auto& value, std::initializer_list<const char*> allowed) {
        result.labels[key] = DiagnosticLabel(value, allowed);
    };
    const webrtc::RTCIceCandidatePairStats* selected = nullptr;
    for (const auto* value : report.GetStatsOfType<webrtc::RTCTransportStats>()) {
        number("transportBytesSent", value->bytes_sent); number("transportBytesReceived", value->bytes_received);
        number("transportPacketsSent", value->packets_sent); number("transportPacketsReceived", value->packets_received);
        number("selectedPairChanges", value->selected_candidate_pair_changes);
        label("dtlsState", value->dtls_state, {"new", "connecting", "connected", "closed", "failed"});
        label("iceRole", value->ice_role, {"controlling", "controlled"});
        if (value->selected_candidate_pair_id) {
            const auto* pair = report.Get(*value->selected_candidate_pair_id);
            if (pair && std::string_view(pair->type()) == webrtc::RTCIceCandidatePairStats::kType)
                selected = &pair->cast_to<webrtc::RTCIceCandidatePairStats>();
        }
    }
    unsigned pairs = 0;
    for (const auto* value : report.GetStatsOfType<webrtc::RTCIceCandidatePairStats>()) {
        ++pairs;
        ++result.numbers["candidatePairs_" + DiagnosticLabel(value->state, {"frozen", "waiting", "in-progress", "failed", "succeeded"})];
        result.numbers["connectivityRequestsSent"] += double(value->requests_sent.value_or(0));
        result.numbers["connectivityResponsesReceived"] += double(value->responses_received.value_or(0));
        result.numbers["connectivityRequestsReceived"] += double(value->requests_received.value_or(0));
        result.numbers["connectivityResponsesSent"] += double(value->responses_sent.value_or(0));
    }
    result.numbers["candidatePairs"] = pairs;
    result.numbers["selectedPair"] = selected ? 1 : 0;
    auto candidates = [&](auto values, const char* prefix, const std::optional<std::string>& selectedId) {
        for (const auto* value : values) {
            const auto type = DiagnosticLabel(value->candidate_type, {"host", "srflx", "prflx", "relay"});
            ++result.numbers[std::string(prefix) + "Candidates_" + type];
            if (selectedId && value->id() == *selectedId) {
                result.labels[std::string(prefix) + "CandidateType"] = type;
                result.labels[std::string(prefix) + "Protocol"] = DiagnosticLabel(value->protocol, {"udp", "tcp"});
                result.labels[std::string(prefix) + "Network"] = DiagnosticLabel(value->network_type,
                    {"ethernet", "wifi", "cellular", "vpn", "loopback", "unknown"});
                if (value->vpn) result.numbers[std::string(prefix) + "Vpn"] = *value->vpn;
                if (value->address) result.labels[std::string(prefix) + "AddressFamily"] =
                    value->address->find(':') != std::string::npos ? "ipv6" : "ipv4";
            }
        }
    };
    candidates(report.GetStatsOfType<webrtc::RTCLocalIceCandidateStats>(), "local", selected ? selected->local_candidate_id : std::nullopt);
    candidates(report.GetStatsOfType<webrtc::RTCRemoteIceCandidateStats>(), "remote", selected ? selected->remote_candidate_id : std::nullopt);
    if (selected) {
        label("selectedPairState", selected->state, {"frozen", "waiting", "in-progress", "failed", "succeeded"});
        number("rttMs", selected->current_round_trip_time, 1000);
        number("availableOutgoingBps", selected->available_outgoing_bitrate);
        number("availableIncomingBps", selected->available_incoming_bitrate);
        number("consentRequestsSent", selected->consent_requests_sent);
        number("packetsDiscardedOnSend", selected->packets_discarded_on_send);
    }
    for (const auto* value : report.GetStatsOfType<webrtc::RTCInboundRtpStreamStats>()) {
        const auto kind = DiagnosticLabel(value->kind, {"audio", "video"});
        auto put = [&](const char* name, const auto& field, double scale = 1) {
            DiagnosticNumber(result, (kind + "Receive" + name).c_str(), field, scale);
        };
        put("Packets", value->packets_received); put("Bytes", value->bytes_received);
        put("PacketsLost", value->packets_lost); put("JitterMs", value->jitter, 1000);
        put("JitterBufferSeconds", value->jitter_buffer_delay); put("JitterBufferEmitted", value->jitter_buffer_emitted_count);
        put("ConcealedSamples", value->concealed_samples); put("Samples", value->total_samples_received);
        put("FramesReceived", value->frames_received); put("FramesDecoded", value->frames_decoded);
        put("KeyFramesDecoded", value->key_frames_decoded); put("FramesDropped", value->frames_dropped);
        put("Fps", value->frames_per_second); put("DecodeSeconds", value->total_decode_time);
        put("ProcessingSeconds", value->total_processing_delay); put("FreezeCount", value->freeze_count);
        put("FreezeSeconds", value->total_freezes_duration); put("Width", value->frame_width); put("Height", value->frame_height);
        put("NackCount", value->nack_count); put("PliCount", value->pli_count);
        if (kind == "video") label("decoder", value->decoder_implementation,
            {"Media Foundation H264 (CPU NV12)", "Media Foundation H264 (D3D11 NV12)"});
        if (value->codec_id) {
            const auto* codec = report.Get(*value->codec_id);
            if (codec && std::string_view(codec->type()) == webrtc::RTCCodecStats::kType)
                result.labels[kind + "Codec"] = DiagnosticLabel(codec->cast_to<webrtc::RTCCodecStats>().mime_type,
                    {"video/H264", "audio/opus"});
        }
    }
    for (const auto* value : report.GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>()) {
        const auto kind = DiagnosticLabel(value->kind, {"audio", "video"});
        auto put = [&](const char* name, const auto& field, double scale = 1) {
            DiagnosticNumber(result, (kind + "Send" + name).c_str(), field, scale);
        };
        put("Packets", value->packets_sent); put("Bytes", value->bytes_sent);
        put("FramesEncoded", value->frames_encoded); put("Fps", value->frames_per_second);
        put("EncodeSeconds", value->total_encode_time); put("PacketDelaySeconds", value->total_packet_send_delay);
        put("TargetBps", value->target_bitrate); put("RetransmittedPackets", value->retransmitted_packets_sent);
        put("KeyFrames", value->key_frames_encoded); put("NackCount", value->nack_count); put("PliCount", value->pli_count);
        if (kind == "video") {
            label("encoder", value->encoder_implementation, {"Media Foundation H264 hardware (D3D11/NV12)", "Media Foundation H264 software (CPU I420/NV12)"});
            label("qualityLimitation", value->quality_limitation_reason, {"none", "cpu", "bandwidth", "other"});
        }
    }
    return result;
}
}
