#include "input/v2/GamepadSink.h"
#include "input/v2/GamepadPoller.h"
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
int main() {
    try {
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
