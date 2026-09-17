#include "RoomInputCommands.h"
#include <QFile>
#include <QJsonDocument>
#include <QSet>
#include <cmath>

namespace {
QByteArray Read(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) return {};
    const auto bytes = file.read(4097); return bytes.size() <= 4096 ? bytes : QByteArray{};
}
uint64_t Sequence(const QJsonObject& value) {
    const auto sequence = value["sequence"].toDouble();
    return std::isfinite(sequence) && sequence >= 1 && sequence <= 9007199254740991.0 && std::floor(sequence) == sequence ? uint64_t(sequence) : 0;
}
}
RoomInputCommands::RoomInputCommands(QString path) : path_(std::move(path)), previous_(Read(path_)) {
    sequence_ = Sequence(QJsonDocument::fromJson(previous_).object());
}
QJsonObject RoomInputCommands::Poll(const std::shared_ptr<screenshare::input::Port>& port, bool host) {
    const auto now = std::chrono::steady_clock::now();
    if (!port || now < next_) return {};
    next_ = now + std::chrono::milliseconds(100);
    const auto bytes = Read(path_);
    if (bytes.isEmpty() || bytes == previous_) return {};
    previous_ = bytes;
    const auto doc = QJsonDocument::fromJson(bytes); const auto object = doc.object();
    const auto sequence = Sequence(object);
    QJsonObject result{{"type", "input-command"}, {"sequence", double(sequence)}, {"accepted", false},
        {"operation", object["operation"].toString()}, {"peer", object["peer"].toString()}};
    if (!doc.isObject() || !sequence || sequence <= sequence_) return result;
    sequence_ = sequence; // Invalid/unavailable commands are consumed, never retried.
    const QSet<QString> keys{"sequence", "operation", "peer", "consent"};
    for (auto it = object.begin(); it != object.end(); ++it) if (!keys.contains(it.key())) return result;
    if (!object["operation"].isString() || (object.contains("peer") && !object["peer"].isString()) ||
        (object.contains("consent") && !object["consent"].isBool())) return result;
    const auto operation = object["operation"].toString(); const auto peer = object["peer"].toString().toStdString();
    if (peer.size() > 128 || peer.find('\0') != std::string::npos) return result;
    if (operation == "revoke") { port->Revoke(peer); result["accepted"] = true; return result; }
    if (peer.empty() || object["consent"] != QJsonValue(true)) return result;
    bool known = false;
    for (const auto& state : port->Read()) if (state.ready && state.peer == peer && (!host || (state.requested & screenshare::input::Gamepad))) known = true;
    if (!known) return result;
    result["accepted"] = host && operation == "grant" ? port->Grant(peer, screenshare::input::Gamepad) :
        !host && operation == "request" && port->Request(peer, screenshare::input::Gamepad);
    return result;
}
