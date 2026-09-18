#pragma once
#include <windows.h>
#include <timeapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace proof1080 {
constexpr int Width = 1920, Height = 1080;
inline double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
// Only this owned window is captured. Marker generation never injects input.
// Source time and received marker use the same process clock; no remote clock
// subtraction or physical display latency is claimed.
class Scene {
public:
    explicit Scene(std::string mode) : mode_(std::move(mode)) {
        timeBeginPeriod(1);
        std::promise<HWND> ready; auto future = ready.get_future();
        thread_ = std::thread([this, ready = std::move(ready)]() mutable {
            WNDCLASSW type{}; type.lpfnWndProc = Procedure; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"BackendComparisonScene";
            RegisterClassW(&type);
            HWND window = CreateWindowW(type.lpszClassName, L"ScreenShare comparison — generated content only", WS_POPUP,
                40, 40, Width, Height, nullptr, nullptr, type.hInstance, this);
            if (window) { ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window); SetTimer(window, 1, 1, nullptr); }
            ready.set_value(window);
            if (!window) return;
            MSG message; while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        });
        window_ = future.get();
        if (!window_) { thread_.join(); timeEndPeriod(1); throw std::runtime_error("Scene creation failed"); }
    }
    ~Scene() { PostMessageW(window_, WM_CLOSE, 0, 0); thread_.join(); timeEndPeriod(1); }
    HWND window() const { return window_; }
    double Time(unsigned id) { std::lock_guard lock(mutex_); return times_[id]; }
    unsigned count() const { return count_.load(); }
    double CpuSeconds() {
        FILETIME created{}, ended{}, kernel{}, user{};
        Require(GetThreadTimes(thread_.native_handle(), &created, &ended, &kernel, &user), "Scene CPU timing failed");
        auto value = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        return double(value(kernel) + value(user)) / 1e7;
    }
    int Shade(unsigned id, int x, int y) const {
        if (mode_ == "static") return ((x / 20 + y / 20) % 2) ? 180 : 60;
        if (mode_ == "scroll") return (((x + int(id) * 4) / 12 + y / 20) % 2) ? 200 : 40;
        const unsigned hash = (unsigned(x / 8) * 73856093u) ^ (unsigned(y / 8) * 19349663u) ^ (id * 83492791u);
        return 30 + int((hash ^ (hash >> 13)) % 196);
    }
private:
    void Paint(HWND window) {
        const auto time = NowMs(); const unsigned id = ++count_;
        Require(id < times_.size(), "Scene marker exhausted");
        std::vector<uint32_t> pixels(Width * Height);
        for (int y = 0; y < Height; ++y) for (int x = 0; x < Width; ++x) {
            int shade = Shade(id, x, y);
            const int markerX = x * 640 / Width, markerY = y * 360 / Height;
            if (markerY >= 160 && markerY < 208 && markerX >= 32 && markerX < 608) {
                bool bit = (id >> ((markerX - 32) / 36)) & 1;
                if (markerY >= 184) bit = !bit;
                shade = bit ? 230 : 25;
            }
            pixels[y * Width + x] = uint32_t(shade) * 0x010101u;
        }
        { std::lock_guard lock(mutex_); times_[id] = time; }
        PAINTSTRUCT paint; const HDC dc = BeginPaint(window, &paint);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = Width;
        info.bmiHeader.biHeight = -Height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        SetDIBitsToDevice(dc, 0, 0, Width, Height, 0, 0, 0, Height, pixels.data(), &info, DIB_RGB_COLORS);
        EndPaint(window, &paint); GdiFlush();
    }
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        if (message == WM_NCCREATE) SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        auto* self = reinterpret_cast<Scene*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_TIMER && self) {
            const double now = NowMs();
            if (now >= self->nextPaint_) { self->nextPaint_ = std::max(self->nextPaint_ + 1000.0 / 60, now); InvalidateRect(window, nullptr, FALSE); }
            return 0;
        }
        if (message == WM_PAINT && self) { self->Paint(window); return 0; }
        if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(window, message, w, l);
    }
    std::string mode_; HWND window_{}; std::thread thread_; std::mutex mutex_;
    double nextPaint_ = 0;
    std::vector<double> times_ = std::vector<double>(65536); std::atomic<unsigned> count_{0};
};


}
