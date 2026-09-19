#pragma once
#include <windows.h>
#include <future>
#include <thread>
#include <stdexcept>
#include <functional>
namespace proof {
class TestWindow {
public:
    explicit TestWindow(bool animate = true) {
        std::promise<HWND> ready; auto result = ready.get_future();
        thread_ = std::thread([animate, ready = std::move(ready)]() mutable {
            WNDCLASSW type{}; type.lpfnWndProc = Procedure;
            type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"OwnedCaptureProof";
            RegisterClassW(&type);
            HWND window = CreateWindowW(type.lpszClassName, L"ScreenShare capture proof", WS_OVERLAPPEDWINDOW,
                60, 60, 640, 400, nullptr, nullptr, type.hInstance, nullptr);
            if (!window) { ready.set_value(nullptr); return; }
            ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window);
            if (animate) SetTimer(window, 1, 30, nullptr);
            ready.set_value(window);
            MSG message;
            while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        });
        window_ = result.get();
        if (!window_) { thread_.join(); throw std::runtime_error("Proof window creation failed"); }
    }
    ~TestWindow() { Close(); }
    HWND handle() const { return window_; }
    void Invoke(std::function<void()> work) {
        std::exception_ptr error;
        std::function<void()> guarded = [&] { try { work(); } catch (...) { error = std::current_exception(); } };
        SendMessageW(window_, WM_APP + 1, 0, reinterpret_cast<LPARAM>(&guarded));
        if (error) std::rethrow_exception(error);
    }
    void Resize() { PostMessageW(window_, WM_APP, 0, 0); }
    void Close() { if (thread_.joinable()) { PostMessageW(window_, WM_CLOSE, 0, 0); thread_.join(); } }
private:
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        if (message == WM_APP + 1) { (*reinterpret_cast<std::function<void()>*>(l))(); return 0; }
        if (message == WM_TIMER) { SetWindowLongPtrW(window, GWLP_USERDATA, GetWindowLongPtrW(window, GWLP_USERDATA) + 1); InvalidateRect(window, nullptr, FALSE); return 0; }
        if (message == WM_APP) { SetWindowPos(window, nullptr, 0, 0, 800, 480, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER); return 0; }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint; HDC dc = BeginPaint(window, &paint);
            const int shade = 70 + int(GetWindowLongPtrW(window, GWLP_USERDATA) % 120);
            HBRUSH brush = CreateSolidBrush(RGB(shade, shade, shade));
            RECT bounds; GetClientRect(window, &bounds); FillRect(dc, &bounds, brush);
            DeleteObject(brush); EndPaint(window, &paint); return 0;
        }
        if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(window, message, w, l);
    }
    HWND window_ = nullptr;
    std::thread thread_;
};
}
