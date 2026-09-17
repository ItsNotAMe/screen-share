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
inline QJsonObject StreamPeerJson(const screenshare::v2::PeerStreamStatus& peer, uint64_t requested) {
    const auto video = peer.receiver.stale ? std::nullopt : peer.receiver.observation;
    const QJsonValue unknown(QJsonValue::Null);
    return {{"peerId", QString::fromStdString(peer.peerId)}, {"requestedRevision", qint64(requested)},
        {"appliedRevision", qint64(peer.appliedRevision)}, {"observedRevision", qint64(peer.observedRevision)},
        {"state", StreamPeerState(peer, requested)}, {"rejected", peer.rejected},
        {"width", peer.width}, {"height", peer.height},
        {"allocatedVideoBps", peer.allocatedVideoBitrateBps}, {"appliedVideoBps", peer.appliedVideoBitrateBps},
        {"transportSampleState", StreamSampleState(peer)},
        {"receiver", QJsonObject{{"sampleState", peer.receiver.stale ? "stale" : video ? "fresh" : "unknown"},
            {"width", video ? QJsonValue(int(video->width)) : unknown},
            {"height", video ? QJsonValue(int(video->height)) : unknown},
            {"framesDecoded", video ? QJsonValue(qint64(video->framesDecoded)) : unknown},
            {"decodeFps", video && video->fpsMilli ? QJsonValue(*video->fpsMilli / 1000.0) : unknown}}},
        {"transportSendBps", !peer.transportSampleStale && peer.transportSendBps ?
            QJsonValue(qint64(*peer.transportSendBps)) : QJsonValue(QJsonValue::Null)}};
}
