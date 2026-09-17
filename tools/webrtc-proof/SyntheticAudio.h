#pragma once
#include <algorithm>
#include "media/audio/PcmAudioEndpoint.h"
#include "media/audio/SwitchablePcmCapture.h"
#include "media/audio/PlaybackControl.h"
#include "media/webrtc/MicrophoneCapture.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

namespace proof {
struct AudioEvidence {
    std::atomic<uint64_t> audibleBlocks{0}, playedBlocks{0}, quietStreak{0};
    std::atomic<int> lastPeak{0};
    std::atomic<bool> captureUnavailable{false}, outputUnavailable{false};
    std::atomic<unsigned> captureStarts{0}, outputStarts{0};
};
class ToneCapture final : public screenshare::media::PcmCaptureEndpoint {
public:
    explicit ToneCapture(bool silent = false, std::shared_ptr<AudioEvidence> evidence = {}) : silent_(silent), evidence_(std::move(evidence)) {}
    void Start() override {
        if (evidence_) { ++evidence_->captureStarts; if (evidence_->captureUnavailable) throw std::runtime_error("Injected capture startup failure"); }
        next_ = std::chrono::steady_clock::now();
    }
    bool Read(screenshare::media::PcmBlock& block, std::stop_token stop) override {
        if (evidence_ && evidence_->captureUnavailable) throw std::runtime_error("Injected capture device loss");
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
    std::shared_ptr<AudioEvidence> evidence_;
    uint64_t position_ = 0;
    std::chrono::steady_clock::time_point next_;
};
class MeasuredPlayout final : public screenshare::media::PcmPlayoutEndpoint {
public:
    explicit MeasuredPlayout(std::shared_ptr<AudioEvidence> evidence) : evidence_(std::move(evidence)) {}
    void Start() override {
        ++evidence_->outputStarts;
        if (evidence_->outputUnavailable) throw std::runtime_error("Injected output startup failure");
        next_ = std::chrono::steady_clock::now();
    }
    void Write(const screenshare::media::PcmBlock& block, std::stop_token) override {
        if (evidence_->outputUnavailable) throw std::runtime_error("Injected output device loss");
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
    return {[evidence] { return std::make_unique<ToneCapture>(false, evidence); },
        [evidence] { return std::make_unique<MeasuredPlayout>(evidence); }};
}
inline screenshare::media::AudioSwitchControl::Factory SyntheticAudioSelectionWithEvidence(screenshare::media::AudioSelection selection, std::shared_ptr<AudioEvidence> evidence) {
    return [selection, evidence]() -> std::unique_ptr<screenshare::media::PcmCaptureEndpoint> {
        if (selection.deviceId == L"invalid") throw std::runtime_error("Injected audio startup failure");
        return std::make_unique<ToneCapture>(selection.kind == screenshare::media::AudioKind::Microphone, evidence);
    };
}
inline screenshare::media::AudioSwitchControl::Factory SyntheticAudioSelection(screenshare::media::AudioSelection selection) {
    return SyntheticAudioSelectionWithEvidence(selection, {});
}
inline screenshare::media::PlaybackControl::Factory SyntheticPlayback(screenshare::media::PlaybackSelection selection, std::shared_ptr<AudioEvidence> evidence) {
    return [selection, evidence]() -> std::unique_ptr<screenshare::media::PcmPlayoutEndpoint> {
        if (selection.deviceId == L"invalid") throw std::runtime_error("Injected playback startup failure");
        return std::make_unique<MeasuredPlayout>(evidence);
    };
}
}
