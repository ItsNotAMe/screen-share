#pragma once

#include <array>
#include <cstdint>

namespace screenshare {

// Injects synthetic mouse/keyboard input on the host machine via the Win32
// SendInput API. Coordinates arrive normalized [0..1] relative to the captured
// surface; the injector maps them to absolute screen pixels using either a
// display rectangle or the current client rectangle of a shared window.
//
// This is the v1 (mouse + keyboard) injector. Gamepad injection (ViGEm) is a
// separate v2 component that will sit behind the same runtime gate.
//
// Self-contained and transport-agnostic: the runtime translates decoded control
// packets into these calls.
class RemoteInputInjector {
public:
    enum class MouseButton {
        Left = 0,
        Right = 1,
        Middle = 2,
        X1 = 3,
        X2 = 4,
    };

    RemoteInputInjector();
    ~RemoteInputInjector();

    RemoteInputInjector(const RemoteInputInjector&) = delete;
    RemoteInputInjector& operator=(const RemoteInputInjector&) = delete;

    // Absolute Windows screen rectangle of the surface being shared (a monitor's
    // bounds, or a captured window's client rect in screen space). Required
    // before mouse coordinates can be mapped.
    void SetTargetBounds(int left, int top, int width, int height);
    // Select a captured window as the dynamic mouse target. Input is accepted
    // only while this window is visible, not minimized, foreground, and owns
    // the mapped screen point. Keyboard injection remains display-only.
    void SetTargetWindow(uint64_t windowHandle);
    // Release any injected mouse buttons as soon as a window target loses
    // focus. Call regularly while a remote-control grant is active.
    void RefreshTargetState();
    [[nodiscard]] bool HasTargetBounds() const { return width_ > 0 && height_ > 0; }
    [[nodiscard]] bool HasTargetWindow() const { return windowHandle_ != 0; }

    // normX/normY are [0..1] across the captured surface.
    void InjectMouseMove(float normX, float normY);
    void InjectMouseButton(MouseButton button, bool down, float normX, float normY);
    void InjectMouseScroll(int wheelDeltaX, int wheelDeltaY);

    // Virtual-key code (Windows VK_*) and hardware scancode; either may drive the
    // injection (scancode preferred when non-zero).
    void InjectKey(uint16_t virtualKey, uint16_t scancode, bool down);

private:
    [[nodiscard]] bool IsTargetWindowForeground() const;
    [[nodiscard]] bool ResolveMousePoint(float normX, float normY, int& absX, int& absY) const;
    void ReleasePressedMouseButtons();

    int left_ = 0;
    int top_ = 0;
    int width_ = 0;
    int height_ = 0;
    uint64_t windowHandle_ = 0;
    float lastNormX_ = 0.5f;
    float lastNormY_ = 0.5f;
    bool hasLastMousePosition_ = false;
    std::array<bool, 5> pressedMouseButtons_{};
};

} // namespace screenshare
