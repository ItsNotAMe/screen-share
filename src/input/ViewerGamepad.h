#pragma once

#include "core/ScreenShareSession.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace screenshare {

enum class PlayStationGamepadModel {
    DualShock4,
    DualSense,
};

struct ViewerGamepadDevice {
    std::string id;
    std::string name;
};

class ViewerGamepad {
public:
    [[nodiscard]] static std::vector<ViewerGamepadDevice> ConnectedDevices();
    [[nodiscard]] static std::optional<RemoteGamepadState> ReadState(std::string_view deviceId);

    [[nodiscard]] static std::optional<RemoteGamepadState> ParsePlayStationReport(
        PlayStationGamepadModel model,
        std::span<const uint8_t> report,
        uint8_t controllerSlot = 0);
};

} // namespace screenshare
