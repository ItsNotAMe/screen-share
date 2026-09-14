#pragma once
#include "media/webrtc/D3dVideoFrameBuffer.h"
#include <future>
#include <functional>
#include <thread>

namespace proof {
// Proof-only source: all WGC/COM lifecycle calls stay on this capture worker.
class LiveCaptureSource {
public:
    using Deliver = std::function<void(webrtc::scoped_refptr<screenshare::media::D3dVideoFrameBuffer>)>;
    LiveCaptureSource(HWND window, Deliver deliver) {
        std::promise<std::shared_ptr<screenshare::media::D3dVideoDevice>> ready;
        auto started = ready.get_future();
        worker_ = std::jthread([this, window, deliver = std::move(deliver), ready = std::move(ready)](std::stop_token stop) mutable {
            bool announced = false;
            try {
                screenshare::DesktopCapturer capture;
                screenshare::CaptureConfig config;
                config.sourceType = screenshare::CaptureSourceType::Window;
                config.windowHandle = reinterpret_cast<uint64_t>(window);
                config.targetWidth = 640; config.targetHeight = 360;
                config.includeNv12 = config.ownedNv12 = true;
                config.includeNv12Readback = config.includeBgraReadback = false;
                capture.Start(config);
                std::shared_ptr<screenshare::media::D3dVideoDevice> device;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!stop.stop_requested()) {
                    auto frame = capture.TryCaptureFrame(std::chrono::milliseconds(10));
                    if (!frame) {
                        if (!announced && std::chrono::steady_clock::now() >= deadline)
                            throw std::runtime_error("Live source startup frame timed out");
                        continue;
                    }
                    if (!device) {
                        device = std::make_shared<screenshare::media::D3dVideoDevice>(frame->d3dDevice);
                        announced = true; ready.set_value(device);
                    }
                    if (enabled_) {
                        deliver(device->RetainCapture(*frame));
                        ++frames;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                capture.Stop();
            } catch (...) {
                failed = true;
                if (!announced) ready.set_exception(std::current_exception());
            }
        });
        device_ = started.get();
    }
    ~LiveCaptureSource() { Stop(); }
    void StartDelivery() { enabled_ = true; }
    void Stop() { worker_.request_stop(); if (worker_.joinable()) worker_.join(); }
    std::shared_ptr<screenshare::media::D3dVideoDevice> device() const { return device_; }
    std::atomic<bool> failed{false};
    std::atomic<unsigned> frames{0};
private:
    std::atomic<bool> enabled_{false};
    std::shared_ptr<screenshare::media::D3dVideoDevice> device_;
    std::jthread worker_;
};
}
