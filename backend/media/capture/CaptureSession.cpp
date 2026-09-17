#include "CaptureSession.h"
#include "CaptureRecovery.h"
#include <condition_variable>
#include <mutex>
#include <thread>

namespace screenshare::media {
struct CaptureSession::Impl {
    mutable std::mutex mutex;
    std::condition_variable_any wake;
    CaptureStatus status;
    bool enabled = false;
    std::jthread worker;

    void Run(std::stop_token stop, Factory factory, Deliver deliver,
             std::chrono::milliseconds startupTimeout) noexcept {
        std::unique_ptr<ICaptureSource> source;
        CaptureState finalState = CaptureState::Stopped;
        CaptureFailure failure = CaptureFailure::Source;
        try {
            source = factory();
            if (!source) throw std::runtime_error("Missing capture source");
            source->Start();
            { std::lock_guard lock(mutex); status.source = source->Info(); }
            CaptureRecovery recovery;
            bool awaitingFrame = true;
            auto deadline = std::chrono::steady_clock::now() + startupTimeout;
            uint64_t sequence = 0;
            while (!stop.stop_requested()) {
                failure = CaptureFailure::Recovery;
                const bool ready = recovery.Poll([&] {
                    source->Rebuild();
                    awaitingFrame = true;
                    deadline = std::chrono::steady_clock::now() + startupTimeout;
                });
                if (ready) {
                    failure = CaptureFailure::Source;
                    std::optional<CaptureSample> sample;
                    try { sample = source->Poll(); }
                    catch (const CaptureLost&) {
                        failure = CaptureFailure::Recovery;
                        recovery.Lost([&] { source->Retire(); });
                        std::lock_guard lock(mutex);
                        status.state = CaptureState::Recovering;
                        continue;
                    }
                    if (source->Closed()) { finalState = CaptureState::Closed; break; }
                    { std::lock_guard lock(mutex); status.source = source->Info(); }
                    if (source->Minimized()) {
                        // A selected minimized window is an intentional pause,
                        // including before its first frame. Resume gets a fresh
                        // startup deadline; do not deliver queued old pixels.
                        deadline = std::chrono::steady_clock::now() + startupTimeout;
                        std::lock_guard lock(mutex);
                        status.state = CaptureState::Minimized;
                    } else if (sample) {
                        if (!sample->resource) throw std::runtime_error("Missing captured resource");
                        awaitingFrame = false;
                        bool send;
                        {
                            std::lock_guard lock(mutex);
                            status.state = CaptureState::Running;
                            status.generation = recovery.generation();
                            sample->session = status.session;
                            send = enabled;
                        }
                        sample->generation = recovery.generation();
                        sample->sequence = ++sequence;
                        if (send && !stop.stop_requested()) {
                            failure = CaptureFailure::Consumer;
                            deliver(std::move(*sample));
                            std::lock_guard lock(mutex);
                            ++status.delivered;
                        }
                    } else if (awaitingFrame && std::chrono::steady_clock::now() >= deadline) {
                        failure = CaptureFailure::StartupTimeout;
                        throw std::runtime_error("Capture startup frame timed out");
                    }
                }
                // Cancellable idle/backoff; no catch-up frame queue. Sources pace
                // their own acquisition and must not block indefinitely in Poll.
                std::unique_lock lock(mutex);
                wake.wait_for(lock, stop, std::chrono::milliseconds(1), [] { return false; });
            }
        } catch (...) { finalState = source && source->Closed() ? CaptureState::Closed : CaptureState::Failed; }
        // Normal stop preserves already-published owned frames until consumers
        // release them. Only failed devices invalidate outstanding resources.
        if (source && finalState == CaptureState::Failed) source->Retire();
        source.reset(); // Native teardown stays on the capture owner, even on error.
        std::lock_guard lock(mutex);
        status.state = finalState;
        status.failure = finalState == CaptureState::Failed ? failure : CaptureFailure::None;
    }
};
CaptureSession::CaptureSession(uint64_t session, Factory factory, Deliver deliver,
                               std::chrono::milliseconds timeout) : impl_(std::make_unique<Impl>()) {
    if (!session || !factory || !deliver || timeout <= std::chrono::milliseconds::zero())
        throw std::invalid_argument("Invalid capture session configuration");
    impl_->status.session = session;
    impl_->worker = std::jthread([this, factory = std::move(factory), deliver = std::move(deliver), timeout]
                                (std::stop_token stop) mutable {
        impl_->Run(stop, std::move(factory), std::move(deliver), timeout);
    });
}
CaptureSession::~CaptureSession() { Stop(); }
void CaptureSession::EnableDelivery() { std::lock_guard lock(impl_->mutex); impl_->enabled = true; }
void CaptureSession::RequestStop() noexcept { impl_->worker.request_stop(); }
void CaptureSession::Stop() {
    RequestStop();
    if (impl_->worker.joinable()) impl_->worker.join();
}
CaptureStatus CaptureSession::status() const { std::lock_guard lock(impl_->mutex); return impl_->status; }
}
