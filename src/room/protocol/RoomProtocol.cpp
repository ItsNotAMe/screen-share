#include "RoomProtocol.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QStringDecoder>
#include <cmath>

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
Validation ValidateClientCommand(const QByteArray& bytes, bool directory) {
    auto fail = [](const char* error) { return Validation{false, QString::fromLatin1(error), {}}; };
    if (bytes.size() > 64 * 1024) return fail("too_large");
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
}
