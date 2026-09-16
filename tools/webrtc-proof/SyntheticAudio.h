#pragma once
#include <algorithm>
#include "media/audio/PcmAudioEndpoint.h"
#include "media/audio/SwitchablePcmCapture.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

namespace proof {
struct AudioEvidence {
    std::atomic<uint64_t> audibleBlocks{0}, playedBlocks{0}, quietStreak{0};
    std::atomic<int> lastPeak{0};
};
class ToneCapture final : public screenshare::media::PcmCaptureEndpoint {
public:
    explicit ToneCapture(bool silent = false) : silent_(silent) {}
    void Start() override { next_ = std::chrono::steady_clock::now(); }
    bool Read(screenshare::media::PcmBlock& block, std::stop_token stop) override {
        next_ += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next_);
        if (stop.stop_requested()) return false;
        for (size_t i = 0; i < 480; ++i) {
            block[i*2] = int16_t(4000 * std::sin(double(position_) * 2 * 3.141592653589793 * 440 / 48000));
            block[i*2+1] = int16_t(3000 * std::sin(double(position_++) * 2 * 3.141592653589793 * 660 / 48000));
        }
        if (silent_) block.fill(0);
        return true;
    }
    uint32_t DelayMs() const override { return 0; }
private:
    bool silent_;
    uint64_t position_ = 0;
    std::chrono::steady_clock::time_point next_;
};
class MeasuredPlayout final : public screenshare::media::PcmPlayoutEndpoint {
public:
    explicit MeasuredPlayout(std::shared_ptr<AudioEvidence> evidence) : evidence_(std::move(evidence)) {}
    void Start() override { next_ = std::chrono::steady_clock::now(); }
    void Write(const screenshare::media::PcmBlock& block, std::stop_token) override {
        int peak = 0; for (const auto sample : block) peak = std::max(peak, std::abs(int(sample))); evidence_->lastPeak = peak;
        ++evidence_->playedBlocks;
        if (peak > 100) { ++evidence_->audibleBlocks; evidence_->quietStreak = 0; }
        else ++evidence_->quietStreak;
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
inline screenshare::media::AudioSwitchControl::Factory SyntheticAudioSelection(screenshare::media::AudioSelection selection) {
    return [selection]() -> std::unique_ptr<screenshare::media::PcmCaptureEndpoint> {
        if (selection.deviceId == L"invalid") throw std::runtime_error("Injected audio startup failure");
        return std::make_unique<ToneCapture>(selection.kind == screenshare::media::AudioKind::Microphone);
    };
}
}
