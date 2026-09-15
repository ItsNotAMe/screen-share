#pragma once
#include "media/capture/WindowsCaptureSource.h"
#include "core/WindowsMediaRuntime.h"
#include <future>

namespace proof {
// Only test setup/fault injection lives here. Production owns the capture loop.
class LiveCaptureSource {
public:
    using Deliver = std::function<void(webrtc::scoped_refptr<screenshare::media::D3dVideoFrameBuffer>)>;
    LiveCaptureSource(HWND window, Deliver deliver) {
        if (FAILED(runtime_.result())) throw std::runtime_error("MTA lease failed");
        auto ready = std::make_shared<std::promise<std::shared_ptr<screenshare::media::D3dVideoDevice>>>();
        auto started = ready->get_future();
        screenshare::CaptureConfig config;
        config.sourceType = screenshare::CaptureSourceType::Window;
        config.windowHandle = reinterpret_cast<uint64_t>(window);
        config.targetWidth = 640; config.targetHeight = 360;
        session_ = std::make_unique<screenshare::media::CaptureSession>(1,
            [this, config] { return std::make_unique<FaultSource>(config, injectLoss_); },
            [this, ready, deliver = std::move(deliver), announced = false](auto sample) mutable {
                auto resource = std::static_pointer_cast<screenshare::media::WindowsCaptureResource>(sample.resource);
                if (!announced) { ready->set_value(resource->device); announced = true; }
                generation = sample.generation;
                if (enabled_) { deliver(resource->buffer); ++frames; }
            });
        session_->EnableDelivery();
        while (started.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready) {
            auto state = session_->status().state;
            if (state == screenshare::media::CaptureState::Failed || state == screenshare::media::CaptureState::Closed)
                throw std::runtime_error("Live capture startup failed");
        }
        device_ = started.get();
    }
    ~LiveCaptureSource() { Stop(); }
    void StartDelivery() { enabled_ = true; }
    void InjectDeviceLoss() { injectLoss_ = true; }
    void Stop() { if (session_) session_->Stop(); }
    bool HasFailed() const { return session_->status().state == screenshare::media::CaptureState::Failed; }
    std::shared_ptr<screenshare::media::D3dVideoDevice> device() const { return device_; }
    std::atomic<unsigned> frames{0};
    std::atomic<uint64_t> generation{1};
private:
    class FaultSource final : public screenshare::media::ICaptureSource {
    public:
        FaultSource(screenshare::CaptureConfig config, std::atomic<bool>& loss) : source_(config), loss_(loss) {}
        void Start() override { source_.Start(); }
        std::optional<screenshare::media::CaptureSample> Poll() override {
            if (loss_.exchange(false)) throw screenshare::media::CaptureLost();
            return source_.Poll();
        }
        bool Closed() const override { return source_.Closed(); }
        void Retire() noexcept override { source_.Retire(); }
        void Rebuild() override { source_.Rebuild(); }
    private:
        screenshare::media::WindowsCaptureSource source_;
        std::atomic<bool>& loss_;
    };
    screenshare::WindowsMediaRuntime runtime_;
    std::atomic<bool> injectLoss_{false}, enabled_{false};
    std::shared_ptr<screenshare::media::D3dVideoDevice> device_;
    std::unique_ptr<screenshare::media::CaptureSession> session_;
};
}
