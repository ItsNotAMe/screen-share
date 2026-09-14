#pragma once
#include "media/audio/PcmAudioEndpoint.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

namespace proof {
struct AudioEvidence { std::atomic<uint64_t> audibleBlocks{0}; };
class ToneCapture final : public screenshare::media::PcmCaptureEndpoint {
public:
    void Start() override { next_ = std::chrono::steady_clock::now(); }
    bool Read(screenshare::media::PcmBlock& block, std::stop_token stop) override {
        next_ += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next_);
        if (stop.stop_requested()) return false;
        for (size_t i = 0; i < 480; ++i) {
            block[i*2] = int16_t(4000 * std::sin(double(position_) * 2 * 3.141592653589793 * 440 / 48000));
            block[i*2+1] = int16_t(3000 * std::sin(double(position_++) * 2 * 3.141592653589793 * 660 / 48000));
        }
        return true;
    }
    uint32_t DelayMs() const override { return 0; }
private:
    uint64_t position_ = 0;
    std::chrono::steady_clock::time_point next_;
};
class MeasuredPlayout final : public screenshare::media::PcmPlayoutEndpoint {
public:
    explicit MeasuredPlayout(std::shared_ptr<AudioEvidence> evidence) : evidence_(std::move(evidence)) {}
    void Start() override { next_ = std::chrono::steady_clock::now(); }
    void Write(const screenshare::media::PcmBlock& block, std::stop_token) override {
        if (std::any_of(block.begin(), block.end(), [](int16_t value) { return std::abs(int(value)) > 100; })) ++evidence_->audibleBlocks;
        next_ += std::chrono::milliseconds(10); std::this_thread::sleep_until(next_);
    }
    uint32_t DelayMs() const override { return 0; }
    uint32_t BufferFrames() const override { return 480; }
private:
    std::shared_ptr<AudioEvidence> evidence_;
    std::chrono::steady_clock::time_point next_;
};
inline screenshare::media::PcmEndpointFactories SyntheticAudio(std::shared_ptr<AudioEvidence> evidence) {
    return {[] { return std::make_unique<ToneCapture>(); },
        [evidence] { return std::make_unique<MeasuredPlayout>(evidence); }};
}
}
