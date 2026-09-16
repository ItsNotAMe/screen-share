#pragma once
#include <QSettings>
#include <optional>
#include <memory>

// Only the display nickname is persisted. Passwords, membership credentials,
// room IDs and service URLs are never stored by this profile.
class RoomProfile final {
public:
    explicit RoomProfile(const QString& testFile = {});
    QString nickname() const;
    bool saveNickname(const QString&);
    static std::optional<QString> normalizeNickname(const QString&);
private:
    std::unique_ptr<QSettings> settings_;
};
