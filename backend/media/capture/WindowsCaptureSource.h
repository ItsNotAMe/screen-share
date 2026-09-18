#pragma once
#include "CaptureSession.h"
#include "media/webrtc/D3dVideoFrameBuffer.h"
#include "input/v2/DesktopTarget.h"
#include <atomic>

namespace screenshare::media {
struct WindowsCaptureResource final : CaptureResource {
    webrtc::scoped_refptr<D3dVideoFrameBuffer> buffer;
    std::shared_ptr<D3dVideoDevice> device;
};
// Windows/WebRTC implementation boundary. Create only through the session's
// factory. WindowsMediaRuntime must outlive the joined session.
class WindowsCaptureSource final : public ICaptureSource {
public:
    explicit WindowsCaptureSource(CaptureConfig config, std::shared_ptr<input::DesktopTargetState> target = {}) : config_(config), target_(std::move(target)), sourceId_((uint64_t(GetCurrentProcessId())<<32)|++nextSource_), property_(input::WindowIdentityProperty(sourceId_)) {
        config_.allowDisplayFallback = true;
        config_.includeNv12 = config_.ownedNv12 = true;
        config_.includeNv12Readback = config_.includeBgraReadback = false;
    }
    ~WindowsCaptureSource() override {
        if(target_)target_->Invalidate(sourceId_);
        const auto window=reinterpret_cast<HWND>(config_.windowHandle);
        if(target_ && config_.sourceType==CaptureSourceType::Window &&
            GetPropW(window,property_.c_str())==reinterpret_cast<HANDLE>(sourceId_))RemovePropW(window,property_.c_str());
    }
    void Start() override {
        capture_.Start(config_);
        if(target_ && config_.sourceType==CaptureSourceType::Window)
            SetPropW(reinterpret_cast<HWND>(config_.windowHandle),property_.c_str(),reinterpret_cast<HANDLE>(sourceId_));
    }
    std::optional<CaptureSample> Poll() override {
        try {
            auto frame = capture_.TryCaptureFrame(std::chrono::milliseconds(10));
            if (!frame) { if(target_) { if(auto target=Target())target_->Touch(*target);else target_->Invalidate(sourceId_); } return std::nullopt; }
            const auto captured = std::chrono::steady_clock::now();
            if (!device_) device_ = std::make_shared<D3dVideoDevice>(frame->d3dDevice);
            auto resource = std::make_shared<WindowsCaptureResource>();
            resource->buffer = device_->RetainCapture(*frame);
            resource->device = device_;
            if(target_) {
                const auto target=Target();
                if(target && target->width==frame->sourceWidth && target->height==frame->sourceHeight)
                    resource->inputGeneration=target_->Publish(*target);
                else target_->Invalidate(sourceId_);
            }
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
    void Retire() noexcept override { if(target_)target_->Invalidate(sourceId_); if (device_) device_->Retire(); }
    void Rebuild() override { capture_.RebuildDevice(); device_.reset(); }
private:
    std::optional<input::DesktopTarget> Target() const {
        const auto bounds=capture_.InputBounds();if(!bounds)return {};
        input::DesktopTarget result{sourceId_,config_.sourceType==CaptureSourceType::Window?config_.windowHandle:0,0,
            bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top};
        if(result.window) {
            const auto window=reinterpret_cast<HWND>(result.window);
            if(GetPropW(window,property_.c_str())!=reinterpret_cast<HANDLE>(sourceId_))return {};
            DWORD pid=0;GetWindowThreadProcessId(window,&pid);result.process=pid;
        }
        return result;
    }
    inline static std::atomic<uint64_t> nextSource_{0};
    std::shared_ptr<input::DesktopTargetState> target_;
    const uint64_t sourceId_;
    const std::wstring property_;
    CaptureConfig config_;
    DesktopCapturer capture_;
    std::shared_ptr<D3dVideoDevice> device_;
};
}
