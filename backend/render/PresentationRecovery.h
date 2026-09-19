#pragma once
#include "render/Nv12D3D11Presenter.h"
#include <dxgi.h>
#include <chrono>
#include <cstdint>

namespace screenshare::media {
// Shared UI/CLI/proof policy. Owner-thread only. Never retain/retry a failed frame. A session gets at most
// three device rebuilds; a fourth failure is terminal even after a good frame.
// This prevents intermittent driver failures from causing endless rebuilds.
class PresentationRecovery {
public:
    using Clock = std::chrono::steady_clock;
    template<class Render, class Reset>
    bool Present(Render&& render, Reset&& reset, Clock::time_point now = Clock::now()) {
        if (terminal_) throw std::runtime_error("Presentation recovery exhausted; restart the viewer");
        if (now < retryAfter_) return false;
        try {
            return render();
        } catch (const PresentationError& error) {
            const auto result = error.result();
            if (result != DXGI_ERROR_DEVICE_REMOVED && result != DXGI_ERROR_DEVICE_RESET &&
                result != DXGI_ERROR_DEVICE_HUNG && result != DXGI_ERROR_DRIVER_INTERNAL_ERROR) throw;
            lastError_ = result;
            reset();
            if (recoveries_ == 3) { terminal_ = true; throw; }
            ++recoveries_;
            retryAfter_ = now + std::chrono::milliseconds(250);
            return false;
        }
    }
    uint64_t recoveries() const noexcept { return recoveries_; }
    HRESULT lastError() const noexcept { return lastError_; }
private:
    Clock::time_point retryAfter_{};
    uint64_t recoveries_ = 0;
    HRESULT lastError_ = S_OK;
    bool terminal_ = false;
};
}
