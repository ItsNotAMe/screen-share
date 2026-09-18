#pragma once
#include "media/capture/SyntheticCaptureSource.h"
#include <atomic>

namespace proof {
struct CaptureLifetime {
    std::atomic<unsigned> sources{0}, resources{0}, peakResources{0};
};
// Test-only accounting around the real portable capture contract. The aliasing
// owner preserves the original resource type/data and follows every shared copy.
class ObservedCaptureSource final : public screenshare::media::ICaptureSource {
    screenshare::media::SyntheticCaptureSource source_{640, 360, 30};
    std::shared_ptr<CaptureLifetime> lifetime_;
public:
    explicit ObservedCaptureSource(std::shared_ptr<CaptureLifetime> lifetime) : lifetime_(std::move(lifetime)) { ++lifetime_->sources; }
    ~ObservedCaptureSource() override { --lifetime_->sources; }
    void Start() override { source_.Start(); }
    std::optional<screenshare::media::CaptureSample> Poll() override {
        auto sample = source_.Poll();
        if (sample) {
            auto original = std::move(sample->resource);
            auto* raw = original.get();
            const auto count = ++lifetime_->resources;
            auto peak = lifetime_->peakResources.load();
            while (peak < count && !lifetime_->peakResources.compare_exchange_weak(peak, count)) {}
            sample->resource = std::shared_ptr<screenshare::media::CaptureResource>(raw,
                [original = std::move(original), lifetime = lifetime_](auto*) mutable {
                    original.reset(); --lifetime->resources;
                });
        }
        return sample;
    }
    bool Closed() const override { return source_.Closed(); }
    void Retire() noexcept override { source_.Retire(); }
    void Rebuild() override { source_.Rebuild(); }
};
}
