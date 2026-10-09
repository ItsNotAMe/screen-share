#pragma once
#include "CaptureSession.h"
#include "media/webrtc/D3dVideoFrameBuffer.h"
#include "input/v2/DesktopTarget.h"
#include "media/DiagnosticHistory.h"
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
    explicit WindowsCaptureSource(CaptureConfig config, std::shared_ptr<input::DesktopTargetState> target = {}, std::shared_ptr<DiagnosticHistory> diagnostics = {}) : config_(config), target_(std::move(target)), sourceId_((uint64_t(GetCurrentProcessId())<<32)|++nextSource_), property_(input::WindowIdentityProperty(sourceId_)), diagnostics_(std::move(diagnostics)) {
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
        if(target_ && config_.sourceType==CaptureSourceType::Window &&
            !SetPropW(reinterpret_cast<HWND>(config_.windowHandle),property_.c_str(),reinterpret_cast<HANDLE>(sourceId_))) {
            const auto error = GetLastError();
            if(diagnostics_)diagnostics_->Event("input-window-marker-failed", error);
        }
    }
    std::optional<CaptureSample> Poll() override {
        try {
            const auto now = std::chrono::steady_clock::now();
            if (now < nextFrame_) return std::nullopt;
            // WGC/DXGI may stop producing frames when the desktop is unchanged.
            // Sample the last owned texture at the configured cadence instead
            // of waiting for damage (or WebRTC's slow idle refresh).
            auto frame = capture_.TryCaptureFrame(std::chrono::milliseconds(0));
            if (Closed()) { retained_.reset(); if (target_) target_->Invalidate(sourceId_); return std::nullopt; }
            if (Minimized()) {
                retained_.reset();
                // A fullscreen game may minimize when the host opens the Grant
                // controls. Preserve its last captured input identity while
                // paused; never publish new geometry or replay old pixels.
                if (target_) {
                    const auto window = reinterpret_cast<HWND>(config_.windowHandle);
                    DWORD process = 0; GetWindowThreadProcessId(window, &process);
                    if (inputTarget_ && process == inputTarget_->process && IsWindowVisible(window) &&
                        GetPropW(window, property_.c_str()) == reinterpret_cast<HANDLE>(sourceId_))
                        target_->Touch(*inputTarget_);
                    else target_->Invalidate(sourceId_);
                }
                return std::nullopt;
            }
            const auto period = std::chrono::nanoseconds(1000000000 / std::max(1, config_.targetFps));
            nextFrame_ += period;
            if(nextFrame_ <= now)nextFrame_ = now + period; // Skip missed slots, without accumulating poll jitter.
            if (!frame) {
                if(target_) {
                    if(inputTarget_) {
                        if(auto target=Target(inputTarget_->width,inputTarget_->height))target_->Touch(*target);
                        else target_->Invalidate(sourceId_);
                    } else target_->Invalidate(sourceId_);
                    if(retained_ && target_->Read().generation != retained_->inputGeneration)retained_.reset();
                }
                if(retained_)return CaptureSample{retained_,now};
                return std::nullopt;
            }
            const auto captured = std::chrono::steady_clock::now();
            if (!device_) device_ = std::make_shared<D3dVideoDevice>(frame->d3dDevice);
            auto resource = std::make_shared<WindowsCaptureResource>();
            resource->buffer = device_->RetainCapture(*frame);
            resource->device = device_;
            if(target_) {
                const auto target=Target(frame->sourceWidth,frame->sourceHeight);
                const int availability = !target ? 0 : target->mouseMapped ? 2 : 1;
                if(diagnostics_ && inputAvailability_ != availability) {
                    diagnostics_->Event(!target ? "capture-input-target-unavailable" : target->mouseMapped ?
                        "capture-input-target-ready" : "capture-input-keyboard-only");
                    inputAvailability_ = availability;
                }
                if(target) {
                    resource->inputGeneration=target_->Publish(*target);
                    inputTarget_ = *target;
                }
                else target_->Invalidate(sourceId_);
            }
            retained_ = resource;
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
    void Retire() noexcept override { retained_.reset(); if(target_)target_->Invalidate(sourceId_); if (device_) device_->Retire(); }
    void Rebuild() override { retained_.reset(); nextFrame_={}; capture_.RebuildDevice(); device_.reset(); }
private:
    std::optional<input::DesktopTarget> Target(int width,int height) const {
        const auto bounds=capture_.InputBounds();
        if(!bounds && config_.sourceType!=CaptureSourceType::Window)return {};
        input::DesktopTarget result{sourceId_,config_.sourceType==CaptureSourceType::Window?config_.windowHandle:0,0,
            bounds?bounds->left:0,bounds?bounds->top:0,bounds?bounds->right-bounds->left:0,bounds?bounds->bottom-bounds->top:0};
        if(result.window) {
            const auto window=reinterpret_cast<HWND>(result.window);
            if(!IsWindowVisible(window) || GetPropW(window,property_.c_str())!=reinterpret_cast<HANDLE>(sourceId_))return {};
            DWORD pid=0;if(!GetWindowThreadProcessId(window,&pid) || !pid)return {};result.process=pid;
        }
        result=input::CapturedInputTarget(result,width,height);
        return result.Valid()?std::optional(result):std::nullopt;
    }
    inline static std::atomic<uint64_t> nextSource_{0};
    std::shared_ptr<input::DesktopTargetState> target_;
    const uint64_t sourceId_;
    const std::wstring property_;
    CaptureConfig config_;
    DesktopCapturer capture_;
    std::shared_ptr<D3dVideoDevice> device_;
    std::shared_ptr<WindowsCaptureResource> retained_;
    std::optional<input::DesktopTarget> inputTarget_;
    std::shared_ptr<DiagnosticHistory> diagnostics_;
    int inputAvailability_ = -1;
    std::chrono::steady_clock::time_point nextFrame_;
};
}
