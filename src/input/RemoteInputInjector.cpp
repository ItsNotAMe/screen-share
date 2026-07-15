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
#include <cstddef>
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

bool MouseButtonInput(
    RemoteInputInjector::MouseButton button,
    bool down,
    DWORD& flags,
    DWORD& mouseData)
{
    flags = 0;
    mouseData = 0;
    switch (button) {
    case RemoteInputInjector::MouseButton::Left:
        flags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        return true;
    case RemoteInputInjector::MouseButton::Right:
        flags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        return true;
    case RemoteInputInjector::MouseButton::Middle:
        flags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        return true;
    case RemoteInputInjector::MouseButton::X1:
        flags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        mouseData = XBUTTON1;
        return true;
    case RemoteInputInjector::MouseButton::X2:
        flags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        mouseData = XBUTTON2;
        return true;
    }
    return false;
}

bool SendMouseButtonOnly(DWORD flags, DWORD mouseData)
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    input.mi.mouseData = mouseData;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

} // namespace

RemoteInputInjector::RemoteInputInjector()
{
    StartHostMouseMonitor();
}

RemoteInputInjector::~RemoteInputInjector()
{
    ReleasePressedMouseButtons();
    StopHostMouseMonitor();
}

void RemoteInputInjector::SetTargetBounds(int left, int top, int width, int height)
{
    ReleasePressedMouseButtons();
    left_ = left;
    top_ = top;
    width_ = std::max(0, width);
    height_ = std::max(0, height);
    windowHandle_ = 0;
    hasLastMousePosition_ = false;
}

void RemoteInputInjector::SetTargetWindow(uint64_t windowHandle)
{
    ReleasePressedMouseButtons();
    left_ = 0;
    top_ = 0;
    width_ = 0;
    height_ = 0;
    windowHandle_ = windowHandle;
    hasLastMousePosition_ = false;
}

bool RemoteInputInjector::IsTargetWindowForeground() const
{
    if (!HasTargetWindow()) {
        return false;
    }
    const HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(windowHandle_));
    if (!IsWindow(window) || !IsWindowVisible(window) || IsIconic(window)) {
        return false;
    }
    HWND targetRoot = GetAncestor(window, GA_ROOT);
    if (targetRoot == nullptr) {
        targetRoot = window;
    }
    return GetForegroundWindow() == targetRoot;
}

bool RemoteInputInjector::ResolveMousePoint(float normX, float normY, int& absX, int& absY) const
{
    int left = left_;
    int top = top_;
    int width = width_;
    int height = height_;
    HWND targetRoot = nullptr;

    if (HasTargetWindow()) {
        if (!IsTargetWindowForeground()) {
            return false;
        }
        const HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(windowHandle_));
        RECT client{};
        if (!GetClientRect(window, &client) || client.right <= client.left || client.bottom <= client.top) {
            return false;
        }
        POINT topLeft{client.left, client.top};
        POINT bottomRight{client.right, client.bottom};
        if (!ClientToScreen(window, &topLeft) || !ClientToScreen(window, &bottomRight)) {
            return false;
        }
        left = topLeft.x;
        top = topLeft.y;
        width = bottomRight.x - topLeft.x;
        height = bottomRight.y - topLeft.y;
        targetRoot = GetAncestor(window, GA_ROOT);
        if (targetRoot == nullptr) {
            targetRoot = window;
        }
    }

    if (width <= 0 || height <= 0) {
        return false;
    }
    const float x = std::clamp(normX, 0.0f, 1.0f);
    const float y = std::clamp(normY, 0.0f, 1.0f);
    absX = left + static_cast<int>(x * static_cast<float>((std::max)(0, width - 1)) + 0.5f);
    absY = top + static_cast<int>(y * static_cast<float>((std::max)(0, height - 1)) + 0.5f);

    if (targetRoot != nullptr) {
        const POINT point{absX, absY};
        HWND pointWindow = WindowFromPoint(point);
        if (pointWindow == nullptr) {
            return false;
        }
        HWND pointRoot = GetAncestor(pointWindow, GA_ROOT);
        if (pointRoot == nullptr) {
            pointRoot = pointWindow;
        }
        if (pointRoot != targetRoot) {
            return false;
        }
    }
    return true;
}

