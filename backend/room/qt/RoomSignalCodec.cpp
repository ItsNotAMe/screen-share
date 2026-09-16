#include "RoomSignalCodec.h"
#include "room/protocol/RoomProtocol.h"
#include <QJsonDocument>

namespace screenshare::room::qt {
QByteArray EncodeRoomSignal(const media::RoomPeerSignal& signal, const QString& room, const QString& target) {
    using Kind = media::RoomPeerSignal::Kind;
    QString type; QJsonObject payload;
    switch (signal.kind) {
    case Kind::Offer: type = "signal.offer"; payload["sdp"] = QString::fromStdString(signal.sdp); break;
    case Kind::Answer: type = "signal.answer"; payload["sdp"] = QString::fromStdString(signal.sdp); break;
    case Kind::Candidate: type = "signal.candidate"; payload = {{"candidate", QString::fromStdString(signal.ice.candidate)}, {"sdpMid", QString::fromStdString(signal.ice.mid)}, {"sdpMLineIndex", signal.ice.line}}; break;
    case Kind::RestartRequest: type = "signal.restart_request"; break;
    default: return {};
    }
    return QJsonDocument(QJsonObject{{"v", 2}, {"type", type}, {"roomId", room}, {"connectionId", QString::fromStdString(signal.connectionId)}, {"toPeerId", target}, {"payload", payload}}).toJson(QJsonDocument::Compact);
}
std::optional<media::RoomPeerSignal> DecodeRoomSignal(const QJsonObject& event) {
    const auto validated = wire::ValidateServerEvent(QJsonDocument(event).toJson(QJsonDocument::Compact));
    if (!validated.ok) return std::nullopt;
    media::RoomPeerSignal signal;
    const auto type = event["type"].toString();
    if (type == "signal.offer") signal.kind = media::RoomPeerSignal::Kind::Offer;
    else if (type == "signal.answer") signal.kind = media::RoomPeerSignal::Kind::Answer;
    else if (type == "signal.candidate") signal.kind = media::RoomPeerSignal::Kind::Candidate;
    else if (type == "signal.restart_request") signal.kind = media::RoomPeerSignal::Kind::RestartRequest;
    else return std::nullopt;
    signal.connectionId = event["connectionId"].toString().toStdString();
    const auto payload = event["payload"].toObject(); signal.sdp = payload["sdp"].toString().toStdString();
    signal.ice = {payload["candidate"].toString().toStdString(), payload["sdpMid"].toString().toStdString(), payload["sdpMLineIndex"].toInt()};
    return signal;
}
}
