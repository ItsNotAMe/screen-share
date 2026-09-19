#include "input/v2/GamepadSink.h"
#include "input/v2/GamepadPoller.h"
#include "input/XInputGamepad.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <set>
#include <source_location>
using namespace screenshare;
using namespace screenshare::input;
using namespace std::chrono_literals;
void Check(bool value, std::source_location at = std::source_location::current()) {
    if (!value) throw std::runtime_error("Gamepad check failed at " + std::to_string(at.line()));
}
template<class F> void Wait(F f) {
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (!f()) { Check(std::chrono::steady_clock::now() < end); std::this_thread::sleep_for(2ms); }
}
struct Evidence {
    std::set<int> slots;
    unsigned created = 0, destroyed = 0, neutral = 0, submitted = 0;
    bool available = true, fail = false, wrongIndex = false;
    RemoteGamepadState last;
};
class Device final : public VirtualGamepadDevice {
    std::shared_ptr<Evidence> state_; unsigned slot_; bool destroyed_ = false;
public:
    Device(std::shared_ptr<Evidence> state, unsigned slot) : state_(state), slot_(slot) { state_->slots.insert(int(slot)); ++state_->created; }
    ~Device() override { Destroy(); }
    bool SubmitState(const RemoteGamepadState& value, std::string*) override { ++state_->submitted; state_->last = value; return !state_->fail; }
    void Neutralize() noexcept override { ++state_->neutral; }
    void Destroy() noexcept override { if (!destroyed_) { state_->slots.erase(int(slot_)); ++state_->destroyed; destroyed_ = true; } }
    std::optional<unsigned> UserIndex() const override { return state_->wrongIndex ? std::optional<unsigned>(0) : slot_; }
};
class Backend final : public VirtualGamepadBackend {
    std::shared_ptr<Evidence> state_;
public:
    explicit Backend(std::shared_ptr<Evidence> state) : state_(state) {}
    VirtualGamepadBackendStatus Status() const override { return {state_->available, {}}; }
    std::unique_ptr<VirtualGamepadDevice> CreateXbox360(std::string*) override {
        for (unsigned i = 0; i < 4; ++i) if (!state_->slots.contains(int(i))) return std::make_unique<Device>(state_, i);
        return {};
    }
};
class TestPort final : public Port {
public:
    std::atomic_bool granted{true};
    std::atomic<unsigned> submitted{0}, revoked{0};
    std::atomic<uint64_t> permission{1};
    bool Request(const std::string&, uint8_t) override { return false; }
    bool Grant(const std::string&, uint8_t) override { return false; }
    void Revoke(const std::string&) override { granted = false; ++revoked; }
    bool Submit(const std::string&, Event event) override { Check(event.kind == Kind::Pad); ++submitted; return granted; }
    bool SubmitIfCurrent(const std::string& peer, uint64_t epoch, Event event) override {
        return permission == epoch && Submit(peer, event);
    }
    void RevokeIfCurrent(const std::string& peer, uint64_t epoch) override { if(permission == epoch) Revoke(peer); }
    std::vector<Status> Read() const override { Status s; s.peer = "host"; s.permission = permission; s.granted = granted ? Gamepad : 0; return {s}; }
};
class NativeDiagnosticBackend final : public VirtualGamepadBackend {
    std::unique_ptr<VirtualGamepadBackend> inner_ = CreateVirtualGamepadBackend();
public:
    VirtualGamepadBackendStatus Status() const override { return inner_->Status(); }
    std::unique_ptr<VirtualGamepadDevice> CreateXbox360(std::string* error = nullptr) override {
        std::string reason;
        auto device = inner_->CreateXbox360(&reason);
        if (!device) std::cerr << "Native create failed: " << reason << '\n';
        else if (!device->UserIndex()) std::cerr << "Native device has no XInput user index\n";
        else { std::cerr << "Native assigned slot=" << *device->UserIndex() << " visible=";
            for (const int slot : XInputGamepad::ConnectedSlots()) std::cerr << slot << ',';
            std::cerr << '\n'; }
        if (error) *error = reason;
        return device;
    }
};
void NativeNeutralLifecycle() {
    // Explicit opt-in only. Reserve a neutral owned device as the local-slot
    // sentinel; never submit buttons/axes or modify pre-existing controllers.
    const auto original = XInputGamepad::ConnectedSlots();
    if (original.size() >= 4) throw std::runtime_error("Native test needs a free XInput slot");
    auto backend = CreateVirtualGamepadBackend();
    if (!backend->Status().available) throw std::runtime_error(backend->Status().message);
    std::unique_ptr<VirtualGamepadDevice> sentinel;
    if (original.empty()) {
        sentinel = backend->CreateXbox360();
        Check(bool(sentinel));
        Check(sentinel->SubmitState(RemoteGamepadState{}));
    }
    Wait([&] { return XInputGamepad::ConnectedSlots().size() == (original.empty() ? 1 : original.size()); });
    const auto reserved = XInputGamepad::ConnectedSlots();
    const int remoteCount = int(4 - reserved.size());
    if (sentinel) Check(sentinel->UserIndex() == unsigned(reserved.front()));
    for (int cycle = 0; cycle < 5; ++cycle) {
        auto sink = std::make_shared<GamepadSink>([] { return std::make_unique<NativeDiagnosticBackend>(); },
            [] { return XInputGamepad::ConnectedSlots(); });
        for (int i = 0; i < remoteCount; ++i) {
            const auto peer = "native-" + std::to_string(i);
            if (!sink->Grant(peer, Gamepad, i)) throw std::runtime_error("Native grant failed: cycle=" + std::to_string(cycle) + " pad=" + std::to_string(i));
            Event neutral; neutral.kind = Kind::Pad;
            Check(sink->Apply(peer, neutral) && sink->Healthy(peer));
        }
        Wait([&] { return XInputGamepad::ConnectedSlots().size() == 4; });
        Check(!sink->Grant("exhausted", Gamepad, 0));
        if (sentinel) Check(sentinel->UserIndex() == unsigned(reserved.front()));
        for (const int slot : XInputGamepad::ConnectedSlots()) {
            if (std::find(reserved.begin(), reserved.end(), slot) != reserved.end()) continue;
            const auto state = XInputGamepad::ReadState(slot);
            Check(state && !state->buttons && !state->leftTrigger && !state->rightTrigger &&
                !state->thumbLX && !state->thumbLY && !state->thumbRX && !state->thumbRY);
        }
        for (int i = 0; i < remoteCount; ++i) sink->Release("native-" + std::to_string(i));
        Wait([&] { return XInputGamepad::ConnectedSlots() == reserved; });
    }
    sentinel.reset(); backend.reset();
    Wait([&] { return XInputGamepad::ConnectedSlots() == original; });
    std::cout << "{\"passed\":true,\"nativeVirtualDevices\":true,\"neutralStateOnly\":true,"
                 "\"keyboardMouseInput\":false,\"cycles\":5,\"remotePadsPerCycle\":" << remoteCount << ',' <<
                 "\"reservedSlotPreserved\":true,\"allOwnedDevicesRemoved\":true,\"preexistingSlots\":"
              << original.size() << "}\n";
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--native-neutral") { NativeNeutralLifecycle(); return 0; }
        Check(argc == 1);
        auto state = std::make_shared<Evidence>(); state->slots = {0};
        GamepadSink sink([state] { return std::make_unique<Backend>(state); }, [state] { return std::vector<int>(state->slots.begin(), state->slots.end()); });
        Check(!sink.Grant("key", Keyboard, 0));
        Check(sink.Grant("one", Gamepad, 0) && sink.Grant("two", Gamepad, 1) && sink.Grant("three", Gamepad, 2));
        Check(!sink.Grant("four", Gamepad, 2) && state->slots.size() == 4 && state->slots.contains(0));
        Event value; value.kind = Kind::Pad; value.buttons = 0x1000; value.axes = {-32768, 32767, 0, 42};
        Check(sink.Apply("one", value) && state->last.controllerSlot == 1 && state->last.thumbLX == -32768);
        sink.Release("two"); Check(state->neutral == 1 && state->destroyed == 1 && sink.Healthy("one") && sink.Healthy("three"));
        Check(sink.Grant("replacement", Gamepad, 1));
        state->fail = true; Check(!sink.Apply("one", value)); sink.Release("one");
        sink.Release("three"); sink.Release("replacement"); Check(state->slots == std::set<int>{0});
        state->fail = false; state->wrongIndex = true;
        Check(!sink.Grant("collision", Gamepad, 0) && state->slots == std::set<int>{0});
        state->wrongIndex = false; state->available = false;
        const auto created = state->created; Check(!sink.Grant("missing", Gamepad, 0) && state->created == created);
        auto port = std::make_shared<TestPort>(); std::atomic<unsigned> buttons{1}; std::atomic_bool plugged{true};
        {
            GamepadPoller poller(port, "host", [&]() -> std::optional<Event> {
                if (!plugged) return {}; Event e; e.kind = Kind::Pad; e.buttons = uint16_t(buttons.load()); return e;
            });
            Wait([&] { return port->submitted == 1; }); std::this_thread::sleep_for(25ms); Check(port->submitted == 1);
            buttons = 2; Wait([&] { return port->submitted == 2; });
            plugged = false; Wait([&] { return port->revoked > 0; }); Check(!port->granted);
        }
        Check(state->created == state->destroyed);
        // Hold an old reader across a new permission, then let its late read
        // and destructor finish. Neither may affect the new grant.
        port = std::make_shared<TestPort>();
        std::atomic_bool reading{false}, finish{false};
        {
            GamepadPoller old(port, "host", [&]() -> std::optional<Event> {
                reading = true;
                while (!finish) std::this_thread::sleep_for(1ms);
                Event e; e.kind = Kind::Pad; e.buttons = 1; return e;
            });
            Wait([&] { return reading.load(); });
            port->permission = 2; finish = true;
        }
        Check(port->granted && port->submitted == 0 && port->revoked == 0);
        std::cout << "{\"passed\":true,\"physical_input\":false,\"gamepad_devices\":true}\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
