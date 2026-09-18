#pragma once
#include "shared/RoomStreamDiagnostics.h"
#include "shared/RoomInputStatus.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSaveFile>
#include <QUuid>

// Typed allowlist: no options, raw logs, SDP, credentials, device IDs, source
// handles, room names, nicknames, network addresses or real peer IDs are exported.
inline QJsonObject RoomDiagnosticReport(const screenshare::v2::RoomStatus& status,
    const std::vector<screenshare::input::Status>& input = {}) {
    QMap<QString, QString> aliases;
    auto alias = [&](const std::string& id) {
        const auto key = QString::fromStdString(id);
        if (!aliases.contains(key)) aliases[key] = "peer-" + QString::number(aliases.size() + 1);
        return aliases.value(key);
    };
    QJsonArray peers, controls;
    for (const auto& peer : status.stream.peers) {
        auto value = StreamPeerJson(peer, status.stream.requestedRevision);
        value["peerId"] = alias(peer.peerId); peers.append(value);
    }
    for (const auto& state : input) {
        auto value = screenshare::frontend::InputStatus(state);
        value["peer"] = alias(state.peer); controls.append(value);
    }
    return {{"schema", 1}, {"backend", "v2"}, {"reportId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"createdUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"appVersion", QCoreApplication::applicationVersion()}, {"phase", int(status.phase)}, {"error", int(status.error)},
        {"activePeers", qint64(status.activePeers)}, {"failedPeers", qint64(status.failedPeers)},
        {"requestedPreferences", StreamPreferencesJson(status.stream.preferences)},
        {"pipeline", PipelineDiagnosticsJson(status.stream)}, {"settingsApplication", StreamApplicationJson(status.stream)},
        {"peers", peers}, {"input", controls},
        {"audio", QJsonObject{{"source", screenshare::media::AudioKindName(status.audio.selected.kind)},
            {"captureState", int(status.audio.health.state)}, {"captureFailures", qint64(status.audio.health.failures)},
            {"playbackState", int(status.playback.health.state)}, {"playbackFailures", qint64(status.playback.health.failures)},
            {"volume", int(status.playback.selected.volume)}, {"muted", status.playback.selected.muted}}},
        {"externalLatencyVerified", false}};
}
inline bool WriteRoomDiagnosticReport(const QString& path, const QJsonObject& report) {
    const auto bytes = QJsonDocument(report).toJson();
    if (path.isEmpty() || bytes.size() > 1024 * 1024 || !QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile file(path); // Failed writes never replace an existing report.
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
