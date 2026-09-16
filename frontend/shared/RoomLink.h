#pragma once
#include <QString>
#include <QRegularExpression>
#include <optional>

// Deliberately service-independent: the configured service remains authoritative.
// No endpoint, credentials, query, fragment or arbitrary URL decoding is accepted.
inline std::optional<QString> ParseRoomReference(const QString& value) {
    if (value.size() > 160) return {};
    auto id = value;
    const QString prefix = QStringLiteral("screenshare://room/v2/");
    if (id.startsWith(prefix)) id = id.mid(prefix.size());
    static const QRegularExpression valid(QStringLiteral("\\A[A-Za-z0-9_-]{1,128}\\z"));
    return valid.match(id).hasMatch() ? std::optional<QString>(id) : std::nullopt;
}
inline QString MakeRoomLink(const QString& id) {
    const auto parsed = ParseRoomReference(id);
    return parsed && *parsed == id ? QStringLiteral("screenshare://room/v2/") + id : QString{};
}
