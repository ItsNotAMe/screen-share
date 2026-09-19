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
    uint64_t permission() const noexcept { return permission_; }
private:
    std::shared_ptr<Port> port_;
    std::string peer_;
    uint64_t permission_ = 0;
    std::jthread worker_;
};
}
