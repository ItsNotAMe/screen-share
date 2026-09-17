#include "GamepadSink.h"
#include "input/XInputGamepad.h"
#include <algorithm>
#include <stdexcept>

namespace screenshare::input {
GamepadSink::GamepadSink(Factory factory, Slots slots) : factory_(std::move(factory)), slots_(std::move(slots)) {
    if (!factory_ || !slots_) throw std::invalid_argument("Gamepad sink requires explicit dependencies");
}
bool GamepadSink::Grant(const std::string& peer, uint8_t capabilities, int padSlot) {
    if (capabilities != Gamepad || padSlot < 0 || padSlot >= 3 || pads_.contains(peer) || pads_.size() >= 3) return false;
    const auto occupied = slots_();
    // Includes our own virtual pads: no fifth XInput device can be granted.
    if (occupied.size() >= 4) return false;
    if (!backend_) backend_ = factory_();
    if (!backend_ || !backend_->Status().available) { backend_.reset(); return false; }
    auto device = backend_->CreateXbox360();
    if (!device) { if (pads_.empty()) backend_.reset(); return false; }
    const auto slot = device->UserIndex();
    if (!slot || *slot > 3 || std::find(occupied.begin(), occupied.end(), int(*slot)) != occupied.end() ||
        std::any_of(pads_.begin(), pads_.end(), [&](const auto& entry) { return entry.second.slot == *slot; })) {
        device->Neutralize(); device->Destroy();
        if (pads_.empty()) backend_.reset();
        return false;
    }
    pads_.emplace(peer, Pad{std::move(device), *slot});
    return true;
}
bool GamepadSink::Apply(const std::string& peer, const Event& event) {
    const auto it = pads_.find(peer);
    if (it == pads_.end() || event.kind != Kind::Pad || !Valid(event)) return false;
    RemoteGamepadState state;
    state.controllerSlot = uint8_t(it->second.slot); state.buttons = event.buttons;
    state.leftTrigger = event.leftTrigger; state.rightTrigger = event.rightTrigger;
    state.thumbLX = event.axes[0]; state.thumbLY = event.axes[1];
    state.thumbRX = event.axes[2]; state.thumbRY = event.axes[3];
    return it->second.device->SubmitState(state);
}
void GamepadSink::Release(const std::string& peer) noexcept {
    const auto it = pads_.find(peer);
    if (it == pads_.end()) return;
    it->second.device->Neutralize(); it->second.device->Destroy(); pads_.erase(it);
    if (pads_.empty()) backend_.reset();
}
bool GamepadSink::Healthy(const std::string& peer) {
    const auto it = pads_.find(peer);
    if (it == pads_.end()) return false;
    // Do not mistake a disconnected/reassigned virtual device for a local pad.
    return it->second.device->UserIndex() == it->second.slot;
}
std::shared_ptr<Sink> CreateWindowsGamepadSink() {
    return std::make_shared<GamepadSink>([] { return CreateVirtualGamepadBackend(); }, [] { return XInputGamepad::ConnectedSlots(); });
}
}
