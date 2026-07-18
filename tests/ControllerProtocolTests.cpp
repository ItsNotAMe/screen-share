#include "core/SessionRuntimeControl.h"
#include "input/VirtualGamepadBackend.h"
#include "transport/UdpProtocol.h"

#include <cstddef>
#include <cstring>
#include <iostream>

namespace {

bool Check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

template <typename Mutator>
std::vector<std::byte> MutatePacket(std::vector<std::byte> datagram, Mutator mutate)
{
    screenshare::udp_protocol::ControlPacket packet{};
    std::memcpy(&packet, datagram.data(), sizeof(packet));
    mutate(packet);
    std::memcpy(datagram.data(), &packet, sizeof(packet));
    return datagram;
}

} // namespace

int main()
{
    using namespace screenshare;
    using namespace screenshare::udp_protocol;
    bool ok = true;

    ControlMessage outbound;
    outbound.command = ControlCommandType::GamepadState;
    outbound.sequence = 42;
    outbound.sessionFingerprint = 123;
    outbound.accessCodeFingerprint = 456;
    outbound.gamepad.controllerSlot = 3;
    outbound.gamepad.buttons = 0xF3FF;
    outbound.gamepad.leftTrigger = 17;
    outbound.gamepad.rightTrigger = 231;
    outbound.gamepad.thumbLX = -32768;
    outbound.gamepad.thumbLY = 32767;
    outbound.gamepad.thumbRX = -1234;
    outbound.gamepad.thumbRY = 5678;
    const auto datagram = BuildControlDatagram(outbound);
    const auto parsed = ParseControlDatagram(datagram);
    ok &= Check(parsed.has_value(), "valid gamepad state parses");
    ok &= Check(parsed && parsed->gamepad == outbound.gamepad, "gamepad state round-trips exactly");
    ok &= Check(parsed && parsed->sequence == 42, "control sequence remains unchanged");

    auto badSchema = MutatePacket(datagram, [](ControlPacket& packet) {
        packet.button = ToNetwork16(2);
    });
    ok &= Check(!ParseControlDatagram(badSchema).has_value(), "unknown gamepad schema is rejected");

    auto badButtons = MutatePacket(datagram, [](ControlPacket& packet) {
        packet.scancode = ToNetwork16(0x0400);
    });
    ok &= Check(!ParseControlDatagram(badButtons).has_value(), "reserved gamepad button is rejected");

    auto badReserved = MutatePacket(datagram, [](ControlPacket& packet) {
        packet.reserved2 = ToNetwork32(1);
    });
    ok &= Check(!ParseControlDatagram(badReserved).has_value(), "nonzero reserved payload is rejected");

    ControlMessage mouse;
    mouse.command = ControlCommandType::MouseButton;
    mouse.button = 1;
    mouse.pressed = true;
    mouse.mouseX = 0.25f;
    mouse.mouseY = 0.75f;
    const auto parsedMouse = ParseControlDatagram(BuildControlDatagram(mouse));
    ok &= Check(parsedMouse && parsedMouse->command == ControlCommandType::MouseButton,
        "existing mouse command remains compatible");
    ok &= Check(parsedMouse && parsedMouse->button == 1 && parsedMouse->pressed,
        "existing mouse payload remains compatible");

    MemorySessionRuntimeControl runtimeControl;
    RemoteInputEvent first;
    first.kind = RemoteInputKind::GamepadState;
    first.gamepad.controllerSlot = 0;
    first.gamepad.buttons = 0x1000;
    runtimeControl.EnqueueInput(first);
    RemoteInputEvent discrete;
    discrete.kind = RemoteInputKind::Key;
    discrete.key = 65;
    discrete.pressed = true;
    runtimeControl.EnqueueInput(discrete);
    RemoteInputEvent latest = first;
    latest.gamepad.buttons = 0x2000;
    runtimeControl.EnqueueInput(latest);
    const auto drained = runtimeControl.DrainInput();
    ok &= Check(drained.size() == 2, "old gamepad state is coalesced across other queued input");
    ok &= Check(drained.size() == 2 && drained[0].kind == RemoteInputKind::Key,
        "discrete input survives gamepad coalescing");
    ok &= Check(drained.size() == 2 && drained[1].gamepad.buttons == 0x2000,
        "newest gamepad state is retained");

    RemoteInputEvent slotZero = first;
    RemoteInputEvent slotOne = first;
    slotOne.gamepad.controllerSlot = 1;
    runtimeControl.EnqueueInput(slotZero);
    runtimeControl.EnqueueInput(slotOne);
    const auto separateSlots = runtimeControl.DrainInput();
    ok &= Check(separateSlots.size() == 2, "different controller slots coalesce independently");

    const auto backend = CreateVirtualGamepadBackend();
    const auto backendStatus = backend->Status();
    ok &= Check(!backendStatus.message.empty(), "optional gamepad backend always reports actionable status");

    return ok ? 0 : 1;
}
