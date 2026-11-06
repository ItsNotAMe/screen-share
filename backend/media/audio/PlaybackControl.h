#pragma once
#include "PcmAudioEndpoint.h"
#include "DiscardPcmPlayout.h"
#include "media/PlaybackSelection.h"
#include <mutex>

namespace screenshare::media {
class PlaybackControl {
public:
    using Factory = std::function<std::unique_ptr<PcmPlayoutEndpoint>()>;
    struct Request { PlaybackSelection selected; Factory factory; std::promise<AudioUpdateResult> reply; };
    PlaybackControl(PlaybackSelection selection, Factory factory) : status_{std::move(selection), 1, {}}, factory_(std::move(factory)) { ValidatePlaybackSelection(status_.selected); }
    std::pair<PlaybackSelection, Factory> Attach() { std::lock_guard lock(mutex_); active_ = !closed_; return {status_.selected, factory_}; }
    void Detach() { std::lock_guard lock(mutex_); active_ = false; status_.health.state = AudioEndpointState::Inactive; Cancel(); }
    void Close() { std::lock_guard lock(mutex_); closed_ = true; status_.health.state = AudioEndpointState::Inactive; Cancel(); }
    void Ready() { std::lock_guard lock(mutex_); if (active_ && !closed_) status_.health.state = AudioEndpointState::Running; }
    void Failed() {
        std::lock_guard lock(mutex_);
        if (active_ && !closed_) {
            if (status_.health.state != AudioEndpointState::Failed) ++status_.health.failures;
            status_.health.state = AudioEndpointState::Failed;
        }
    }
    std::future<AudioUpdateResult> Submit(PlaybackSelection selection, Factory factory) {
        ValidatePlaybackSelection(selection);
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
        if (error == AudioUpdateError::None) { status_.selected = request->selected; factory_ = request->factory; ++status_.revision; status_.health.state = AudioEndpointState::Running; }
        busy_ = false; request->reply.set_value({error, status_.revision});
    }
    PlaybackStatus Status() const { std::lock_guard lock(mutex_); return status_; }
    bool CanRecover() const { std::lock_guard lock(mutex_); return active_ && !closed_ && !busy_; }
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

// Runs entirely on the existing ADM playout worker, without an extra audio queue.
// Healthy output owns pacing; paced discard keeps callbacks alive after failure.
// Device initialization can pause local playout; it never blocks video/signaling.
class ControlledPcmPlayout final : public PcmPlayoutEndpoint {
    std::shared_ptr<PlaybackControl> control_;
    std::unique_ptr<PcmPlayoutEndpoint> device_;
    PlaybackSelection selected_;
    PlaybackControl::Factory factory_;
    std::function<std::chrono::steady_clock::time_point()> now_;
    std::chrono::steady_clock::time_point retryAt_;
    std::chrono::milliseconds retryDelay_{250};
    DiscardPcmPlayout discard_;
    void Reroute() {
        device_.reset(); discard_.Start();
        retryAt_ = now_(); retryDelay_ = std::chrono::milliseconds(250);
    }
    void Fail() {
        device_.reset(); discard_.Start(); control_->Failed();
        retryAt_ = now_() + retryDelay_;
        retryDelay_ = std::min(retryDelay_ * 2, std::chrono::milliseconds(5000));
    }
    static std::unique_ptr<PcmPlayoutEndpoint> Open(const PlaybackControl::Factory& factory) {
        auto device = factory();
        if (!device) throw std::runtime_error("Missing playback endpoint");
        device->Start(); return device;
    }
    static PcmBlock Apply(PcmBlock block, const PlaybackSelection& selected) {
        const auto gain = selected.muted ? 0 : int(selected.volume);
        for (auto& sample : block) sample = int16_t(int(sample) * gain / 100);
        return block;
    }
public:
    explicit ControlledPcmPlayout(std::shared_ptr<PlaybackControl> control,
        std::function<std::chrono::steady_clock::time_point()> now = std::chrono::steady_clock::now)
        : control_(std::move(control)), now_(std::move(now)) {}
    ~ControlledPcmPlayout() override { control_->Detach(); }
    void Start() override {
        auto [selected, factory] = control_->Attach(); selected_ = std::move(selected);
        factory_ = std::move(factory);
        if (!control_->CanRecover()) { discard_.Start(); return; }
        try {
            device_ = Open(factory_); control_->Ready();
        } catch (...) { Fail(); }
    }
    void Write(const PcmBlock& block, std::stop_token stop) override {
        if (stop.stop_requested()) return;
        if (device_ && device_->DeviceChanged()) Reroute();
        if (auto request = control_->Take()) {
            try {
                std::unique_ptr<PcmPlayoutEndpoint> replacement;
                if (!device_ || request->selected.deviceId != selected_.deviceId) {
                    replacement = Open(request->factory);
                }
                if (stop.stop_requested()) { control_->Complete(std::move(request), AudioUpdateError::Cancelled); return; }
                (replacement ? replacement.get() : device_.get())->Write(Apply(block, request->selected), stop);
                if (stop.stop_requested()) { control_->Complete(std::move(request), AudioUpdateError::Cancelled); return; }
                if (replacement) device_ = std::move(replacement);
                selected_ = request->selected; factory_ = request->factory;
                retryDelay_ = std::chrono::milliseconds(250);
                control_->Complete(std::move(request), AudioUpdateError::None); return;
            } catch (...) {
                control_->Complete(std::move(request), AudioUpdateError::Failed);
                if (!device_) Fail();
            }
        }
        if (!device_ && now_() >= retryAt_ && control_->CanRecover()) {
            try {
                auto replacement = Open(factory_);
                if (stop.stop_requested() || !control_->CanRecover()) return;
                // Activation may have blocked. Drop that in-flight audio and
                // validate with silence; the next callback supplies live PCM.
                replacement->Write(PcmBlock{}, stop);
                if (stop.stop_requested() || !control_->CanRecover()) return;
                device_ = std::move(replacement); retryDelay_ = std::chrono::milliseconds(250);
                control_->Ready(); return;
            } catch (...) { if (stop.stop_requested()) return; Fail(); }
        }
        if (device_) {
            try { device_->Write(Apply(block, selected_), stop); return; }
            catch (...) {
                if (stop.stop_requested()) return;
                if (device_->DeviceChanged()) Reroute(); else Fail();
            }
        }
        discard_.Write(block, stop);
    }
    uint32_t DelayMs() const override { return device_ ? device_->DelayMs() : 0; }
    uint32_t BufferFrames() const override { return device_ ? device_->BufferFrames() : 0; }
    uint32_t EnginePeriodUs() const override { return device_ ? device_->EnginePeriodUs() : 0; }
};
}
