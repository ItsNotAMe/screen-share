#pragma once
#include "CaptureTestWindow.h"
#include "media/webrtc/Nv12VideoPresenter.h"
#include "media/webrtc/LatestVideoFrameSink.h"
#include <dwmapi.h>

namespace proof {
class PresentationTestWindow {
public:
    PresentationTestWindow() : window_(false) {
        window_.Invoke([&] {
            SetWindowTextW(window_.handle(), L"ScreenShare received video proof");
            SetWindowPos(window_.handle(), nullptr, 760, 60, 640, 480, SWP_NOZORDER | SWP_NOACTIVATE);
            presenter_ = std::make_unique<screenshare::media::Nv12VideoPresenter>(window_.handle());
            if (!presenter_->hardware() || presenter_->maximumFrameLatency() != 1)
                throw std::runtime_error("GPU presentation or one-frame DXGI limit unavailable");
        });
    }
    ~PresentationTestWindow() { window_.Invoke([&] { presenter_.reset(); }); }
    void Drain(screenshare::media::LatestVideoFrameSink& sink) {
        if (auto frame = sink.Take()) window_.Invoke([&] {
            presenter_->Present(*frame);
            presented = presenter_->presented(); conversions = presenter_->conversions(); repacks = presenter_->repacks();
        });
    }
    void Resize() { window_.Invoke([&] { SetWindowPos(window_.handle(), nullptr, 0, 0, 800, 600, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); }); }
    void ValidateVisiblePixels() {
        window_.Invoke([&] {
            if (FAILED(DwmFlush())) throw std::runtime_error("Compositor synchronization failed");
            RECT bounds; GetClientRect(window_.handle(), &bounds);
            // Flip-model pixels are composed by DWM; the window's GDI DC can
            // still expose its old WM_PAINT backing surface. Sample only this
            // generated window's client coordinates from the composed desktop.
            POINT centerPoint{bounds.right / 2, bounds.bottom / 2}, barPoint{8, 8};
            ClientToScreen(window_.handle(), &centerPoint); ClientToScreen(window_.handle(), &barPoint);
            HDC dc = GetDC(nullptr);
            if (!dc) throw std::runtime_error("Presentation validation DC unavailable");
            const COLORREF center = GetPixel(dc, centerPoint.x, centerPoint.y);
            const COLORREF bar = GetPixel(dc, barPoint.x, barPoint.y);
            ReleaseDC(nullptr, dc);
            const int r = GetRValue(center), g = GetGValue(center), b = GetBValue(center);
            // Desktop color transforms (e.g. night light) can change neutral RGB.
            // Chroma is checked before composition by the receive-side sink.
            if (center == CLR_INVALID || bar == CLR_INVALID || r < 40 || r > 240 ||
                r < GetRValue(bar) + 20 || g < GetGValue(bar) + 15 || b < GetBValue(bar) + 10 ||
                GetRValue(bar) > 40 || GetGValue(bar) > 40 || GetBValue(bar) > 40)
                throw std::runtime_error("Presented window pixels/letterboxing did not match generated video; center=" +
                    std::to_string(center) + " bar=" + std::to_string(bar) + "; check occlusion");
        });
    }
    uint64_t presented = 0, conversions = 0, repacks = 0;
private:
    TestWindow window_;
    std::unique_ptr<screenshare::media::Nv12VideoPresenter> presenter_;
};
}
