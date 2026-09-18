#pragma once
#include "api/RoomSession.h"
#include "media/webrtc/WindowsRoomRuntime.h"
#include <QJsonObject>
#include <chrono>

struct RoomSettingsChange {
    std::chrono::milliseconds at;
    screenshare::media::StreamPreferences preferences;
};
struct RoomSessionConfig {
    screenshare::v2::RoomOptions room;
    screenshare::media::WindowsRoomRuntimeOptions media;
    std::chrono::seconds duration{0}; // Zero runs until cancellation/window close.
    bool preview = true;
    QString inputCommandsFile;
    QString gamepadDevice;
    QString reportFile;
    std::vector<RoomSettingsChange> changes;
    struct CaptureChange { std::chrono::milliseconds at; screenshare::media::CaptureSelection selection; };
    std::vector<CaptureChange> captureChanges;
    struct AudioChange { std::chrono::milliseconds at; screenshare::media::AudioSelection selection; };
    std::vector<AudioChange> audioChanges;
    struct PlaybackChange { std::chrono::milliseconds at; screenshare::media::PlaybackSelection selection; };
    std::vector<PlaybackChange> playbackChanges;
};
// Strict configuration; loopback is available only to injected test harnesses.
RoomSessionConfig ParseRoomSessionConfig(const QJsonObject&, bool diagnosticLoopback = false);
