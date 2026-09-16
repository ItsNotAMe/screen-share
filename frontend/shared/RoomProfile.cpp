#include "shared/RoomProfile.h"
#include "room/protocol/RoomProtocol.h"
#include <QJsonDocument>
RoomProfile::RoomProfile(const QString& file) {
    if (file.isEmpty()) settings_ = std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope, "ScreenShare", "RoomV2Profile");
    else settings_ = std::make_unique<QSettings>(file, QSettings::IniFormat);
}
std::optional<QString> RoomProfile::normalizeNickname(const QString& value) {
    const auto result = screenshare::room::wire::ValidateClientCommand(QJsonDocument(QJsonObject{
        {"v", 2}, {"type", "profile.update"}, {"roomId", "local"}, {"requestId", "local"},
        {"payload", QJsonObject{{"nickname", value}, {"expectedRevision", 0}}}}).toJson(QJsonDocument::Compact));
    if (!result.ok) return {};
    return result.message["payload"].toObject()["nickname"].toString();
}
QString RoomProfile::nickname() const {
    return normalizeNickname(settings_->value("nickname", "Guest").toString()).value_or("Guest");
}
bool RoomProfile::saveNickname(const QString& value) {
    const auto canonical = normalizeNickname(value);
    if (!canonical) return false;
    settings_->setValue("nickname", *canonical); settings_->sync();
    return settings_->status() == QSettings::NoError;
}
