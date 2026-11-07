#pragma once
#include "CaptureSession.h"
#include "CaptureCadence.h"
#include <atomic>
#include <vector>

namespace screenshare::media {
struct SyntheticCaptureResource final : CaptureResource {
    int width = 0, height = 0;
    std::vector<uint8_t> luma;
};
// CPU-only deterministic moving pattern, paced against a monotonic clock.
// Falling behind drops missed production opportunities rather than catching up.
class SyntheticCaptureSource final : public ICaptureSource {
public:
    SyntheticCaptureSource(int width = 640, int height = 360, int fps = 60,
        std::shared_ptr<std::atomic<bool>> demand = {})
        : width_(width), height_(height), demand_(std::move(demand)), cadence_(fps) {
        if (width < 2 || height < 2 || width > 3840 || height > 2160 || fps < 1 || fps > 240)
            throw std::invalid_argument("Invalid synthetic capture dimensions/rate");
    }
    void Start() override { cadence_.Reset(); }
    std::optional<CaptureSample> Poll() override {
        const auto now = std::chrono::steady_clock::now();
        if (!cadence_.Due(now, !demand_ || demand_->load(std::memory_order_relaxed))) return std::nullopt;
        cadence_.Sampled(now);
        auto frame = std::make_shared<SyntheticCaptureResource>();
        frame->width = width_; frame->height = height_;
        frame->luma.resize(static_cast<size_t>(width_) * height_);
        for (int y = 0; y < height_; ++y)
            for (int x = 0; x < width_; ++x)
                frame->luma[static_cast<size_t>(y) * width_ + x] =
                    static_cast<uint8_t>(50 + ((x + y + index_) % 100));
        ++index_;
        return CaptureSample{std::move(frame), now};
    }
    bool Closed() const override { return false; }
    void Retire() noexcept override {}
    void Rebuild() override { Start(); }
private:
    int width_, height_;
    uint64_t index_ = 0;
    const std::shared_ptr<std::atomic<bool>> demand_;
    CaptureCadence cadence_;
};
}
