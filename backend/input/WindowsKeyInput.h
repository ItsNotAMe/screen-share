#pragma once
#include <Windows.h>
#include <cstdint>

namespace screenshare {
// The input protocol and Win32 preview encode the E0 prefix in scan-code bit 8.
// The Qt viewer converts its native E0xx representation at the capture boundary.
// SendInput requires the low scan byte and a separate extended-key flag.
inline INPUT WindowsKeyInput(uint16_t virtualKey, uint16_t scancode, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    // Pause uses an E1 sequence, which cannot be represented by the E0 flag.
    if (scancode && virtualKey != VK_PAUSE) {
        input.ki.wScan = scancode & 0xff;
        input.ki.dwFlags |= KEYEVENTF_SCANCODE;
        if (scancode & 0x100) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    } else {
        input.ki.wVk = virtualKey;
        const auto mapped = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC_EX);
        input.ki.wScan = mapped & 0xff;
        // VK navigation keys can map to the keypad's unprefixed scan byte.
        // Preserve their dedicated-key identity when no native scan is supplied.
        switch (virtualKey) {
        case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
        case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_INSERT: case VK_DELETE: case VK_RCONTROL: case VK_RMENU:
        case VK_DIVIDE: case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_SNAPSHOT:
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY; break;
        default:
            if ((mapped & 0xff00) == 0xe000) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
    }
    return input;
}
}