void RemoteInputInjector::ReleasePressedMouseButtons()
{
    for (std::size_t index = 0; index < pressedMouseButtons_.size(); ++index) {
        if (!pressedMouseButtons_[index]) {
            continue;
        }
        DWORD flags = 0;
        DWORD mouseData = 0;
        const auto button = static_cast<MouseButton>(index);
        if (MouseButtonInput(button, false, flags, mouseData)) {
            static_cast<void>(SendMouseButtonOnly(flags, mouseData));
        }
        pressedMouseButtons_[index] = false;
    }
}

void RemoteInputInjector::RefreshTargetState()
{
    if (HasTargetWindow() && !IsTargetWindowForeground()) {
        ReleasePressedMouseButtons();
    }
}

void RemoteInputInjector::InjectMouseMove(float normX, float normY)
{
    lastNormX_ = std::clamp(normX, 0.0f, 1.0f);
    lastNormY_ = std::clamp(normY, 0.0f, 1.0f);
    hasLastMousePosition_ = true;
    int absX = 0;
    int absY = 0;
    if (HostUsingMouseNow() || !ResolveMousePoint(lastNormX_, lastNormY_, absX, absY)) {
        RefreshTargetState();
        return;
    }
    static_cast<void>(SendAbsoluteMouse(MOUSEEVENTF_MOVE, absX, absY, 0));
}

void RemoteInputInjector::InjectMouseButton(MouseButton button, bool down, float normX, float normY)
{
    const std::size_t buttonIndex = static_cast<std::size_t>(button);
    if (buttonIndex >= pressedMouseButtons_.size()) {
        return;
    }
    lastNormX_ = std::clamp(normX, 0.0f, 1.0f);
    lastNormY_ = std::clamp(normY, 0.0f, 1.0f);
    hasLastMousePosition_ = true;

    DWORD flags = 0;
    DWORD mouseData = 0;
    if (!MouseButtonInput(button, down, flags, mouseData)) {
        return;
    }

    int absX = 0;
    int absY = 0;
    const bool canTarget = ResolveMousePoint(lastNormX_, lastNormY_, absX, absY);
    if (down) {
        if (HostUsingMouseNow() || !canTarget) {
            RefreshTargetState();
            return;
        }
        if (SendAbsoluteMouse(MOUSEEVENTF_MOVE | flags, absX, absY, mouseData)) {
            pressedMouseButtons_[buttonIndex] = true;
        }
        return;
    }

    // A release must never be suppressed after its matching press; otherwise a
    // focus change or host mouse movement could leave the OS button held down.
    if (!pressedMouseButtons_[buttonIndex]) {
        return;
    }
    if (!HostUsingMouseNow() && canTarget) {
        static_cast<void>(SendAbsoluteMouse(MOUSEEVENTF_MOVE | flags, absX, absY, mouseData));
    } else {
        static_cast<void>(SendMouseButtonOnly(flags, mouseData));
    }
    pressedMouseButtons_[buttonIndex] = false;
}

void RemoteInputInjector::InjectMouseScroll(int wheelDeltaX, int wheelDeltaY)
{
    int absX = 0;
    int absY = 0;
    if (HostUsingMouseNow() || !hasLastMousePosition_ ||
        !ResolveMousePoint(lastNormX_, lastNormY_, absX, absY)) {
        RefreshTargetState();
        return;
    }
    // Wheel messages are delivered to the window under the cursor. Reposition
    // first so scrolling follows the viewer's cursor instead of the host's.
    if (!SendAbsoluteMouse(MOUSEEVENTF_MOVE, absX, absY, 0)) {
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
    // Keyboard injection remains confined to a full-display share. Window
    // mouse targeting is coordinate-scoped, but SendInput keyboard events go
    // only to the foreground thread and cannot be constrained to a client rect.
    if (!HasTargetBounds()) {
        return;
    }
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
