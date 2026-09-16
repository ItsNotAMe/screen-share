#pragma once
#include <QSettings>
#include <optional>
#include <memory>
#include "media/StreamPreferences.h"

// Allowlisted local defaults only. No credentials, service/room identity,
// capture handles, process IDs or device IDs are persisted.
class RoomProfile final {
public:
    explicit RoomProfile(const QString& testFile = {});
    QString nickname() const;
    bool saveNickname(const QString&);
    screenshare::media::StreamPreferences streamPreferences() const;
    bool saveStreamPreferences(const screenshare::media::StreamPreferences&);
    struct Playback { int volume = 100; bool muted = false; };
    Playback playback() const;
    bool savePlayback(Playback);
    static std::optional<QString> normalizeNickname(const QString&);
private:
    bool saveValue(const QString&, const QVariant&);
    std::unique_ptr<QSettings> settings_;
    QString fallbackNickname_;
};
