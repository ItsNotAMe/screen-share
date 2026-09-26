#pragma once
#include "shared/RoomStreamDiagnostics.h"
#include "shared/RoomInputStatus.h"
#include "shared/DiagnosticHistoryJson.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSaveFile>
#include <QUuid>
#include <QSysInfo>
#include <QCryptographicHash>

QString RoomReportBuildVersion();
QJsonObject RoomReportBuildInfo();
QJsonArray RoomReportGraphicsInfo();

// Typed allowlist: no options, raw logs, SDP, credentials, device IDs, source
// handles, room names, nicknames, network addresses or real peer IDs are exported.
inline QJsonObject RoomDiagnosticReport(const screenshare::v2::RoomStatus& status,
    const std::vector<screenshare::input::Status>& input = {}) {
    QMap<QString, QString> aliases;
    auto correlation = [&](const std::string& id) {
        return QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromStdString(status.roomId + ":" + id), QCryptographicHash::Sha256).toHex().left(20));
    };
    auto alias = [&](const std::string& id) {
        const auto key = QString::fromStdString(id);
        if (!aliases.contains(key)) aliases[key] = "peer-" + QString::number(aliases.size() + 1);
        return aliases.value(key);
    };
    QJsonArray peers, controls, connections;
    for (const auto& peer : status.stream.peers) {
        auto value = StreamPeerJson(peer, status.stream.requestedRevision);
        value["peerId"] = alias(peer.peerId); peers.append(value);
    }
    for (const auto& state : input) {
        auto value = screenshare::frontend::InputStatus(state);
        value["peer"] = alias(state.peer); controls.append(value);
    }
    for (const auto& connection : status.stream.connections) {
        auto value = PeerRecoveryJson(connection.recovery);
        value["peerId"] = alias(connection.peerId);
        value["negotiated"] = connection.negotiated;
        value["retained"] = connection.retained;
        value["localCandidates"] = qint64(connection.localCandidates);
        value["remoteCandidates"] = qint64(connection.remoteCandidates);
        value["correlation"] = correlation(connection.peerId);
        value["generation"] = qint64(connection.generation);
        value["ageMs"] = qint64(connection.ageMs);
        value["current"] = DiagnosticRecordJson(connection.current);
        value["events"] = DiagnosticHistoryJson(connection.events);
        value["transportHistory"] = DiagnosticHistoryJson(connection.transportHistory);
        value["mediaHistory"] = DiagnosticHistoryJson(connection.mediaHistory);
        value["statsAgeMs"] = connection.statsAgeMs ? QJsonValue(qint64(*connection.statsAgeMs)) : QJsonValue(QJsonValue::Null);
        connections.append(value);
    }
    return {{"schema", 1}, {"backend", "v2"}, {"reportId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"createdUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"appVersion", RoomReportBuildVersion()}, {"phase", int(status.phase)}, {"error", int(status.error)},
        {"activePeers", qint64(status.activePeers)}, {"failedPeers", qint64(status.failedPeers)},
        {"pendingPeers", qint64(status.pendingPeers)}, {"connections", connections},
        {"diagnosticsSchema", 2}, {"role", status.host ? "host" : "viewer"},
        {"selfCorrelation", correlation(status.peerId)},
        {"build", RoomReportBuildInfo()}, {"graphicsAdapters", RoomReportGraphicsInfo()},
        {"sessionEvents", DiagnosticHistoryJson(status.sessionEvents)},
        {"mediaEvents", DiagnosticHistoryJson(status.stream.mediaEvents)},
        {"performanceHistory", DiagnosticHistoryJson(status.stream.performance)},
        {"platform", QJsonObject{{"os", QSysInfo::productType()}, {"version", QSysInfo::productVersion()},
            {"kernelVersion", QSysInfo::kernelVersion()}, {"architecture", QSysInfo::currentCpuArchitecture()}}},
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
    if (path.isEmpty() || bytes.size() > 32 * 1024 * 1024 || !QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile file(path); // Failed writes never replace an existing report.
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
