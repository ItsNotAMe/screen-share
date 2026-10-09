#pragma once
#include "PcmAudioEndpoint.h"
#include "SilentPcmCapture.h"
#include "media/AudioSelection.h"
#include <optional>
#include <utility>

namespace screenshare::media {
// Resolve on the audio worker and replace there, preserving COM ownership.
// A failed or vanished window produces silence, never system-audio fallback.
class SourcePcmCapture final : public PcmCaptureEndpoint {
public:
    using Selection = std::function<AudioSelection()>;
    using Factory = std::function<std::unique_ptr<PcmCaptureEndpoint>(AudioSelection)>;
    SourcePcmCapture(Selection selection, Factory factory, std::function<void(bool)> health)
        : selection_(std::move(selection)), factory_(std::move(factory)), health_(std::move(health)) {}
    void Start() override { silence_.Start(); }
    bool Read(PcmBlock& block, std::stop_token stop) override {
        if (stop.stop_requested()) return false;
        try {
            const auto selected = Resolve();
            ValidateAudioSelection(selected);
            if (selected.kind != AudioKind::System && selected.kind != AudioKind::Process)
                throw std::invalid_argument("Shared-source audio must resolve to system or process audio");
            if (!selected_ || *selected_ != selected) {
                current_.reset(); // Drop pending PCM from the previous source first.
                selected_ = selected;
                auto candidate = factory_(selected);
                if (!candidate) throw std::runtime_error("Missing shared-source audio endpoint");
                candidate->Start(stop);
                current_ = std::move(candidate);
            }
            if (current_) {
                if (!current_->Read(block, stop)) {
                    if (stop.stop_requested()) return false;
                    throw std::runtime_error("Shared-source audio endpoint stopped");
                }
                // A blocking device read may span a source change. Discard it.
                if (Resolve() != selected) block.fill(0);
                health_(true);
                return !stop.stop_requested();
            }
        } catch (...) { current_.reset(); }
        if (stop.stop_requested()) return false;
        health_(false);
        return silence_.Read(block, stop);
    }
    uint32_t DelayMs() const override { return current_ ? current_->DelayMs() : 0; }
    uint64_t DroppedFrames() const override { return current_ ? current_->DroppedFrames() : 0; }
private:
    AudioSelection Resolve() {
        try { return selection_(); }
        catch (...) { selected_.reset(); throw; }
    }
    Selection selection_;
    Factory factory_;
    std::function<void(bool)> health_;
    std::optional<AudioSelection> selected_;
    std::unique_ptr<PcmCaptureEndpoint> current_;
    SilentPcmCapture silence_;
};
}
