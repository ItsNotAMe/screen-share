#include "input/RemoteInputInjector.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace screenshare {
namespace {

// Host-priority mouse arbitration: a low-level mouse hook records when the host
// physically moves the mouse (events without the injected flag). While that is
// recent, viewer-injected mouse input is suppressed so the host is never fought
// for control of their own cursor.
constexpr DWORD kHostMouseCooldownMs = 300;
std::atomic<DWORD> g_lastPhysicalMouseTick{0};
std::mutex g_monitorMutex;
int g_monitorRefcount = 0;
std::thread g_monitorThread;
std::atomic<DWORD> g_monitorThreadId{0};

LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (info != nullptr && (info->flags & LLMHF_INJECTED) == 0) {
            g_lastPhysicalMouseTick.store(GetTickCount(), std::memory_order_relaxed);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void MonitorThreadMain()
{
    g_monitorThreadId.store(GetCurrentThreadId(), std::memory_order_release);
    const HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // Pump the message queue; WM_QUIT (posted on stop) breaks the loop.
    }
    if (hook != nullptr) {
        UnhookWindowsHookEx(hook);
    }
}

void StartHostMouseMonitor()
{
    std::lock_guard<std::mutex> lock(g_monitorMutex);
    if (g_monitorRefcount++ == 0) {
        g_monitorThreadId.store(0, std::memory_order_release);
        g_monitorThread = std::thread(MonitorThreadMain);
    }
}

void StopHostMouseMonitor()
{
    std::lock_guard<std::mutex> lock(g_monitorMutex);
    if (g_monitorRefcount > 0 && --g_monitorRefcount == 0) {
        for (int waited = 0; waited < 200 && g_monitorThreadId.load(std::memory_order_acquire) == 0; ++waited) {
            Sleep(1);
        }
        const DWORD threadId = g_monitorThreadId.load(std::memory_order_acquire);
        if (threadId != 0) {
            PostThreadMessageW(threadId, WM_QUIT, 0, 0);
        }
        if (g_monitorThread.joinable()) {
            g_monitorThread.join();
        }
    }
}

bool HostUsingMouseNow()
{
    const DWORD last = g_lastPhysicalMouseTick.load(std::memory_order_relaxed);
    if (last == 0) {
        return false;
    }
    return (GetTickCount() - last) < kHostMouseCooldownMs;
}

// Maps an absolute screen pixel to the 0..65535 normalized space SendInput uses
// for MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK.
LONG ToVirtualDesktopAbsolute(int pixel, int virtualOrigin, int virtualExtent)
{
    if (virtualExtent <= 1) {
        return 0;
    }
    const double normalized =
        static_cast<double>(pixel - virtualOrigin) / static_cast<double>(virtualExtent - 1);
    const double clamped = std::clamp(normalized, 0.0, 1.0);
    return static_cast<LONG>(clamped * 65535.0 + 0.5);
}

bool SendAbsoluteMouse(DWORD flags, int absX, int absY, DWORD mouseData)
{
    const int virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = ToVirtualDesktopAbsolute(absX, virtualLeft, virtualWidth);
    input.mi.dy = ToVirtualDesktopAbsolute(absY, virtualTop, virtualHeight);
    input.mi.mouseData = mouseData;
    input.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

} // namespace

RemoteInputInjector::RemoteInputInjector()
{
    StartHostMouseMonitor();
}

RemoteInputInjector::~RemoteInputInjector()
{
    StopHostMouseMonitor();
}

void RemoteInputInjector::SetTargetBounds(int left, int top, int width, int height)
{
    left_ = left;
    top_ = top;
    width_ = std::max(0, width);
    height_ = std::max(0, height);
}

void RemoteInputInjector::InjectMouseMove(float normX, float normY)
{
    if (!HasTargetBounds() || HostUsingMouseNow()) {
        return;
    }
    const int absX = left_ + static_cast<int>(std::clamp(normX, 0.0f, 1.0f) * static_cast<float>(width_));
    const int absY = top_ + static_cast<int>(std::clamp(normY, 0.0f, 1.0f) * static_cast<float>(height_));
    SendAbsoluteMouse(MOUSEEVENTF_MOVE, absX, absY, 0);
}

void RemoteInputInjector::InjectMouseButton(MouseButton button, bool down, float normX, float normY)
{
    if (!HasTargetBounds() || HostUsingMouseNow()) {
        return;
    }
    // Position the cursor at the click point first so the press lands correctly.
    const int absX = left_ + static_cast<int>(std::clamp(normX, 0.0f, 1.0f) * static_cast<float>(width_));
    const int absY = top_ + static_cast<int>(std::clamp(normY, 0.0f, 1.0f) * static_cast<float>(height_));

    DWORD flags = MOUSEEVENTF_MOVE;
    DWORD mouseData = 0;
    switch (button) {
    case MouseButton::Left:
        flags |= down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case MouseButton::Right:
        flags |= down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    case MouseButton::Middle:
        flags |= down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    case MouseButton::X1:
        flags |= down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        mouseData = XBUTTON1;
        break;
    case MouseButton::X2:
        flags |= down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        mouseData = XBUTTON2;
        break;
    }
    SendAbsoluteMouse(flags, absX, absY, mouseData);
}

void RemoteInputInjector::InjectMouseScroll(int wheelDeltaX, int wheelDeltaY)
{
    if (HostUsingMouseNow()) {
        return;
    }
    if (wheelDeltaY != 0) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = static_cast<DWORD>(wheelDeltaY);
        SendInput(1, &input, sizeof(INPUT));
    }
    if (wheelDeltaX != 0) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        input.mi.mouseData = static_cast<DWORD>(wheelDeltaX);
        SendInput(1, &input, sizeof(INPUT));
    }
}

void RemoteInputInjector::InjectKey(uint16_t virtualKey, uint16_t scancode, bool down)
{
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtualKey;
    input.ki.wScan = scancode;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    if (scancode != 0 && virtualKey == 0) {
        input.ki.dwFlags |= KEYEVENTF_SCANCODE;
    }
    SendInput(1, &input, sizeof(INPUT));
}

} // namespace screenshare
