#pragma once
#include "PcmAudioEndpoint.h"
#include "SilentPcmCapture.h"
#include "media/AudioSelection.h"
#include <optional>
#include <utility>
#include <algorithm>
#include <chrono>

namespace screenshare::media {
// Resolve on the audio worker and replace there, preserving COM ownership.
// A failed or vanished window produces silence, never system-audio fallback.
// Retry the same validated target with capped backoff; a new selected target
// bypasses the old target's delay. No recovery work runs after cancellation.
class SourcePcmCapture final : public PcmCaptureEndpoint {
public:
    using Selection = std::function<AudioSelection()>;
    using Factory = std::function<std::unique_ptr<PcmCaptureEndpoint>(AudioSelection)>;
    SourcePcmCapture(Selection selection, Factory factory, std::function<void(bool)> health,
        std::function<std::chrono::steady_clock::time_point()> now = std::chrono::steady_clock::now)
        : selection_(std::move(selection)), factory_(std::move(factory)), health_(std::move(health)), now_(std::move(now)) {}
    void Start() override {
        Retire(); retiredDropped_ = 0; selected_.reset();
        retryAt_ = now_(); retryDelay_ = std::chrono::milliseconds(250); silence_.Start();
    }
    bool Read(PcmBlock& block, std::stop_token stop) override {
        if (stop.stop_requested()) return false;
        bool attemptedCapture = false;
        try {
            const auto selected = Resolve();
            ValidateAudioSelection(selected);
            if (selected.kind != AudioKind::System && selected.kind != AudioKind::Process)
                throw std::invalid_argument("Shared-source audio must resolve to system or process audio");
            if (!selected_ || *selected_ != selected) {
                Retire(); // Drop pending PCM from the previous source first.
                selected_ = selected;
                retryAt_ = now_(); retryDelay_ = std::chrono::milliseconds(250);
            }
            if (!current_ && now_() >= retryAt_) {
                attemptedCapture = true;
                auto candidate = factory_(selected);
                if (!candidate) throw std::runtime_error("Missing shared-source audio endpoint");
                candidate->Start(stop);
                if (stop.stop_requested()) return false;
                current_ = std::move(candidate);
            }
            if (current_) {
                attemptedCapture = true;
                if (!current_->Read(block, stop)) {
                    if (stop.stop_requested()) return false;
                    throw std::runtime_error("Shared-source audio endpoint stopped");
                }
                if (stop.stop_requested()) return false;
                // A blocking device read may span a source change. Discard it.
                if (Resolve() != selected) {
                    Retire(); selected_.reset(); health_(false);
                    return silence_.Read(block, stop);
                }
                if (stop.stop_requested()) return false;
                retryDelay_ = std::chrono::milliseconds(250);
                health_(true);
                return !stop.stop_requested();
            }
        } catch (...) {
            Retire();
            if (stop.stop_requested()) return false;
            if (attemptedCapture) {
                retryAt_ = now_() + retryDelay_;
                retryDelay_ = std::min(retryDelay_ * 2, std::chrono::milliseconds(5000));
            }
        }
        if (stop.stop_requested()) return false;
        health_(false);
        return silence_.Read(block, stop);
    }
    uint32_t DelayMs() const override { return current_ ? current_->DelayMs() : 0; }
    uint64_t DroppedFrames() const override { return retiredDropped_ + (current_ ? current_->DroppedFrames() : 0); }
private:
    void Retire() { if (current_) { retiredDropped_ += current_->DroppedFrames(); current_.reset(); } }
    AudioSelection Resolve() { return selection_(); }
    Selection selection_;
    Factory factory_;
    std::function<void(bool)> health_;
    std::function<std::chrono::steady_clock::time_point()> now_;
    std::chrono::steady_clock::time_point retryAt_;
    std::chrono::milliseconds retryDelay_{250};
    std::optional<AudioSelection> selected_;
    std::unique_ptr<PcmCaptureEndpoint> current_;
    uint64_t retiredDropped_ = 0;
    SilentPcmCapture silence_;
};
}
