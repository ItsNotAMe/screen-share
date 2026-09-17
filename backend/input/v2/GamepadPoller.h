#pragma once
#include "InputService.h"
#include <functional>
#include <thread>

namespace screenshare::input {
// Viewer-selected controller only. Missing reports/unplug/failure revoke the
// grant; this owner never enumerates or switches devices implicitly.
class GamepadPoller final {
public:
    using Read = std::function<std::optional<Event>()>;
    GamepadPoller(std::shared_ptr<Port>, std::string peer, Read);
    ~GamepadPoller();
    GamepadPoller(const GamepadPoller&) = delete;
    GamepadPoller& operator=(const GamepadPoller&) = delete;
private:
    std::shared_ptr<Port> port_;
    std::string peer_;
    std::jthread worker_;
};
}
