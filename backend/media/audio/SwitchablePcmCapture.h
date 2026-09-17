#pragma once
#include "PcmAudioEndpoint.h"
#include "SilentPcmCapture.h"
#include "media/AudioSelection.h"
#include <condition_variable>
#include <algorithm>
#include <optional>
#include <mutex>
#include <thread>

namespace screenshare::media {
// One command across all recording incarnations. Endpoint factories and native
// endpoint destruction execute on their own capture workers, including failures.
class AudioSwitchControl {
public:
    using Factory = std::function<std::unique_ptr<PcmCaptureEndpoint>()>;
    struct Request { Factory factory; AudioSelection selected; std::promise<AudioUpdateResult> reply; };
    AudioSwitchControl(AudioSelection initial, Factory factory) {
        ValidateAudioSelection(initial);
        factory_ = SelectFactory(initial, std::move(factory));
        status_ = {std::move(initial), 1};
    }
    Factory Attach() { std::lock_guard lock(mutex_); active_ = !closed_; return factory_; }
    void Detach() { std::lock_guard lock(mutex_); active_ = false; status_.health.state = AudioEndpointState::Inactive; CancelQueued(); }
    void Close() { std::lock_guard lock(mutex_); closed_ = true; status_.health.state = AudioEndpointState::Inactive; CancelQueued(); }
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
        if (closed_ || !active_) return CaptureUpdateReady(AudioUpdateError::Unavailable);
        if (busy_) return CaptureUpdateReady(AudioUpdateError::Busy);
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
private:
    void SetReady() { status_.health.state = status_.selected.kind == AudioKind::None ? AudioEndpointState::Silent : AudioEndpointState::Running; }
    static Factory SelectFactory(const AudioSelection& selected, Factory factory) {
        if (selected.kind == AudioKind::None) return [] { return std::make_unique<SilentPcmCapture>(); };
        return factory;
    }
    void CancelQueued() {
        if (queued_) { queued_->reply.set_value({AudioUpdateError::Cancelled, status_.revision}); queued_.reset(); busy_ = false; }
    }
    mutable std::mutex mutex_;
    Factory factory_;
    AudioSelectionStatus status_;
    std::unique_ptr<Request> queued_;
    bool active_ = false, busy_ = false, closed_ = false;
};

class SwitchablePcmCapture final : public PcmCaptureEndpoint {
    struct Producer {
        std::mutex mutex;
        std::condition_variable_any ready;
        std::optional<PcmBlock> latest;
        std::chrono::steady_clock::time_point capturedAt;
        bool started = false, failed = false;
        uint32_t delay = 0;
        uint64_t dropped = 0;
        // Last member: joins before any state the worker uses is destroyed.
        std::jthread worker;
        explicit Producer(AudioSwitchControl::Factory factory) : worker([this, factory = std::move(factory)](std::stop_token stop) {
            try {
                auto device = factory();
                if (!device) throw std::runtime_error("Missing audio capture endpoint");
                device->Start(stop);
                { std::lock_guard lock(mutex); started = true; } ready.notify_all();
                PcmBlock block;
                uint64_t overflow = 0;
                while (!stop.stop_requested() && device->Read(block, stop)) {
                    if (stop.stop_requested()) break;
                    {
                        std::lock_guard lock(mutex);
                        if (latest) overflow += 480;
                        latest = block; capturedAt = std::chrono::steady_clock::now();
                        delay = device->DelayMs(); dropped = device->DroppedFrames() + overflow;
                    }
                    ready.notify_all();
                }
            } catch (...) {}
            { std::lock_guard lock(mutex); failed = true; } ready.notify_all();
        }) {}
        void Start(std::stop_token stop) {
            std::unique_lock lock(mutex); ready.wait(lock, stop, [&] { return started || failed; });
            if (stop.stop_requested()) worker.request_stop();
            if (!started || failed || stop.stop_requested()) throw std::runtime_error("Audio capture startup failed");
        }
        bool Pop(PcmBlock& block, uint32_t& latency, uint64_t& lost, std::stop_token stop = {}, bool wait = false) {
            std::unique_lock lock(mutex);
            if (wait) ready.wait_for(lock, stop, std::chrono::milliseconds(10), [&] { return latest.has_value() || failed; });
            if (!latest || stop.stop_requested()) return false;
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - capturedAt).count();
            if (age > 30) { latest.reset(); return false; }
            block = *latest; latest.reset(); latency = delay + uint32_t(age); lost = dropped; return true;
        }
        bool Failed() { std::lock_guard lock(mutex); return failed; }
    };
    std::shared_ptr<AudioSwitchControl> control_;
    std::unique_ptr<Producer> current_, candidate_;
    std::unique_ptr<AudioSwitchControl::Request> request_;
    std::chrono::steady_clock::time_point deadline_;
    uint32_t delay_ = 0;
    uint64_t dropped_ = 0, retiredDropped_ = 0;
    SilentPcmCapture silence_;
    void Fail() { current_.reset(); delay_ = 0; silence_.Start(); control_->Failed(); }
    void Finish(AudioUpdateError error) { candidate_.reset(); control_->Complete(std::move(request_), error); }
public:
    explicit SwitchablePcmCapture(std::shared_ptr<AudioSwitchControl> control) : control_(std::move(control)) {}
    ~SwitchablePcmCapture() override { control_->Detach(); Finish(AudioUpdateError::Cancelled); }
    void Start() override { Start({}); }
    void Start(std::stop_token stop) override {
        try {
            current_ = std::make_unique<Producer>(control_->Attach()); current_->Start(stop); control_->Ready();
        } catch (...) {
            current_.reset();
            if (stop.stop_requested()) throw;
            Fail();
        }
    }
    bool Read(PcmBlock& block, std::stop_token stop) override {
        // Follow the endpoint's 10ms cadence, without a second independent clock.
        // A stalled device waits at most 10ms; each producer retains one block.
        if (stop.stop_requested()) return false;
        if (!request_) {
            request_ = control_->Take();
            if (request_) {
                deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                try { candidate_ = std::make_unique<Producer>(request_->factory); }
                catch (...) { Finish(AudioUpdateError::Failed); }
            }
        }
        if (candidate_) {
            uint64_t lost = 0;
            if (candidate_->Failed()) Finish(AudioUpdateError::Failed);
            else if (candidate_->Pop(block, delay_, lost)) {
                retiredDropped_ = dropped_; current_ = std::move(candidate_);
                dropped_ = retiredDropped_ + lost; Finish(AudioUpdateError::None); return true;
            } else if (std::chrono::steady_clock::now() >= deadline_) Finish(AudioUpdateError::Timeout);
        }
        if (current_ && current_->Failed()) Fail();
        if (!current_) return silence_.Read(block, stop);
        uint64_t lost = 0;
        if (current_->Pop(block, delay_, lost, stop, true)) dropped_ = retiredDropped_ + lost;
        else { block.fill(0); delay_ = 0; }
        return !stop.stop_requested();
    }
    uint32_t DelayMs() const override { return delay_; }
    uint64_t DroppedFrames() const override { return dropped_; }
};
}
