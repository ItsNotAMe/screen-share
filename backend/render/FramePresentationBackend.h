#pragma once

#include "render/Nv12D3D11Presenter.h"
#include "render/PresentationRecovery.h"
#include <Windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

// Owner-thread only. UI uses its presentation worker; CLI uses its window thread.
// No frame is retained here. Injection exercises both clients without a GPU.
class FramePresentationBackend {
public:
    virtual ~FramePresentationBackend() = default;
    virtual bool Present(HWND window, uint32_t width, uint32_t height,
        bool smooth, bool lowLatency, const screenshare::Nv12D3D11Presenter::FrameView& frame,
        screenshare::Nv12D3D11Presenter::ScaleMode scale = screenshare::Nv12D3D11Presenter::ScaleMode::Fit) = 0;
    virtual void Reset() noexcept = 0;
    virtual void Update(HWND window, uint32_t width, uint32_t height, bool smooth, bool lowLatency,
        screenshare::Nv12D3D11Presenter::ScaleMode scale = screenshare::Nv12D3D11Presenter::ScaleMode::Fit) = 0;
    virtual uint32_t MaximumFrameLatency() const noexcept = 0;
    virtual bool HardwareAccelerated() const noexcept { return false; }
    virtual screenshare::PresentationOutcome LastOutcome() const noexcept { return screenshare::PresentationOutcome::Unknown; }
};
using FramePresentationFactory = std::function<std::unique_ptr<FramePresentationBackend>()>;
std::unique_ptr<FramePresentationBackend> CreateNativeFramePresentation();

class FramePresentationSession {
public:
    struct Statistics {
        uint64_t errors = 0, recoveries = 0;
        uint32_t maximumFrameLatency = 0;
        bool hardwareAccelerated = false;
        bool terminal = false;
        HRESULT lastError = S_OK;
        screenshare::PresentationOutcome outcome = screenshare::PresentationOutcome::Unknown;
        uint64_t busyDrops = 0, occludedDrops = 0, minimizedDrops = 0, unavailableDrops = 0, backoffDrops = 0;
    };
    explicit FramePresentationSession(FramePresentationFactory factory = {}) : factory_(std::move(factory)) {}
    bool Present(HWND window, uint32_t width, uint32_t height, bool smooth, bool lowLatency,
        const screenshare::Nv12D3D11Presenter::FrameView& frame,
        screenshare::Nv12D3D11Presenter::ScaleMode scale = screenshare::Nv12D3D11Presenter::ScaleMode::Fit);
    bool Update(HWND window, uint32_t width, uint32_t height, bool smooth, bool lowLatency,
        screenshare::Nv12D3D11Presenter::ScaleMode scale = screenshare::Nv12D3D11Presenter::ScaleMode::Fit);
    void Clear() noexcept;
    void Release() noexcept { backend_.reset(); }
    Statistics statistics() const noexcept { return statistics_; }
private:
    template<class Operation> bool Run(Operation&& operation, bool frame = false) {
        if (statistics_.terminal) return false;
        bool invoked = false;
        bool failed = false;
        try {
            const bool result = recovery_.Present([&] {
                invoked = true;
                try {
                    if (!backend_) backend_ = factory_ ? factory_() : CreateNativeFramePresentation();
                    if (!backend_) throw std::runtime_error("Missing presentation backend");
                    return operation(*backend_);
                } catch (const screenshare::PresentationError& error) {
                    failed = true;
                    ++statistics_.errors; statistics_.lastError = error.result();
                    statistics_.outcome = screenshare::PresentationOutcome::Failed; throw;
                } catch (...) {
                    failed = true;
                    ++statistics_.errors; statistics_.lastError = E_FAIL;
                    statistics_.outcome = screenshare::PresentationOutcome::Failed; throw;
                }
            }, [&] { if (backend_) backend_->Reset(); });
            if (frame) {
                using enum screenshare::PresentationOutcome;
                if (!invoked) { statistics_.outcome = Backoff; ++statistics_.backoffDrops; }
                else if (result) statistics_.outcome = Presented;
                else if (!failed) statistics_.outcome = backend_->LastOutcome();
                switch (statistics_.outcome) {
                case Busy: ++statistics_.busyDrops; break;
                case Occluded: ++statistics_.occludedDrops; break;
                case Minimized: ++statistics_.minimizedDrops; break;
                case Unavailable: ++statistics_.unavailableDrops; break;
                default: break;
                }
            }
            statistics_.recoveries = recovery_.recoveries();
            statistics_.maximumFrameLatency = backend_ ? backend_->MaximumFrameLatency() : 0;
            statistics_.hardwareAccelerated = backend_ && backend_->HardwareAccelerated();
            return result;
        } catch (...) {
            if (backend_) backend_->Reset();
            statistics_.recoveries = recovery_.recoveries();
            statistics_.maximumFrameLatency = 0;
            statistics_.hardwareAccelerated = false;
            statistics_.terminal = true;
            return false;
        }
    }
    FramePresentationFactory factory_;
    std::unique_ptr<FramePresentationBackend> backend_;
    screenshare::media::PresentationRecovery recovery_;
    Statistics statistics_;
};
