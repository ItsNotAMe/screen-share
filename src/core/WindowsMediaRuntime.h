#pragma once

#include <Windows.h>
#include <combaseapi.h>

namespace screenshare {

// Own at application scope, across capture sessions and worker-thread restarts.
// Initialize the UI's STA first (e.g. QApplication); this lease preserves an
// existing apartment but otherwise makes the calling thread implicitly MTA.
// Join all media workers before destruction. Never use as a DLL/process-exit
// static: COM must be released before process shutdown begins.
class WindowsMediaRuntime final {
public:
    WindowsMediaRuntime() noexcept : result_(CoIncrementMTAUsage(&cookie_)) {}
    ~WindowsMediaRuntime() noexcept {
        if (SUCCEEDED(result_)) {
            CoDecrementMTAUsage(cookie_);
        }
    }

    WindowsMediaRuntime(const WindowsMediaRuntime&) = delete;
    WindowsMediaRuntime& operator=(const WindowsMediaRuntime&) = delete;
    HRESULT result() const noexcept { return result_; }

private:
    CO_MTA_USAGE_COOKIE cookie_{};
    HRESULT result_;
};

} // namespace screenshare
