#include "render/FramePresentationBackend.h"

namespace {
class NativeFramePresentation final : public FramePresentationBackend {
    screenshare::Nv12D3D11Presenter presenter_;
    bool lowLatency_ = false;
public:
    bool Present(HWND window, uint32_t width, uint32_t height, bool smooth,
        bool lowLatency, const screenshare::Nv12D3D11Presenter::FrameView& frame,
        screenshare::Nv12D3D11Presenter::ScaleMode scale) override {
        if (!window || !IsWindow(window)) { Reset(); return false; }
        if (IsIconic(window)) return false;
        Prepare(window, width, height, smooth, lowLatency, scale);
        return presenter_.TryPresent(frame);
    }
    void Update(HWND window, uint32_t width, uint32_t height, bool smooth, bool lowLatency,
        screenshare::Nv12D3D11Presenter::ScaleMode scale) override {
        if (!window || !IsWindow(window)) { Reset(); return; }
        if (IsIconic(window)) return;
        Prepare(window, width, height, smooth, lowLatency, scale);
        presenter_.Redraw();
    }
    void Prepare(HWND window, uint32_t width, uint32_t height, bool smooth, bool lowLatency,
        screenshare::Nv12D3D11Presenter::ScaleMode scale) {
        if (!window || !IsWindow(window)) { Reset(); return; }
        if (IsIconic(window)) return;
        if (lowLatency_ != lowLatency) {
            presenter_.Reset(); presenter_.SetLowLatency(lowLatency); lowLatency_ = lowLatency;
        }
        presenter_.Attach(window);
        presenter_.Resize(width, height);
        presenter_.SetLinearSampling(smooth);
        presenter_.SetScaleMode(scale);
    }
    void Reset() noexcept override { presenter_.Reset(); }
    uint32_t MaximumFrameLatency() const noexcept override { return presenter_.maximumFrameLatency(); }
};
}

std::unique_ptr<FramePresentationBackend> CreateNativeFramePresentation() {
    return std::make_unique<NativeFramePresentation>();
}

bool FramePresentationSession::Present(HWND window, uint32_t width, uint32_t height, bool smooth,
    bool lowLatency, const screenshare::Nv12D3D11Presenter::FrameView& frame,
    screenshare::Nv12D3D11Presenter::ScaleMode scale) {
    return Run([&](auto& backend) { return backend.Present(window, width, height, smooth, lowLatency, frame, scale); });
}

bool FramePresentationSession::Update(HWND window, uint32_t width, uint32_t height, bool smooth,
    bool lowLatency, screenshare::Nv12D3D11Presenter::ScaleMode scale) {
    return Run([&](auto& backend) { backend.Update(window, width, height, smooth, lowLatency, scale); return true; });
}

void FramePresentationSession::Clear() noexcept {
    if (backend_) backend_->Reset();
    recovery_ = {};
    statistics_.recoveries = 0;
    statistics_.maximumFrameLatency = 0;
    statistics_.terminal = false;
}
