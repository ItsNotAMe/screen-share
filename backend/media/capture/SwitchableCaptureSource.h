#pragma once
#include "CaptureSession.h"
#include "media/CaptureSelection.h"
#include <mutex>

namespace screenshare::media {
// External threads submit one bounded request. Native source creation, polling,
// replacement and destruction all remain on the existing capture worker.
class CaptureSwitchControl {
public:
    struct Request { CaptureSession::Factory factory; CaptureSelection selected; std::promise<CaptureUpdateResult> reply; };
    explicit CaptureSwitchControl(CaptureSelection initial) { status_.selected = initial; status_.revision = 1; }
    std::future<CaptureUpdateResult> Submit(CaptureSelection selected, CaptureSession::Factory factory) {
        std::lock_guard lock(mutex_);
        if (closed_) return CaptureUpdateReady(CaptureUpdateError::Unavailable);
        if (busy_) return CaptureUpdateReady(CaptureUpdateError::Busy);
        queued_ = std::make_unique<Request>(Request{std::move(factory), selected, {}}); busy_ = true;
        return queued_->reply.get_future();
    }
    std::unique_ptr<Request> Take() { std::lock_guard lock(mutex_); return std::move(queued_); }
    void Complete(std::unique_ptr<Request> request, CaptureUpdateError error) {
        if (!request) return;
        std::lock_guard lock(mutex_);
        if (error == CaptureUpdateError::None) { status_.selected = request->selected; ++status_.revision; }
        busy_ = false; request->reply.set_value({error, status_.revision});
    }
    void Close() {
        std::lock_guard lock(mutex_); closed_ = true;
        if (queued_) { queued_->reply.set_value({CaptureUpdateError::Cancelled, status_.revision}); queued_.reset(); }
    }
    CaptureSelectionStatus Status() const { std::lock_guard lock(mutex_); return status_; }
private:
    mutable std::mutex mutex_;
    bool busy_ = false, closed_ = false;
    std::unique_ptr<Request> queued_;
    CaptureSelectionStatus status_;
};
class SwitchableCaptureSource final : public ICaptureSource {
    std::shared_ptr<CaptureSwitchControl> control_;
    std::unique_ptr<ICaptureSource> current_, candidate_;
    std::unique_ptr<CaptureSwitchControl::Request> request_;
    std::chrono::steady_clock::time_point deadline_;
    void Finish(CaptureUpdateError error) { candidate_.reset(); control_->Complete(std::move(request_), error); }
public:
    SwitchableCaptureSource(CaptureSession::Factory initial, std::shared_ptr<CaptureSwitchControl> control)
        : control_(std::move(control)), current_(initial()) { if (!current_) throw std::runtime_error("Missing initial capture source"); }
    ~SwitchableCaptureSource() override { control_->Close(); Finish(CaptureUpdateError::Cancelled); }
    void Start() override { current_->Start(); }
    std::optional<CaptureSample> Poll() override {
        if (!request_) request_ = control_->Take();
        if (request_) {
            try {
                if (!candidate_) {
                    deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    candidate_ = request_->factory();
                    if (!candidate_) throw std::runtime_error("Missing replacement capture source");
                    candidate_->Start();
                }
                auto sample = candidate_->Poll();
                if (candidate_->Closed()) throw std::runtime_error("Replacement capture source closed");
                if (sample && !candidate_->Minimized()) {
                    if (!sample->resource) throw std::runtime_error("Missing replacement frame resource");
                    current_ = std::move(candidate_); // Normal retirement retains already-owned frames.
                    Finish(CaptureUpdateError::None);
                    return sample;
                }
                if (std::chrono::steady_clock::now() >= deadline_) Finish(CaptureUpdateError::Timeout);
            } catch (...) { Finish(CaptureUpdateError::Failed); }
        }
        return current_->Poll();
    }
    bool Closed() const override { return current_->Closed(); }
    bool Minimized() const override { return current_->Minimized(); }
    CaptureSourceInfo Info() const override { return current_->Info(); }
    void Retire() noexcept override { current_->Retire(); }
    void Rebuild() override { current_->Rebuild(); }
};
}
