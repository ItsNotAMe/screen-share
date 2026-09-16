#pragma once
#include "PcmAudioEndpoint.h"
#include "media/PlaybackSelection.h"
#include <mutex>

namespace screenshare::media {
class PlaybackControl {
public:
    using Factory = std::function<std::unique_ptr<PcmPlayoutEndpoint>()>;
    struct Request { PlaybackSelection selected; Factory factory; std::promise<AudioUpdateResult> reply; };
    PlaybackControl(PlaybackSelection selection, Factory factory) : status_{std::move(selection), 1}, factory_(std::move(factory)) {}
    std::pair<PlaybackSelection, Factory> Attach() { std::lock_guard lock(mutex_); active_ = !closed_; return {status_.selected, factory_}; }
    void Detach() { std::lock_guard lock(mutex_); active_ = false; Cancel(); }
    void Close() { std::lock_guard lock(mutex_); closed_ = true; Cancel(); }
    std::future<AudioUpdateResult> Submit(PlaybackSelection selection, Factory factory) {
        std::lock_guard lock(mutex_);
        if (closed_ || !active_) return CaptureUpdateReady(AudioUpdateError::Unavailable);
        if (busy_) return CaptureUpdateReady(AudioUpdateError::Busy);
        queued_ = std::make_unique<Request>(Request{std::move(selection), std::move(factory), {}}); busy_ = true;
        return queued_->reply.get_future();
    }
    std::unique_ptr<Request> Take() { std::lock_guard lock(mutex_); return std::move(queued_); }
    void Complete(std::unique_ptr<Request> request, AudioUpdateError error) {
        if (!request) return;
        std::lock_guard lock(mutex_);
        if (closed_) error = AudioUpdateError::Cancelled;
        if (error == AudioUpdateError::None) { status_.selected = request->selected; factory_ = request->factory; ++status_.revision; }
        busy_ = false; request->reply.set_value({error, status_.revision});
    }
    PlaybackStatus Status() const { std::lock_guard lock(mutex_); return status_; }
private:
    void Cancel() {
        if (queued_) { queued_->reply.set_value({AudioUpdateError::Cancelled, status_.revision}); queued_.reset(); busy_ = false; }
    }
    mutable std::mutex mutex_;
    PlaybackStatus status_;
    Factory factory_;
    std::unique_ptr<Request> queued_;
    bool active_ = false, busy_ = false, closed_ = false;
};

// Runs entirely on the existing ADM playout worker. No extra audio queue or clock.
// Device initialization can pause local playout; it never blocks video/signaling.
class ControlledPcmPlayout final : public PcmPlayoutEndpoint {
    std::shared_ptr<PlaybackControl> control_;
    std::unique_ptr<PcmPlayoutEndpoint> device_;
    PlaybackSelection selected_;
    static PcmBlock Apply(PcmBlock block, const PlaybackSelection& selected) {
        const auto gain = selected.muted ? 0 : int(selected.volume);
        for (auto& sample : block) sample = int16_t(int(sample) * gain / 100);
        return block;
    }
public:
    explicit ControlledPcmPlayout(std::shared_ptr<PlaybackControl> control) : control_(std::move(control)) {}
    ~ControlledPcmPlayout() override { control_->Detach(); }
    void Start() override {
        auto [selected, factory] = control_->Attach(); selected_ = std::move(selected); device_ = factory();
        if (!device_) throw std::runtime_error("Missing playback endpoint");
        device_->Start();
    }
    void Write(const PcmBlock& block, std::stop_token stop) override {
        if (stop.stop_requested()) return;
        if (auto request = control_->Take()) {
            try {
                std::unique_ptr<PcmPlayoutEndpoint> replacement;
                if (request->selected.deviceId != selected_.deviceId) {
                    replacement = request->factory();
                    if (!replacement) throw std::runtime_error("Missing replacement playback endpoint");
                    replacement->Start();
                }
                if (stop.stop_requested()) { control_->Complete(std::move(request), AudioUpdateError::Cancelled); return; }
                (replacement ? replacement.get() : device_.get())->Write(Apply(block, request->selected), stop);
                if (stop.stop_requested()) { control_->Complete(std::move(request), AudioUpdateError::Cancelled); return; }
                if (replacement) device_ = std::move(replacement);
                selected_ = request->selected; control_->Complete(std::move(request), AudioUpdateError::None); return;
            } catch (...) { control_->Complete(std::move(request), AudioUpdateError::Failed); }
        }
        device_->Write(Apply(block, selected_), stop);
    }
    uint32_t DelayMs() const override { return device_->DelayMs(); }
    uint32_t BufferFrames() const override { return device_->BufferFrames(); }
    uint32_t EnginePeriodUs() const override { return device_->EnginePeriodUs(); }
};
}
