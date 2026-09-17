#pragma once
#include "api/RoomSession.h"
#include "shared/StreamPreferencesJson.h"
#include <QJsonObject>

// Shared UI/CLI vocabulary. These are local sender/source observations, never
// receiver-display acknowledgements or inferred network-congestion diagnoses.
inline QString StreamPeerState(const screenshare::v2::PeerStreamStatus& peer, uint64_t requested) {
    if (peer.rejected) return "rejected";
    if (!peer.appliedRevision || peer.appliedRevision != requested) return "pending";
    if (!peer.appliedVideoBitrateBps) return "upload-paused";
    if (peer.observedRevision != requested) return "waiting-for-source";
    return "source-observed";
}
inline QString StreamSampleState(const screenshare::v2::PeerStreamStatus& peer) {
    return peer.transportSampleStale ? "stale" : peer.transportSendBps ? "fresh" : "unknown";
}
inline QString StreamLimitReason(screenshare::media::VideoLimitReason reason) {
    using enum screenshare::media::VideoLimitReason;
    switch (reason) {
    case None: return "none"; case Cpu: return "cpu"; case Bandwidth: return "bandwidth"; case Other: return "other";
    default: return "unknown";
    }
}
inline QJsonObject StreamPeerJson(const screenshare::v2::PeerStreamStatus& peer, uint64_t requested) {
    const auto video = peer.receiver.stale ? std::nullopt : peer.receiver.observation;
    const QJsonValue unknown(QJsonValue::Null);
    const auto sender = peer.transportSampleStale ? screenshare::media::SenderVideoObservation{} : peer.sender;
    const auto& source = peer.source;
    const char* scalingPath = "unknown";
    using enum screenshare::media::SourceScalingPath;
    switch (source.scalingPath) {
    case Unchanged: scalingPath = "unchanged"; break;
    case Gpu: scalingPath = "gpu"; break;
    case Cpu: scalingPath = "cpu"; break;
    case CpuReadback: scalingPath = "cpu-readback"; break;
    default: break;
    }
    auto number = [&](std::optional<double> value) { return value ? QJsonValue(*value) : unknown; };
    return {{"peerId", QString::fromStdString(peer.peerId)}, {"requestedRevision", qint64(requested)},
        {"appliedRevision", qint64(peer.appliedRevision)}, {"observedRevision", qint64(peer.observedRevision)},
        {"state", StreamPeerState(peer, requested)}, {"rejected", peer.rejected},
        {"width", peer.width}, {"height", peer.height},
        {"source", QJsonObject{{"scalingPath", scalingPath},
            {"activeImage", source.observedRevision ? QJsonValue(QJsonObject{{"left", source.imageLeft}, {"top", source.imageTop},
                {"width", source.imageWidth}, {"height", source.imageHeight}}) : unknown}}},
        {"allocatedVideoBps", peer.allocatedVideoBitrateBps}, {"appliedVideoBps", peer.appliedVideoBitrateBps},
        {"transportSampleState", StreamSampleState(peer)},
        {"sender", QJsonObject{{"videoPayloadBps", sender.payloadBps ? QJsonValue(qint64(*sender.payloadBps)) : unknown},
            {"encodedFps", number(sender.encodedFps)}, {"rttMs", number(sender.rttMs)}, {"jitterMs", number(sender.jitterMs)},
            {"lossFraction", number(sender.lossFraction)}, {"availableOutgoingBps", number(sender.availableOutgoingBps)},
            {"limitingReason", StreamLimitReason(sender.limitingReason)}}},
        {"receiver", QJsonObject{{"sampleState", peer.receiver.stale ? "stale" : video ? "fresh" : "unknown"},
            {"ageSeconds", peer.receiver.ageSeconds ? QJsonValue(qint64(*peer.receiver.ageSeconds)) : unknown},
            {"width", video ? QJsonValue(int(video->width)) : unknown},
            {"height", video ? QJsonValue(int(video->height)) : unknown},
            {"framesDecoded", video ? QJsonValue(qint64(video->framesDecoded)) : unknown},
            {"decodeFps", video && video->fpsMilli ? QJsonValue(*video->fpsMilli / 1000.0) : unknown}}},
        {"transportSendBps", !peer.transportSampleStale && peer.transportSendBps ?
            QJsonValue(qint64(*peer.transportSendBps)) : QJsonValue(QJsonValue::Null)}};
}
