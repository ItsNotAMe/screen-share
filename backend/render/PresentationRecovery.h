#pragma once
#include "render/Nv12D3D11Presenter.h"
#include <dxgi.h>
#include <chrono>
#include <cstdint>

namespace screenshare::media {
// Shared UI/CLI/proof policy. Owner-thread only. Never retain/retry a failed
// frame. Thirty successfully presented fresh frames renew the three-rebuild
// budget. Redraws and skipped presents cannot renew it; unstable failures still
// exhaust recovery. The diagnostic recovery count remains cumulative.
class PresentationRecovery {
public:
    using Clock = std::chrono::steady_clock;
    template<class Render, class Reset>
    bool Present(Render&& render, Reset&& reset, Clock::time_point now = Clock::now(), bool freshFrame = true) {
        if (terminal_) throw std::runtime_error("Presentation recovery exhausted; restart the viewer");
        if (now < retryAfter_) return false;
        try {
            const bool presented = render();
            if (presented && freshFrame && healthyFrames_ < 30) {
                if (++healthyFrames_ == 30) attempts_ = 0;
            }
            return presented;
        } catch (const PresentationError& error) {
            const auto result = error.result();
            if (result != DXGI_ERROR_DEVICE_REMOVED && result != DXGI_ERROR_DEVICE_RESET &&
                result != DXGI_ERROR_DEVICE_HUNG && result != DXGI_ERROR_DRIVER_INTERNAL_ERROR) throw;
            lastError_ = result;
            healthyFrames_ = 0;
            reset();
            if (attempts_ == 3) { terminal_ = true; throw; }
            ++attempts_; ++recoveries_;
            retryAfter_ = now + std::chrono::milliseconds(250);
            return false;
        }
    }
    uint64_t recoveries() const noexcept { return recoveries_; }
    HRESULT lastError() const noexcept { return lastError_; }
private:
    Clock::time_point retryAfter_{};
    uint64_t recoveries_ = 0;
    unsigned attempts_ = 0, healthyFrames_ = 0;
    HRESULT lastError_ = S_OK;
    bool terminal_ = false;
};
}
