#pragma once
#include "shared/RoomSessionConfig.h"

struct RoomCliHooks {
    std::function<void(const QJsonObject&)> report;
    std::function<bool()> pump; // Owner-thread presentation/cancellation; false stops.
    std::function<void(std::shared_ptr<screenshare::input::Port>, const screenshare::v2::RoomStatus&)> input;
    std::function<std::optional<screenshare::input::Event>()> gamepad; // Explicit test/embedding source.
    std::function<bool()> controlActive; // False cancels local consent as well as the remote grant.
    std::function<void(uint8_t,std::function<void(const screenshare::input::Event&)>)> inputCapture;
};
int RunRoomCliSession(const RoomSessionConfig&, screenshare::v2::RoomRuntimeFactory,
                      RoomCliHooks, bool diagnosticLoopback = false);
int RunRoomCli(int argc, char** argv);
