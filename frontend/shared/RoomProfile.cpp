#include "shared/RoomProfile.h"
#include "room/protocol/RoomProtocol.h"
#include <QJsonDocument>
#include <QRandomGenerator>
#include "shared/StreamPreferencesJson.h"
RoomProfile::RoomProfile(const QString& file) {
    if (file.isEmpty()) settings_ = std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope, "ScreenShare", "RoomV2Profile");
    else settings_ = std::make_unique<QSettings>(file, QSettings::IniFormat);
    fallbackNickname_ = QString("Guest-%1").arg(QRandomGenerator::system()->generate(), 8, 16, QChar('0'));
    if (!normalizeNickname(settings_->value("nickname").toString())) saveNickname(fallbackNickname_);
}
std::optional<QString> RoomProfile::normalizeNickname(const QString& value) {
    const auto result = screenshare::room::wire::ValidateClientCommand(QJsonDocument(QJsonObject{
        {"v", 2}, {"type", "profile.update"}, {"roomId", "local"}, {"requestId", "local"},
        {"payload", QJsonObject{{"nickname", value}, {"expectedRevision", 0}}}}).toJson(QJsonDocument::Compact));
    if (!result.ok) return {};
    return result.message["payload"].toObject()["nickname"].toString();
}
QString RoomProfile::nickname() const {
    return normalizeNickname(settings_->value("nickname").toString()).value_or(fallbackNickname_);
}
screenshare::media::StreamPreferences RoomProfile::streamPreferences() const {
    const auto bytes = settings_->value("stream/v1").toByteArray();
    if (bytes.size() > 4096) return {};
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject()) return {};
    try { return ParseStreamPreferences(document.object()); } catch (...) { return {}; }
}
bool RoomProfile::saveStreamPreferences(const screenshare::media::StreamPreferences& value) {
    QJsonObject object;
    try { object = StreamPreferencesJson(value); } catch (...) { return false; }
    return saveValue("stream/v1", QJsonDocument(object).toJson(QJsonDocument::Compact));
}
RoomProfile::Playback RoomProfile::playback() const {
    const auto bytes = settings_->value("playback/v1").toByteArray();
    if (bytes.size() > 256) return {};
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject()) return {};
    const auto object = document.object();
    const auto volume = object["volume"].toDouble(-1);
    if (object.size() != 2 || !object["volume"].isDouble() || volume < 0 || volume > 100 || volume != int(volume) || !object["muted"].isBool()) return {};
    return {int(volume), object["muted"].toBool()};
}
bool RoomProfile::savePlayback(Playback value) {
    if (value.volume < 0 || value.volume > 100) return false;
    return saveValue("playback/v1", QJsonDocument(QJsonObject{{"volume", value.volume}, {"muted", value.muted}}).toJson(QJsonDocument::Compact));
}
bool RoomProfile::saveNickname(const QString& value) {
    const auto canonical = normalizeNickname(value);
    if (!canonical) return false;
    return saveValue("nickname", *canonical);
}
bool RoomProfile::saveValue(const QString& key, const QVariant& value) {
    const bool existed = settings_->contains(key);
    const auto previous = settings_->value(key);
    settings_->setValue(key, value); settings_->sync();
    if (settings_->status() == QSettings::NoError) return true;
    if (existed) settings_->setValue(key, previous); else settings_->remove(key);
    settings_->sync(); return false;
}
