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
};
using FramePresentationFactory = std::function<std::unique_ptr<FramePresentationBackend>()>;
std::unique_ptr<FramePresentationBackend> CreateNativeFramePresentation();

class FramePresentationSession {
public:
    struct Statistics {
        uint64_t errors = 0, recoveries = 0;
        uint32_t maximumFrameLatency = 0;
        bool terminal = false;
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
    template<class Operation> bool Run(Operation&& operation) {
        if (statistics_.terminal) return false;
        try {
            const bool result = recovery_.Present([&] {
                try {
                    if (!backend_) backend_ = factory_ ? factory_() : CreateNativeFramePresentation();
                    if (!backend_) throw std::runtime_error("Missing presentation backend");
                    return operation(*backend_);
                } catch (...) { ++statistics_.errors; throw; }
            }, [&] { if (backend_) backend_->Reset(); });
            statistics_.recoveries = recovery_.recoveries();
            statistics_.maximumFrameLatency = backend_ ? backend_->MaximumFrameLatency() : 0;
            return result;
        } catch (...) {
            if (backend_) backend_->Reset();
            statistics_.recoveries = recovery_.recoveries();
            statistics_.maximumFrameLatency = 0;
            statistics_.terminal = true;
            return false;
        }
    }
    FramePresentationFactory factory_;
    std::unique_ptr<FramePresentationBackend> backend_;
    screenshare::media::PresentationRecovery recovery_;
    Statistics statistics_;
};
