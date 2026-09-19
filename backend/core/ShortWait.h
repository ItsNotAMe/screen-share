#pragma once
#include <chrono>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace screenshare {
// Owned by a media worker. Ordinary sleeps can expand to a ~15.6 ms quantum
// for silent/occluded Windows processes. This private timer does not alter
// process-wide power policy or system timer resolution and never busy-spins.
class ShortWait {
public:
#ifdef _WIN32
    ShortWait() : timer_(CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE)) {
        if (!timer_) throw std::runtime_error("Media poll timer creation failed");
    }
    ~ShortWait() { CloseHandle(timer_); }
#else
    ShortWait() = default;
#endif
    ShortWait(const ShortWait&) = delete;
    ShortWait& operator=(const ShortWait&) = delete;
    void Wait(std::chrono::nanoseconds delay = std::chrono::milliseconds(1)) {
        if (delay <= std::chrono::nanoseconds::zero() || delay > std::chrono::milliseconds(10))
            throw std::invalid_argument("Media poll wait must be within 10 ms");
#ifdef _WIN32
        LARGE_INTEGER due; due.QuadPart = -((delay.count() + 99) / 100);
        if (!SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0) ||
            WaitForSingleObject(timer_, 100) != WAIT_OBJECT_0)
            throw std::runtime_error("Media poll timer wait failed");
#else
        std::this_thread::sleep_for(delay);
#endif
    }
private:
#ifdef _WIN32
    HANDLE timer_;
#endif
};
}
