#pragma once
#include "PcmAudioEndpoint.h"
#include "SilentPcmCapture.h"
#include <stdexcept>

namespace screenshare::media {
// Follow an endpoint's default-device notifications on its existing capture
// worker. Explicit devices/process capture never report a route change.
class RoutedPcmCapture final : public PcmCaptureEndpoint {
public:
    using Factory = std::function<std::unique_ptr<PcmCaptureEndpoint>()>;
    explicit RoutedPcmCapture(Factory factory) : factory_(std::move(factory)) {}
    void Start() override { Start({}); }
    void Start(std::stop_token stop) override {
        Retire(); retiredDrops_ = 0; silence_.Start();
        if (!Open(stop)) throw std::runtime_error("Default capture startup cancelled");
    }
    bool Read(PcmBlock& block, std::stop_token stop) override {
        if (stop.stop_requested()) return false;
        if (!current_) throw std::runtime_error("Default capture is not started");
        if (!current_->DeviceChanged()) {
            try {
                const bool read = current_->Read(block, stop);
                if (stop.stop_requested()) return false;
                if (!current_->DeviceChanged()) return read;
            } catch (...) {
                if (stop.stop_requested()) return false;
                if (!current_->DeviceChanged()) throw;
            }
        }
        // Discard in-flight PCM as well as the old endpoint's queue. A change
        // during replacement startup is handled on the next read, never a loop
        // of activations here. Prime with paced silence before live new audio.
        Retire();
        if (!Open(stop)) return false;
        return silence_.Read(block, stop);
    }
    uint32_t DelayMs() const override { return current_ ? current_->DelayMs() : 0; }
    uint64_t DroppedFrames() const override { return retiredDrops_ + (current_ ? current_->DroppedFrames() : 0); }
private:
    bool Open(std::stop_token stop) {
        if (stop.stop_requested()) return false;
        auto candidate = factory_();
        if (!candidate) throw std::runtime_error("Missing default capture endpoint");
        if (stop.stop_requested()) return false;
        candidate->Start(stop);
        if (stop.stop_requested()) return false;
        current_ = std::move(candidate); return true;
    }
    void Retire() { if (current_) { retiredDrops_ += current_->DroppedFrames(); current_.reset(); } }
    Factory factory_;
    std::unique_ptr<PcmCaptureEndpoint> current_;
    SilentPcmCapture silence_;
    uint64_t retiredDrops_ = 0;
};
}
