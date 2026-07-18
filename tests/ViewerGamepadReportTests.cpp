#include "input/ViewerGamepad.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint32_t ReflectedCrc32Polynomial = 0xedb88320U;

bool Check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

uint32_t UpdateCrc32(uint32_t crc, std::span<const uint8_t> bytes)
{
    for (uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1U) != 0 ? ReflectedCrc32Polynomial : 0U);
        }
    }
    return crc;
}

void AddBluetoothCrc(std::vector<uint8_t>& report)
{
    const std::array<uint8_t, 1> seed{0xa1};
    uint32_t crc = UpdateCrc32(0xffffffffU, seed);
    crc = ~UpdateCrc32(crc, std::span<const uint8_t>(report).first(report.size() - 4));
    const size_t offset = report.size() - 4;
    report[offset] = static_cast<uint8_t>(crc);
    report[offset + 1] = static_cast<uint8_t>(crc >> 8);
    report[offset + 2] = static_cast<uint8_t>(crc >> 16);
    report[offset + 3] = static_cast<uint8_t>(crc >> 24);
}

bool TestDualShock4Usb()
{
    std::vector<uint8_t> report(64);
    report[0] = 0x01;
    report[1] = 255;
    report[2] = 0;
    report[3] = 0;
    report[4] = 255;
    report[5] = 0x01 | 0x20 | 0x80; // up-right, Cross/A, Triangle/Y
    report[6] = 0xf3; // L1, R1, Share, Options, L3, R3
    report[7] = 0xff; // PS/touchpad are intentionally not mapped to XInput
    report[8] = 20;
    report[9] = 230;

    const auto state = screenshare::ViewerGamepad::ParsePlayStationReport(
        screenshare::PlayStationGamepadModel::DualShock4, report, 2);
    if (!Check(state.has_value(), "DualShock 4 USB report was rejected.")) {
        return false;
    }
    const uint16_t expectedButtons =
        0x0001 | 0x0008 | 0x1000 | 0x8000 | 0x0100 | 0x0200 |
        0x0020 | 0x0010 | 0x0040 | 0x0080;
    return
        Check(state->controllerSlot == 2, "Controller slot was not preserved.") &
        Check(state->buttons == expectedButtons, "DualShock 4 buttons were mapped incorrectly.") &
        Check(state->leftTrigger == 20 && state->rightTrigger == 230, "DualShock 4 triggers were mapped incorrectly.") &
        Check(state->thumbLX == 32767 && state->thumbLY == 32767, "DualShock 4 left stick was mapped incorrectly.") &
        Check(state->thumbRX == -32768 && state->thumbRY == -32767, "DualShock 4 right stick was mapped incorrectly.");
}

bool TestDualShock4BluetoothAndMinimal()
{
    std::vector<uint8_t> bluetooth(78);
    bluetooth[0] = 0x11;
    bluetooth[3] = bluetooth[4] = bluetooth[5] = bluetooth[6] = 128;
    bluetooth[7] = 0x08;
    bluetooth[10] = 77;
    bluetooth[11] = 88;
    AddBluetoothCrc(bluetooth);
    bool passed = Check(
        screenshare::ViewerGamepad::ParsePlayStationReport(
            screenshare::PlayStationGamepadModel::DualShock4, bluetooth).has_value(),
        "Valid DualShock 4 Bluetooth report was rejected.");
    bluetooth[20] ^= 1;
    passed &= Check(
        !screenshare::ViewerGamepad::ParsePlayStationReport(
            screenshare::PlayStationGamepadModel::DualShock4, bluetooth).has_value(),
        "DualShock 4 Bluetooth report with a bad CRC was accepted.");

    std::vector<uint8_t> minimal(10);
    minimal[0] = 0x01;
    minimal[1] = minimal[2] = minimal[3] = minimal[4] = 128;
    minimal[5] = 0x08;
    passed &= Check(
        screenshare::ViewerGamepad::ParsePlayStationReport(
            screenshare::PlayStationGamepadModel::DualShock4, minimal).has_value(),
        "DualShock 4 minimal Bluetooth report was rejected.");
    return passed;
}

bool TestDualSenseUsbAndBluetooth()
{
    std::vector<uint8_t> usb(64);
    usb[0] = 0x01;
    usb[1] = usb[2] = usb[3] = usb[4] = 128;
    usb[5] = 41;
    usb[6] = 199;
    usb[8] = 0x04 | 0x10 | 0x40; // down, Square/X, Circle/B
    usb[9] = 0x33; // L1, R1, Create/Back, Options/Start
    const auto usbState = screenshare::ViewerGamepad::ParsePlayStationReport(
        screenshare::PlayStationGamepadModel::DualSense, usb);
    bool passed = Check(usbState.has_value(), "DualSense USB report was rejected.");
    if (usbState) {
        passed &= Check(
            usbState->buttons == (0x0002 | 0x4000 | 0x2000 | 0x0100 | 0x0200 | 0x0020 | 0x0010),
            "DualSense buttons were mapped incorrectly.");
        passed &= Check(
            usbState->leftTrigger == 41 && usbState->rightTrigger == 199,
            "DualSense triggers were mapped incorrectly.");
    }

    std::vector<uint8_t> bluetooth(78);
    bluetooth[0] = 0x31;
    bluetooth[2] = bluetooth[3] = bluetooth[4] = bluetooth[5] = 128;
    bluetooth[6] = 12;
    bluetooth[7] = 34;
    bluetooth[9] = 0x08;
    AddBluetoothCrc(bluetooth);
    passed &= Check(
        screenshare::ViewerGamepad::ParsePlayStationReport(
            screenshare::PlayStationGamepadModel::DualSense, bluetooth).has_value(),
        "Valid DualSense Bluetooth report was rejected.");

    std::vector<uint8_t> simpleBluetooth(10);
    simpleBluetooth[0] = 0x01;
    simpleBluetooth[1] = simpleBluetooth[2] = simpleBluetooth[3] = simpleBluetooth[4] = 128;
    simpleBluetooth[5] = 0x02 | 0x20;
    simpleBluetooth[8] = 55;
    simpleBluetooth[9] = 66;
    const auto simpleState = screenshare::ViewerGamepad::ParsePlayStationReport(
        screenshare::PlayStationGamepadModel::DualSense, simpleBluetooth);
    passed &= Check(simpleState.has_value(), "DualSense simple Bluetooth report was rejected.");
    if (simpleState) {
        passed &= Check(
            simpleState->buttons == (0x0008 | 0x1000) &&
                simpleState->leftTrigger == 55 && simpleState->rightTrigger == 66,
            "DualSense simple Bluetooth report was mapped incorrectly.");
    }
    return passed;
}

} // namespace

int main()
{
    bool passed = true;
    passed &= TestDualShock4Usb();
    passed &= TestDualShock4BluetoothAndMinimal();
    passed &= TestDualSenseUsbAndBluetooth();
    passed &= Check(
        !screenshare::ViewerGamepad::ParsePlayStationReport(
            screenshare::PlayStationGamepadModel::DualSense, std::array<uint8_t, 8>{}).has_value(),
        "A truncated PlayStation report was accepted.");

    std::unordered_set<std::string> deviceIds;
    for (const auto& device : screenshare::ViewerGamepad::ConnectedDevices()) {
        passed &= Check(!device.id.empty() && !device.name.empty(), "An enumerated controller has no identity.");
        passed &= Check(deviceIds.insert(device.id).second, "A controller was enumerated more than once.");
        (void)screenshare::ViewerGamepad::ReadState(device.id);
    }
    return passed ? 0 : 1;
}
