#include "RoomProtocol.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QStringDecoder>
#include <cmath>
#include <QSet>

namespace screenshare::room::wire {
namespace {
bool Keys(const QJsonObject& o, QStringList required, QStringList optional = {}) {
    for (const auto& k : required) if (!o.contains(k)) return false;
    for (auto i = o.begin(); i != o.end(); ++i)
        if (!required.contains(i.key()) && !optional.contains(i.key())) return false;
    return true;
}
bool Integer(QJsonValue v, double min = 0, double max = 9007199254740991.0) {
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= min &&
        v.toDouble() <= max && std::floor(v.toDouble()) == v.toDouble();
}
bool Text(QJsonValue v, int min, int bytes) {
    if (!v.isString()) return false;
    auto s = v.toString();
    if (s.size() < min || s.toUtf8().size() > bytes) return false;
    for (qsizetype i = 0; i < s.size(); ++i) {
        if (s[i].isHighSurrogate()) {
            if (++i == s.size() || !s[i].isLowSurrogate()) return false;
        } else if (s[i].isLowSurrogate()) return false;
    }
    return true;
}
bool Id(QJsonValue v) {
    if (!Text(v, 1, 128)) return false;
    for (auto c : v.toString()) {
        auto p = c.unicode();
        if (!(p >= 'a' && p <= 'z') && !(p >= 'A' && p <= 'Z') &&
            !(p >= '0' && p <= '9') && p != '_' && p != '-') return false;
    }
    return true;
}
bool Space(char16_t p) {
    return (p >= 9 && p <= 13) || p == 32 || p == 0x85 || p == 0xa0 ||
        p == 0x1680 || (p >= 0x2000 && p <= 0x200a) || p == 0x2028 ||
        p == 0x2029 || p == 0x202f || p == 0x205f || p == 0x3000;
}
QString Name(QJsonValue v, int points = 32, int bytes = 128) {
    if (!Text(v, 0, 1024)) return {};
    auto s = v.toString().normalized(QString::NormalizationForm_C);
    while (!s.isEmpty() && Space(s.front().unicode())) s.removeFirst();
    while (!s.isEmpty() && Space(s.back().unicode())) s.chop(1);
    if (s.toUcs4().size() > points || s.toUtf8().size() > bytes) return {};
    for (auto c : s) {
        auto p = c.unicode();
        if (p <= 31 || (p >= 127 && p <= 159) || p == 0x61c || p == 0x200e ||
            p == 0x200f || (p >= 0x202a && p <= 0x202e) ||
            (p >= 0x2066 && p <= 0x2069) || p == 0xfeff) return {};
    }
    return s;
}
}
static Validation ParseEnvelope(const QByteArray& bytes, qsizetype limit) {
    auto fail = [](const char* error) { return Validation{false, QString::fromLatin1(error), {}}; };
    if (bytes.size() > limit) return fail("too_large");
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    QString decoded = decoder.decode(bytes);
    // Preserve string BOMs through Qt's UTF-8 JSON string storage. Do not turn
    // an illegal escape or an out-of-string BOM into valid JSON while doing so.
    bool inString = false, escaped = false;
    QString preserved;
    preserved.reserve(decoded.size());
    for (auto c : decoded) {
        if (c.unicode() == 0xfeff) {
            if (!inString || escaped) return fail("invalid_json");
            preserved += QStringLiteral("\\ufeff");
        } else preserved += c;
        if (escaped) escaped = false;
        else if (inString && c == '\\') escaped = true;
        else if (c == '"') inString = !inString;
    }
    decoded = std::move(preserved);
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(decoded.toUtf8(), &error);
    if (decoder.hasError()) return fail("invalid_json");
    if (error.error != QJsonParseError::NoError) {
        // Qt documents accept only object/array roots; JSON also permits scalars.
        auto scalar = QJsonDocument::fromJson("[" + decoded.toUtf8() + "]");
        if (scalar.isArray() && scalar.array().size() == 1) return fail("invalid_envelope");
        return fail("invalid_json");
    }
    if (!doc.isObject()) return fail("invalid_envelope");
    auto m = doc.object();
    if (!Integer(m["v"], 2, 2) || !m["type"].isString() || !m["payload"].isObject()) return fail("invalid_envelope");
    return {true, {}, m};
}
Validation ValidateClientCommand(const QByteArray& bytes, bool directory) {
    auto fail = [](const char* error) { return Validation{false, QString::fromLatin1(error), {}}; };
    auto parsed = ParseEnvelope(bytes, 64 * 1024);
    if (!parsed.ok) return parsed;
    auto m = parsed.message;
    auto type = m["type"].toString();
    const bool signal = QStringList{"signal.offer", "signal.answer", "signal.candidate", "signal.restart_request"}.contains(type);
    const bool mutation = QStringList{"profile.update", "room.update", "peer.disconnect", "peer.leave"}.contains(type);
    if (!signal && !mutation && type != "state.resync") return fail("invalid_envelope");
    if (!signal && bytes.size() > 16 * 1024) return fail("too_large");
    QStringList required{"v", "type", "payload"};
    if (directory) { if (type != "state.resync") return fail("invalid_envelope"); }
    else required << "roomId";
    if (mutation) required << "requestId";
    if (signal) required << "connectionId" << "toPeerId";
    if (!Keys(m, required)) return fail("invalid_envelope");
    for (const auto& k : required) if (k.endsWith("Id") && !Id(m[k])) return fail("invalid_envelope");
    auto p = m["payload"].toObject();
    bool valid = false;
    if (type == "profile.update") {
        auto name = Name(p["nickname"]);
        valid = Keys(p, {"nickname", "expectedRevision"}) && Integer(p["expectedRevision"]) && !name.isEmpty();
        if (valid) p["nickname"] = name;
    } else if (type == "room.update") {
        valid = Keys(p, {"expectedRevision"}, {"name", "visibility", "viewerLimit"}) && Integer(p["expectedRevision"]) && p.size() > 1;
        if (p.contains("name")) { auto name = Name(p["name"], 64, 256); valid &= !name.isEmpty(); p["name"] = name; }
        if (p.contains("visibility")) valid &= p["visibility"] == "public" || p["visibility"] == "unlisted";
        if (p.contains("viewerLimit")) valid &= Integer(p["viewerLimit"], 1, 63);
    } else if (type == "peer.disconnect") valid = Keys(p, {"peerId"}) && Id(p["peerId"]);
    else if (type == "signal.offer" || type == "signal.answer")
        valid = Keys(p, {"sdp"}) && Text(p["sdp"], 1, 60 * 1024) && !p["sdp"].toString().contains(QChar(0));
    else if (type == "signal.candidate")
        valid = Keys(p, {"candidate", "sdpMid", "sdpMLineIndex"}, {"usernameFragment"}) &&
            Text(p["candidate"], 0, 4096) && !p["candidate"].toString().contains(QChar(0)) &&
            (p["sdpMid"].isNull() || (Text(p["sdpMid"], 1, 64) && !p["sdpMid"].toString().contains(QChar(0)))) &&
            (p["sdpMLineIndex"].isNull() || Integer(p["sdpMLineIndex"], 0, 31)) &&
            (!p.contains("usernameFragment") || p["usernameFragment"].isNull() || (Text(p["usernameFragment"], 1, 256) && !p["usernameFragment"].toString().contains(QChar(0))));
    else valid = Keys(p, {});
    if (!valid) return fail("invalid_payload");
    m["payload"] = p;
    return {true, {}, m};
}

namespace {
bool CanonicalName(QJsonValue v, int points = 32, int bytes = 128) {
    const auto name = Name(v, points, bytes);
    return !name.isEmpty() && v.isString() && name == v.toString();
}
bool Policy(QJsonValue v) {
    auto p = v.toObject();
    return v.isObject() && Keys(p, {"name", "visibility", "viewerLimit", "passwordProtected"}) &&
        CanonicalName(p["name"], 64, 256) && (p["visibility"] == "public" || p["visibility"] == "unlisted") &&
        Integer(p["viewerLimit"], 1, 63) && p["passwordProtected"].isBool();
}
bool Member(QJsonValue v) {
    auto p = v.toObject();
    return v.isObject() && Keys(p, {"peerId", "nickname", "role", "status"}) && Id(p["peerId"]) &&
        CanonicalName(p["nickname"]) && (p["role"] == "host" || p["role"] == "viewer") &&
        (p["status"] == "connected" || p["status"] == "reconnecting");
}
bool Summary(QJsonValue v) {
    auto p = v.toObject();
    return v.isObject() && Keys(p, {"roomId", "name", "viewerCount", "viewerLimit", "passwordProtected", "status", "summaryVersion", "leaseExpiresAt"}) &&
        Id(p["roomId"]) && CanonicalName(p["name"], 64, 256) && Integer(p["viewerCount"], 0, 63) &&
        Integer(p["viewerLimit"], 1, 63) && p["passwordProtected"].isBool() &&
        QStringList{"open", "full", "reconnecting"}.contains(p["status"].toString()) && Integer(p["summaryVersion"]) && Integer(p["leaseExpiresAt"]) &&
        (p["status"] == "reconnecting" || ((p["status"] == "full") == (p["viewerCount"].toInt() >= p["viewerLimit"].toInt())));
}
bool RoomState(const QJsonObject& p) {
    if (!Keys(p, {"selfPeerId", "policy", "status", "members"}) || !Id(p["selfPeerId"]) || !Policy(p["policy"]) ||
        (p["status"] != "open" && p["status"] != "reconnecting") || !p["members"].isArray()) return false;
    auto members = p["members"].toArray();
    if (members.isEmpty() || members.size() > 64) return false;
    QSet<QString> ids; int hosts = 0; bool hostReconnecting = false;
    for (auto v : members) {
        if (!Member(v)) return false;
        auto m = v.toObject(); ids.insert(m["peerId"].toString());
        if (m["role"] == "host") { ++hosts; hostReconnecting = m["status"] == "reconnecting"; }
    }
    return ids.size() == members.size() && ids.contains(p["selfPeerId"].toString()) && hosts == 1 &&
        ((p["status"] == "reconnecting") == hostReconnecting);
}
}
Validation ValidateServerEvent(const QByteArray& bytes, bool directory) {
    auto fail = [](const char* error) { return Validation{false, QString::fromLatin1(error), {}}; };
    auto parsed = ParseEnvelope(bytes, (directory ? 256 : 64) * 1024);
    if (!parsed.ok) return parsed;
    auto m = parsed.message; auto type = m["type"].toString();
    bool state = type == "state.snapshot" || type == "state.delta";
    bool signal = QStringList{"signal.offer", "signal.answer", "signal.candidate", "signal.restart_request"}.contains(type);
    if ((!state && !signal && type != "command.result" && type != "room.closed") || (directory && !state)) return fail("invalid_envelope");
    if (!signal && !(directory && type == "state.snapshot") && bytes.size() > 16 * 1024) return fail("too_large");
    QStringList required{"v", "type", "payload"};
    if (!directory) required << "roomId";
    if (state) required << "revision";
    if (signal) required << "connectionId" << "fromPeerId" << "toPeerId";
    if (type == "command.result") required << "requestId";
    if (!Keys(m, required) || (state && !Integer(m["revision"]))) return fail("invalid_envelope");
    for (const auto& k : required) if (k.endsWith("Id") && !Id(m[k])) return fail("invalid_envelope");
    auto p = m["payload"].toObject(); bool valid = false;
    if (signal) {
        if (m["fromPeerId"] == m["toPeerId"]) return fail("invalid_payload");
        auto command = m; command.remove("fromPeerId");
        return ValidateClientCommand(QJsonDocument(command).toJson(QJsonDocument::Compact)).ok ? Validation{true, {}, m} : fail("invalid_payload");
    }
    if (type == "command.result") {
        valid = p["status"] == "ok" ? Keys(p, {"status"}) : p["status"] == "conflict" ?
            Keys(p, {"status", "currentRevision"}) && Integer(p["currentRevision"]) :
            p["status"] == "error" && Keys(p, {"status", "code"}) && QStringList{"forbidden", "not_found", "invalid_state", "rate_limited", "invalid_command"}.contains(p["code"].toString());
    } else if (type == "room.closed") {
        valid = Keys(p, {"reason"}) && QStringList{"host_left", "host_expired", "kicked", "server_shutdown"}.contains(p["reason"].toString());
    } else if (type == "state.snapshot") {
        if (!directory) valid = RoomState(p);
        else {
            auto rooms = p["rooms"].toArray(); QSet<QString> ids;
            valid = Keys(p, {"rooms"}) && p["rooms"].isArray() && rooms.size() <= 500;
            for (auto room : rooms) { valid &= Summary(room); ids.insert(room.toObject()["roomId"].toString()); }
            valid &= ids.size() == rooms.size();
        }
    } else if (directory) {
        valid = p["op"] == "upsert" ? Keys(p, {"op", "room"}) && Summary(p["room"]) :
            p["op"] == "remove" && Keys(p, {"op", "roomId"}) && Id(p["roomId"]);
    } else {
        valid = p["op"] == "policy" ? Keys(p, {"op", "policy"}) && Policy(p["policy"]) :
            p["op"] == "member.upsert" ? Keys(p, {"op", "member"}) && Member(p["member"]) :
            p["op"] == "member.remove" ? Keys(p, {"op", "peerId"}) && Id(p["peerId"]) :
            p["op"] == "host.status" && Keys(p, {"op", "status"}) && (p["status"] == "open" || p["status"] == "reconnecting");
    }
    return valid ? Validation{true, {}, m} : fail("invalid_payload");
}
}
