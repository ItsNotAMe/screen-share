#pragma once
#include "shared/RoomSessionConfig.h"

struct RoomCliHooks {
    std::function<void(const QJsonObject&)> report;
    std::function<bool()> pump; // Owner-thread presentation/cancellation; false stops.
};
int RunRoomCliSession(const RoomSessionConfig&, screenshare::v2::RoomRuntimeFactory,
                      RoomCliHooks, bool diagnosticLoopback = false);
int RunRoomCli(int argc, char** argv);
