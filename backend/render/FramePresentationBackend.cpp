#include "render/FramePresentationBackend.h"
#include "render/PresentationTarget.h"
#include <algorithm>

namespace {
class NativeFramePresentation final : public FramePresentationBackend {
    screenshare::Nv12D3D11Presenter presenter_;
    bool lowLatency_ = false;
    screenshare::PresentationOutcome outcome_ = screenshare::PresentationOutcome::Unknown;
public:
    bool Present(HWND window, uint32_t width, uint32_t height, bool smooth,
        bool lowLatency, const screenshare::Nv12D3D11Presenter::FrameView& frame,
        screenshare::Nv12D3D11Presenter::ScaleMode scale) override {
        if (!window || !IsWindow(window)) { Reset(); outcome_ = screenshare::PresentationOutcome::Unavailable; return false; }
        outcome_ = screenshare::PresentationTargetBlockReason(window);
        if (outcome_ != screenshare::PresentationOutcome::Unknown) return false;
        Prepare(window, width, height, smooth, lowLatency, scale);
        const bool result = presenter_.TryPresent(frame);
        outcome_ = presenter_.lastOutcome(); return result;
    }
    void Update(HWND window, uint32_t width, uint32_t height, bool smooth, bool lowLatency,
        screenshare::Nv12D3D11Presenter::ScaleMode scale) override {
        if (!window || !IsWindow(window)) { Reset(); return; }
        outcome_ = screenshare::PresentationTargetBlockReason(window);
        if (outcome_ != screenshare::PresentationOutcome::Unknown) return;
        Prepare(window, width, height, smooth, lowLatency, scale);
        presenter_.Redraw();
    }
    void Prepare(HWND window, uint32_t width, uint32_t height, bool smooth, bool lowLatency,
        screenshare::Nv12D3D11Presenter::ScaleMode scale) {
        if (lowLatency_ != lowLatency) {
            presenter_.Reset(); presenter_.SetLowLatency(lowLatency); lowLatency_ = lowLatency;
        }
        presenter_.Attach(window);
        // Qt supplies logical dimensions; DXGI uses actual HWND client pixels.
        // Feeding a different size on every frame triggers Resize's redraw of
        // the old image, consuming the one-frame queue before TryPresent runs.
        RECT client{};
        if (GetClientRect(window, &client)) {
            width = static_cast<uint32_t>(std::max<LONG>(1, client.right - client.left));
            height = static_cast<uint32_t>(std::max<LONG>(1, client.bottom - client.top));
        }
        presenter_.Resize(width, height);
        presenter_.SetLinearSampling(smooth);
        presenter_.SetScaleMode(scale);
    }
    void Reset() noexcept override { presenter_.Reset(); }
    uint32_t MaximumFrameLatency() const noexcept override { return presenter_.maximumFrameLatency(); }
    screenshare::PresentationOutcome LastOutcome() const noexcept override { return outcome_; }
};
}

std::unique_ptr<FramePresentationBackend> CreateNativeFramePresentation() {
    return std::make_unique<NativeFramePresentation>();
}

bool FramePresentationSession::Present(HWND window, uint32_t width, uint32_t height, bool smooth,
    bool lowLatency, const screenshare::Nv12D3D11Presenter::FrameView& frame,
    screenshare::Nv12D3D11Presenter::ScaleMode scale) {
    return Run([&](auto& backend) { return backend.Present(window, width, height, smooth, lowLatency, frame, scale); }, true);
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
    statistics_.lastError = S_OK;
    statistics_.outcome = screenshare::PresentationOutcome::Unknown;
}
