#pragma once

#include "core/ScreenShareSession.h"

#include <memory>
#include <string>

namespace screenshare {

struct VirtualGamepadBackendStatus {
    bool available = false;
    std::string message;
};

class VirtualGamepadDevice {
public:
    virtual ~VirtualGamepadDevice() = default;
    virtual bool SubmitState(const RemoteGamepadState& state, std::string* errorMessage = nullptr) = 0;
    virtual void Neutralize() noexcept = 0;
    virtual void Destroy() noexcept = 0;
};

class VirtualGamepadBackend {
public:
    virtual ~VirtualGamepadBackend() = default;
    [[nodiscard]] virtual VirtualGamepadBackendStatus Status() const = 0;
    [[nodiscard]] virtual std::unique_ptr<VirtualGamepadDevice> CreateXbox360(
        std::string* errorMessage = nullptr) = 0;
};

[[nodiscard]] std::unique_ptr<VirtualGamepadBackend> CreateVirtualGamepadBackend();

} // namespace screenshare
