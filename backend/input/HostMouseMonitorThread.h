#pragma once
#include <Windows.h>
#include <functional>
#include <system_error>
#include <thread>
#include <utility>

namespace screenshare {
// Dedicated hook owner. Callbacks run on this thread, must not throw, and allow
// lifecycle checks without installing a global input hook.
class HostMouseMonitorThread {
public:
    HostMouseMonitorThread(std::function<HHOOK()> install, std::function<void(HHOOK)> uninstall)
        : stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (!stop_) throw std::system_error(GetLastError(), std::system_category(), "Create mouse monitor stop event");
        try {
            thread_ = std::thread([this, install = std::move(install), uninstall = std::move(uninstall)] {
                const auto hook = install();
                Pump();
                if (hook) uninstall(hook);
            });
        } catch (...) { CloseHandle(stop_); throw; }
    }
    ~HostMouseMonitorThread() {
        RequestStop();
        thread_.join();
        CloseHandle(stop_);
    }
    HostMouseMonitorThread(const HostMouseMonitorThread&) = delete;
    HostMouseMonitorThread& operator=(const HostMouseMonitorThread&) = delete;
    void RequestStop() noexcept {
        SetEvent(stop_);
    }
private:
    void Pump() noexcept {
        // A signaled event persists through delayed hook/queue startup. It also
        // avoids quit-post failure when the message queue is full. Never poll.
        while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT &&
            MsgWaitForMultipleObjectsEx(1, &stop_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0 + 1) {
            MSG message{};
            // Check cancellation between messages so traffic cannot delay stop.
            while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) return;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
    HANDLE stop_;
    std::thread thread_;
};
}
