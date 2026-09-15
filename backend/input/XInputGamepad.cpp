#include "input/XInputGamepad.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <Xinput.h>

#include <array>
#include <mutex>

namespace screenshare {
namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetStateFn ResolveXInputGetState()
{
    static std::once_flag once;
    static XInputGetStateFn function = nullptr;
    static HMODULE module = nullptr;
    std::call_once(once, [] {
        constexpr std::array<const wchar_t*, 3> Libraries{
            L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
        for (const wchar_t* library : Libraries) {
            module = LoadLibraryW(library);
            if (module == nullptr) {
                continue;
            }
            function = reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"));
            if (function != nullptr) {
                break;
            }
            FreeLibrary(module);
            module = nullptr;
        }
    });
    return function;
}

} // namespace

std::vector<int> XInputGamepad::ConnectedSlots()
{
    std::vector<int> slots;
    for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
        if (ReadState(slot).has_value()) {
            slots.push_back(slot);
        }
    }
    return slots;
}

std::optional<RemoteGamepadState> XInputGamepad::ReadState(int slot)
{
    const auto getState = ResolveXInputGetState();
    if (getState == nullptr || slot < 0 || slot >= XUSER_MAX_COUNT) {
        return std::nullopt;
    }

    XINPUT_STATE state{};
    if (getState(static_cast<DWORD>(slot), &state) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    RemoteGamepadState result;
    result.controllerSlot = static_cast<uint8_t>(slot);
    result.buttons = state.Gamepad.wButtons & 0xF3FFU;
    result.leftTrigger = state.Gamepad.bLeftTrigger;
    result.rightTrigger = state.Gamepad.bRightTrigger;
    result.thumbLX = state.Gamepad.sThumbLX;
    result.thumbLY = state.Gamepad.sThumbLY;
    result.thumbRX = state.Gamepad.sThumbRX;
    result.thumbRY = state.Gamepad.sThumbRY;
    return result;
}

} // namespace screenshare
