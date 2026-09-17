#pragma once
#include "CaptureSession.h"
#include "media/webrtc/D3dVideoFrameBuffer.h"

namespace screenshare::media {
struct WindowsCaptureResource final : CaptureResource {
    webrtc::scoped_refptr<D3dVideoFrameBuffer> buffer;
    std::shared_ptr<D3dVideoDevice> device;
};
// Windows/WebRTC implementation boundary. Create only through the session's
// factory. WindowsMediaRuntime must outlive the joined session.
class WindowsCaptureSource final : public ICaptureSource {
public:
    explicit WindowsCaptureSource(CaptureConfig config) : config_(config) {
        config_.allowDisplayFallback = true;
        config_.includeNv12 = config_.ownedNv12 = true;
        config_.includeNv12Readback = config_.includeBgraReadback = false;
    }
    void Start() override { capture_.Start(config_); }
    std::optional<CaptureSample> Poll() override {
        try {
            auto frame = capture_.TryCaptureFrame(std::chrono::milliseconds(10));
            if (!frame) return std::nullopt;
            const auto captured = std::chrono::steady_clock::now();
            if (!device_) device_ = std::make_shared<D3dVideoDevice>(frame->d3dDevice);
            auto resource = std::make_shared<WindowsCaptureResource>();
            resource->buffer = device_->RetainCapture(*frame);
            resource->device = device_;
            return CaptureSample{std::move(resource), captured};
        } catch (const CaptureDeviceLostError&) { throw CaptureLost(); }
        catch (...) { if (Closed()) return std::nullopt; throw; }
    }
    bool Closed() const override { return capture_.sourceState() == CaptureSourceState::Closed; }
    bool Minimized() const override { return capture_.sourceState() == CaptureSourceState::Minimized; }
    CaptureSourceInfo Info() const override {
        return {capture_.config().backend == CaptureBackend::WindowsGraphicsCapture ?
            CaptureImplementation::WindowsGraphicsCapture : CaptureImplementation::DesktopDuplication, capture_.displayFallback()};
    }
    void Retire() noexcept override { if (device_) device_->Retire(); }
    void Rebuild() override { capture_.RebuildDevice(); device_.reset(); }
private:
    CaptureConfig config_;
    DesktopCapturer capture_;
    std::shared_ptr<D3dVideoDevice> device_;
};
}
