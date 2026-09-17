#pragma once
#include "InputService.h"
#include "input/VirtualGamepadBackend.h"
#include <functional>
#include <map>

namespace screenshare::input {
// One instance per room. All operations, including lazy driver access, run on
// the service's input owner. Injectable dependencies never fall back to Windows.
class GamepadSink final : public Sink {
public:
    using Factory = std::function<std::unique_ptr<VirtualGamepadBackend>()>;
    using Slots = std::function<std::vector<int>()>;
    GamepadSink(Factory, Slots);
    bool Grant(const std::string&, uint8_t, int) override;
    bool Apply(const std::string&, const Event&) override;
    void Release(const std::string&) noexcept override;
    bool Healthy(const std::string&) override;
private:
    struct Pad { std::unique_ptr<VirtualGamepadDevice> device; unsigned slot; };
    Factory factory_;
    Slots slots_;
    std::unique_ptr<VirtualGamepadBackend> backend_;
    std::map<std::string, Pad> pads_;
};
std::shared_ptr<Sink> CreateWindowsGamepadSink();
}
