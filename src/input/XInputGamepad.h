#pragma once

#include "core/ScreenShareSession.h"

#include <optional>
#include <vector>

namespace screenshare {

class XInputGamepad {
public:
    [[nodiscard]] static std::vector<int> ConnectedSlots();
    [[nodiscard]] static std::optional<RemoteGamepadState> ReadState(int slot);
};

} // namespace screenshare
