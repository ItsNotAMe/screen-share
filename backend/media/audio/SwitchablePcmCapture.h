#pragma once
#include "PcmAudioEndpoint.h"
#include "SilentPcmCapture.h"
#include "AudioThreadTask.h"
#include "media/AudioSelection.h"
#include <algorithm>
#include <optional>
#include <mutex>
#include <thread>
#include <deque>

namespace screenshare::media {
// One command across all recording incarnations. Endpoint factories and native
// endpoint destruction execute on their own capture workers, including failures.
class AudioSwitchControl {
public:
    using Factory = std::function<std::unique_ptr<PcmCaptureEndpoint>()>;
    using MicrophoneProcessor = std::function<std::unique_ptr<PcmCaptureEndpoint>(std::unique_ptr<PcmCaptureEndpoint>)>;
    struct Request { Factory factory; AudioSelection selected; std::promise<AudioUpdateResult> reply; };
    AudioSwitchControl(AudioSelection initial, Factory factory, MicrophoneProcessor microphoneProcessor = {},
        std::shared_ptr<DiagnosticHistory> diagnostics = {})
        : microphoneProcessor_(std::move(microphoneProcessor)), diagnostics_(std::move(diagnostics)) {
        ValidateAudioSelection(initial);
        factory_ = SelectFactory(initial, std::move(factory));
        status_ = {std::move(initial), 1, {}, false};
        status_.microphoneProcessing = bool(microphoneProcessor_) && status_.selected.kind == AudioKind::Microphone;
    }
    Factory Attach() { std::lock_guard lock(mutex_); active_ = !closed_; return factory_; }
    Factory RecoveryFactory() const {
        std::lock_guard lock(mutex_);
        // Shared-source capture has its own validated-target recovery. Explicit
        // process IDs require a new selection rather than following PID reuse.
        if (!active_ || closed_ || busy_ || (status_.selected.kind != AudioKind::System &&
            status_.selected.kind != AudioKind::Microphone)) return {};
        return factory_;
    }
    void Detach() { std::lock_guard lock(mutex_); active_ = false; status_.health.state = AudioEndpointState::Inactive; CancelQueued(); }
    void Close() { std::lock_guard lock(mutex_); closed_ = true; status_.health.state = AudioEndpointState::Inactive; CancelQueued(); }
    bool Active() const { std::lock_guard lock(mutex_); return active_ && !closed_; }
    void InitialReady() {
        std::lock_guard lock(mutex_);
        // Shared-source audio can already have reported health from its first
        // read. Do not overwrite that report with the outer startup result.
        if (active_ && !closed_ && status_.health.state == AudioEndpointState::Inactive) SetReady();
    }
    void Ready() { std::lock_guard lock(mutex_); if (active_ && !closed_) SetReady(); }
    void Failed() {
        std::lock_guard lock(mutex_);
        if (active_ && !closed_) {
            if (status_.health.state != AudioEndpointState::Failed) ++status_.health.failures;
            status_.health.state = AudioEndpointState::Failed;
        }
    }
    std::future<AudioUpdateResult> Submit(AudioSelection selected, Factory factory) {
        ValidateAudioSelection(selected);
        factory = SelectFactory(selected, std::move(factory));
        std::lock_guard lock(mutex_);
        if (closed_) return CaptureUpdateReady(AudioUpdateError::Unavailable);
        if (busy_) return CaptureUpdateReady(AudioUpdateError::Busy);
        if (!active_) {
            // No recording worker exists yet (or the last one has detached).
            // Commit configuration only; Attach must see it before opening any
            // endpoint for the next viewer. Never activate devices on this caller.
            factory_ = std::move(factory);
            status_.selected = std::move(selected);
            ++status_.revision;
            status_.health.state = AudioEndpointState::Inactive;
            status_.microphoneProcessing = bool(microphoneProcessor_) && status_.selected.kind == AudioKind::Microphone;
            std::promise<AudioUpdateResult> reply;
            reply.set_value({AudioUpdateError::None, status_.revision});
            return reply.get_future();
        }
        queued_ = std::make_unique<Request>(Request{std::move(factory), std::move(selected), {}});
        busy_ = true; return queued_->reply.get_future();
    }
    std::unique_ptr<Request> Take() { std::lock_guard lock(mutex_); return std::move(queued_); }
    void Complete(std::unique_ptr<Request> request, AudioUpdateError error) {
        if (!request) return;
        std::lock_guard lock(mutex_);
        if (closed_) error = AudioUpdateError::Cancelled;
        if (error == AudioUpdateError::None) { factory_ = request->factory; status_.selected = request->selected; ++status_.revision; SetReady(); }
        busy_ = false; request->reply.set_value({error, status_.revision});
    }
    AudioSelectionStatus Status() const { std::lock_guard lock(mutex_); return status_; }
    const std::shared_ptr<DiagnosticHistory>& Diagnostics() const { return diagnostics_; }
private:
    void SetReady() {
        status_.health.state = status_.selected.kind == AudioKind::None ? AudioEndpointState::Silent : AudioEndpointState::Running;
        status_.microphoneProcessing = bool(microphoneProcessor_) && status_.selected.kind == AudioKind::Microphone;
    }
    Factory SelectFactory(const AudioSelection& selected, Factory factory) const {
        if (selected.kind == AudioKind::None) return [] { return std::make_unique<SilentPcmCapture>(); };
        if (selected.kind == AudioKind::Microphone && microphoneProcessor_)
            return [factory = std::move(factory), process = microphoneProcessor_] { return process(factory()); };
        return factory;
    }
    void CancelQueued() {
        if (queued_) { queued_->reply.set_value({AudioUpdateError::Cancelled, status_.revision}); queued_.reset(); busy_ = false; }
    }
    mutable std::mutex mutex_;
    const MicrophoneProcessor microphoneProcessor_;
    const std::shared_ptr<DiagnosticHistory> diagnostics_;
    Factory factory_;
    AudioSelectionStatus status_;
    std::unique_ptr<Request> queued_;
    bool active_ = false, busy_ = false, closed_ = false;
};

class SwitchablePcmCapture final : public PcmCaptureEndpoint {
    static constexpr auto kBlockDuration = std::chrono::milliseconds(10);
    static constexpr auto kMaxQueueAge = std::chrono::milliseconds(80);
    static constexpr size_t kMaxQueuedBlocks = kMaxQueueAge / kBlockDuration;
    struct Producer {
        enum class State { Opening, Started, Failed };
        std::mutex mutex;
        struct Captured { PcmBlock block; std::chrono::steady_clock::time_point at; };
        std::deque<Captured> blocks;
        State state = State::Opening;
        uint32_t delay = 0;
        uint64_t dropped = 0, expired = 0;
        // Last member: joins before any state the worker uses is destroyed.
        std::jthread worker;
        explicit Producer(AudioSwitchControl::Factory factory, std::shared_ptr<DiagnosticHistory> diagnostics)
            : worker([this, factory = std::move(factory), diagnostics = std::move(diagnostics)](std::stop_token stop) {
            try {
                if (stop.stop_requested()) return;
                auto device = factory();
                if (!device) throw std::runtime_error("Missing audio capture endpoint");
                if (stop.stop_requested()) return;
                device->Start(stop);
                AudioThreadTask scheduling(AudioWorker::DeviceCapture, diagnostics);
                { std::lock_guard lock(mutex); state = State::Started; }
                PcmBlock block;
                uint64_t overflow = 0;
                while (!stop.stop_requested() && device->Read(block, stop)) {
                    if (stop.stop_requested()) break;
                    {
                        std::lock_guard lock(mutex);
                        // Allow a burst to fill the existing freshness window.
                        // The output clock still sends one block at a time.
                        if (blocks.size() == kMaxQueuedBlocks) { blocks.pop_front(); overflow += 480; }
                        blocks.push_back({block, std::chrono::steady_clock::now()});
                        delay = device->DelayMs(); dropped = device->DroppedFrames() + overflow;
                    }
                }
            } catch (...) {}
            std::lock_guard lock(mutex); state = State::Failed;
        }) {}
        bool Pop(PcmBlock& block, uint32_t& latency, uint64_t& lost) {
            std::unique_lock lock(mutex);
            const auto now = std::chrono::steady_clock::now();
            while (!blocks.empty() && now - blocks.front().at > kMaxQueueAge) {
                blocks.pop_front(); expired += 480;
            }
            if (blocks.empty()) return false;
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - blocks.front().at).count();
            block = blocks.front().block; blocks.pop_front(); latency = delay + uint32_t(age); lost = dropped + expired; return true;
        }
        State Status() { std::lock_guard lock(mutex); return state; }
    };
    std::shared_ptr<AudioSwitchControl> control_;
    std::unique_ptr<Producer> current_, candidate_;
    std::unique_ptr<AudioSwitchControl::Request> request_;
    std::chrono::steady_clock::time_point deadline_;
    std::chrono::steady_clock::time_point nextOutput_;
    ShortWait outputTimer_;
    uint32_t delay_ = 0;
    uint64_t dropped_ = 0, retiredDropped_ = 0;
    std::function<std::chrono::steady_clock::time_point()> now_;
    std::chrono::steady_clock::time_point retryAt_, pcmDeadline_;
    std::chrono::milliseconds retryDelay_{250};
    bool opening_ = false, recovering_ = false;
    void Fail() {
        current_.reset(); opening_ = recovering_ = false; delay_ = 0;
        retiredDropped_ = dropped_; control_->Failed();
        retryAt_ = now_() + retryDelay_;
        retryDelay_ = std::min(retryDelay_ * 2, std::chrono::milliseconds(5000));
    }
    void Finish(AudioUpdateError error) { candidate_.reset(); control_->Complete(std::move(request_), error); }
public:
    explicit SwitchablePcmCapture(std::shared_ptr<AudioSwitchControl> control,
        std::function<std::chrono::steady_clock::time_point()> now = std::chrono::steady_clock::now)
        : control_(std::move(control)), now_(std::move(now)) {}
    ~SwitchablePcmCapture() override { control_->Detach(); Finish(AudioUpdateError::Cancelled); }
    void Start() override { Start({}); }
    void Start(std::stop_token stop) override {
        if (stop.stop_requested()) throw std::runtime_error("Audio capture startup cancelled");
        retryDelay_ = std::chrono::milliseconds(250); opening_ = recovering_ = false;
        try {
            auto factory = control_->Attach();
            if (control_->Active()) {
                // WebRTC waits for this Start call on its media worker. Begin
                // the paced PCM timeline while device activation runs separately.
                pcmDeadline_ = now_() + std::chrono::seconds(5);
                current_ = std::make_unique<Producer>(std::move(factory), control_->Diagnostics());
                opening_ = true;
            }
        } catch (...) {
            if (stop.stop_requested()) throw;
            Fail();
        }
        // Allow up to 60ms of startup prefill without waiting for activation.
        // The device may deliver several 10ms blocks together after a short stall.
        nextOutput_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    }
    bool Read(PcmBlock& block, std::stop_token stop) override {
        // One 10ms output slot covers either captured PCM or replacement silence.
        // A timeout followed by a late block must not create two slots: that
        // overfeeds NetEq and makes A/V synchronization delay healthy video.
        if (stop.stop_requested()) return false;
        const auto now = std::chrono::steady_clock::now();
        nextOutput_ += kBlockDuration;
        if (nextOutput_ < now) nextOutput_ = now; // Never replay missed slots.
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < nextOutput_) {
            const auto remaining = nextOutput_ - std::chrono::steady_clock::now();
            if (remaining > std::chrono::steady_clock::duration::zero())
                outputTimer_.Wait(std::min(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining),
                    std::chrono::nanoseconds(kBlockDuration)));
        }
        if (stop.stop_requested()) return false;
        if (!request_) {
            request_ = control_->Take();
            if (request_) {
                // A manual change supersedes an automatic activation, including
                // mute. Cancellation/destruction stays on the producer owner.
                if (recovering_) { current_.reset(); recovering_ = false; delay_ = 0; }
                deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                try { candidate_ = std::make_unique<Producer>(request_->factory, control_->Diagnostics()); }
                catch (...) { Finish(AudioUpdateError::Failed); }
            }
        }
        if (candidate_) {
            uint64_t lost = 0;
            if (candidate_->Status() == Producer::State::Failed) Finish(AudioUpdateError::Failed);
            else if (candidate_->Pop(block, delay_, lost)) {
                retiredDropped_ = dropped_; current_ = std::move(candidate_);
                opening_ = false;
                dropped_ = retiredDropped_ + lost;
                retryDelay_ = std::chrono::milliseconds(250);
                pcmDeadline_ = now_() + std::chrono::seconds(5);
                Finish(AudioUpdateError::None); return true;
            } else if (std::chrono::steady_clock::now() >= deadline_) Finish(AudioUpdateError::Timeout);
        }
        if (opening_ && !control_->Active()) { current_.reset(); opening_ = false; }
        if (recovering_ && !control_->RecoveryFactory()) { current_.reset(); recovering_ = false; delay_ = 0; }
        if (current_) {
            const auto state = current_->Status();
            if (state == Producer::State::Failed) Fail();
            else if (opening_ && state == Producer::State::Started) {
                opening_ = false;
                control_->InitialReady();
            }
        }
        if (!current_ && !request_ && !stop.stop_requested() && now_() >= retryAt_) {
            if (auto factory = control_->RecoveryFactory()) {
                try {
                    current_ = std::make_unique<Producer>(std::move(factory), control_->Diagnostics());
                    recovering_ = true; pcmDeadline_ = now_() + std::chrono::seconds(5);
                } catch (...) { Fail(); }
            }
        }
        if (!current_) { block.fill(0); return true; }
        uint64_t lost = 0;
        if (current_->Pop(block, delay_, lost)) {
            dropped_ = retiredDropped_ + lost;
            // Valid PCM, including silence, proves delivery. Observe queued
            // fresh audio before expiring a deadline from a delayed owner tick.
            pcmDeadline_ = now_() + std::chrono::seconds(5);
            if (recovering_) {
                recovering_ = false; retryDelay_ = std::chrono::milliseconds(250);
                control_->Ready();
            }
        }
        else {
            block.fill(0); delay_ = 0;
            // Bound initial activation as well as missing PCM from an open
            // recoverable device. Teardown still requires driver cooperation.
            // Shared-source capture owns validated-target recovery itself;
            // manual selection, Close and nonrecoverable targets retain priority.
            if (now_() >= pcmDeadline_ && (opening_ || control_->RecoveryFactory())) Fail();
        }
        return !stop.stop_requested();
    }
    uint32_t DelayMs() const override { return delay_; }
    uint64_t DroppedFrames() const override { return dropped_; }
};
}
