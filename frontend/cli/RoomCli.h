#pragma once
#include "api/RoomSession.h"
#include "media/webrtc/WindowsRoomRuntime.h"
#include <QJsonObject>
#include <chrono>

struct RoomCliSettingsChange {
    std::chrono::milliseconds at;
    screenshare::media::StreamPreferences preferences;
};
struct RoomCliConfig {
    screenshare::v2::RoomOptions room;
    screenshare::media::WindowsRoomRuntimeOptions media;
    std::chrono::seconds duration{0}; // Zero runs until cancellation/window close.
    bool preview = true;
    std::vector<RoomCliSettingsChange> changes;
};
// Strict configuration; loopback is available only to injected test harnesses.
RoomCliConfig ParseRoomCliConfig(const QJsonObject&, bool diagnosticLoopback = false);
struct RoomCliHooks {
    std::function<void(const QJsonObject&)> report;
    std::function<bool()> pump; // Owner-thread presentation/cancellation; false stops.
};
int RunRoomCliSession(const RoomCliConfig&, screenshare::v2::RoomRuntimeFactory,
                      RoomCliHooks, bool diagnosticLoopback = false);
int RunRoomCli(int argc, char** argv);
