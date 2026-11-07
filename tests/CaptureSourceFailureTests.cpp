#include "media/capture/SwitchableCaptureSource.h"
#include "media/capture/SyntheticCaptureSource.h"
#include "media/capture/CaptureRecovery.h"
#include <atomic>
#include <iostream>
#include <thread>

using namespace screenshare::media;
using namespace std::chrono_literals;
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void Wait(F condition, const char* message = "Capture failure scenario timed out") {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!condition()) {
        Check(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(1ms);
    }
}
enum class Fault { None, Read, Lost };
struct State {
    std::atomic<Fault> fault{Fault::None};
    std::atomic<bool> failStart{false}, failRebuild{false}, silent{false}, minimized{false}, wrongThread{false};
    std::atomic<unsigned> live{0}, starts{0}, retired{0}, rebuilt{0};
};
class Source final : public ICaptureSource {
    std::shared_ptr<State> state_;
    std::thread::id owner_ = std::this_thread::get_id();
    SyntheticCaptureSource pixels_{64, 36, 120};
    void Owner() noexcept { if (owner_ != std::this_thread::get_id()) state_->wrongThread = true; }
public:
    explicit Source(std::shared_ptr<State> state) : state_(std::move(state)) { ++state_->live; }
    ~Source() override { Owner(); --state_->live; }
    void Start() override {
        Owner(); ++state_->starts;
        if (state_->failStart) throw std::runtime_error("Injected capture startup failure");
        pixels_.Start();
    }
    std::optional<CaptureSample> Poll() override {
        Owner();
        switch (state_->fault.exchange(Fault::None)) {
        case Fault::Read: throw std::runtime_error("Injected capture read failure");
        case Fault::Lost: throw CaptureLost();
        default: break;
        }
        return state_->silent ? std::nullopt : pixels_.Poll();
    }
    bool Closed() const override { return false; }
    bool Minimized() const override { return state_->minimized; }
    void Retire() noexcept override { Owner(); ++state_->retired; pixels_.Retire(); }
    void Rebuild() override {
        Owner(); ++state_->rebuilt;
        if (state_->failRebuild) throw std::runtime_error("Injected capture rebuild failure");
        pixels_.Rebuild();
    }
};
auto Factory(std::shared_ptr<State> state) { return [state] { return std::make_unique<Source>(state); }; }
void RecoveryBudgetBoundaries() {
    for (const unsigned healthyFrames : {29u, 30u}) {
        CaptureRecovery policy;
        auto now = CaptureRecovery::Clock::time_point{};
        unsigned retired = 0, rebuilt = 0;
        for (unsigned loss = 0; loss < 5; ++loss) {
            now += 1h; // Time alone must not renew a capture budget.
            bool exhausted = false;
            try { policy.Lost([&] { ++retired; }, now); }
            catch (const std::runtime_error&) { exhausted = true; }
            if (healthyFrames == 29 && loss == 3) {
                Check(exhausted && retired == 4 && rebuilt == 3, "Short recoveries renewed the capture budget");
                // Healthy callbacks cannot resurrect a terminal device.
                for (unsigned frame = 0; frame < 100; ++frame) policy.FrameCaptured();
                bool resumed = false;
                try { policy.Poll([] {}, now); resumed = true; } catch (const std::runtime_error&) {}
                Check(!resumed, "Terminal capture resumed after health callbacks");
                break;
            }
            Check(!exhausted, "Healthy capture could not renew its recovery budget");
            // Pending recovery cannot earn credit or skip the existing backoff.
            for (unsigned frame = 0; frame < 100; ++frame) policy.FrameCaptured();
            Check(!policy.Poll([&] { ++rebuilt; }, now + 249ms), "Capture backoff was skipped");
            now += 250ms;
            Check(policy.Poll([&] { ++rebuilt; }, now) && policy.generation() == loss + 2,
                "Capture rebuild lost its generation");
            for (unsigned frame = 0; frame < healthyFrames; ++frame) policy.FrameCaptured();
        }
        if (healthyFrames == 30) Check(retired == 5 && rebuilt == 5, "Renewed capture recovery lost an attempt");
    }
    CaptureRecovery interrupted;
    auto now = CaptureRecovery::Clock::time_point{};
    for (unsigned loss = 0; loss < 3; ++loss) {
        interrupted.Lost([] {}, now); now += 250ms;
        Check(interrupted.Poll([] {}, now), "Interrupted recovery did not rebuild");
        for (unsigned frame = 0; frame < 29; ++frame) interrupted.FrameCaptured();
        interrupted.Interrupted(); // A skipped frame or explicit capture pause.
        interrupted.FrameCaptured();
    }
    bool exhausted = false;
    try { interrupted.Lost([] {}, now); } catch (const std::runtime_error&) { exhausted = true; }
    Check(exhausted, "Interrupted captures accumulated healthy-frame credit");
    const auto terminalGeneration = interrupted.generation();
    interrupted.AwaitReplacement();
    interrupted.Lost([] {}, now); now += 250ms;
    Check(interrupted.Poll([] {}, now) && interrupted.generation() == terminalGeneration + 2,
        "Explicit source replacement did not reset terminal capture recovery");
}
void Unavailable(CaptureSession& capture, CaptureFailure failure) {
    Wait([&] {
        const auto status = capture.status();
        Check(status.state != CaptureState::Failed, "Replaceable capture failure terminated the capture session");
        return status.state == CaptureState::Unavailable;
    });
    Check(capture.status().failure == failure, "Capture failure reason was lost");
}
void Replace(CaptureSession& capture, const std::shared_ptr<CaptureSwitchControl>& control, const std::shared_ptr<State>& state) {
    const auto before = capture.status();
    auto replacement = control->Submit({}, Factory(state));
    Check(replacement.wait_for(1s) == std::future_status::ready && replacement.get().error == CaptureUpdateError::None,
        "Replacement could not commit after capture failure");
    Wait([&] { return capture.status().delivered >= before.delivered + 3; });
    Check(capture.status().failure == CaptureFailure::None && capture.status().state == CaptureState::Running,
        "Fresh replacement did not clear failure state");
}
void ReadFailure() {
    auto state = std::make_shared<State>();
    auto control = std::make_shared<CaptureSwitchControl>(CaptureSelection{});
    uint64_t sequence = 0, generation = 0;
    CaptureSession capture(1, [=] { return std::make_unique<SwitchableCaptureSource>(Factory(state), control); },
        [&](auto sample) {
            Check(sample.sequence > sequence && sample.generation >= generation, "Capture identity regressed after replacement");
            sequence = sample.sequence; generation = sample.generation;
        });
    capture.EnableDelivery(); Wait([&] { return capture.status().delivered >= 3; });
    const auto initialGeneration = capture.status().generation;
    state->fault = Fault::Read; Unavailable(capture, CaptureFailure::Source);
    const auto failed = capture.status();
    Check(!state->live && state->retired > 0 && control->Status().revision == 1, "Failed source was retained or selection changed");
    auto rejected = control->Submit({}, []() -> std::unique_ptr<ICaptureSource> { throw std::runtime_error("Unavailable replacement"); });
    Check(rejected.wait_for(1s) == std::future_status::ready && rejected.get().error == CaptureUpdateError::Failed,
        "Unavailable replacement did not resolve");
    std::this_thread::sleep_for(40ms);
    Check(capture.status().state == CaptureState::Unavailable && capture.status().delivered == failed.delivered && state->starts == 1,
        "Failed source retried or published stale frames");
    Replace(capture, control, state);
    Check(control->Status().revision == 2 && capture.status().generation == failed.generation && failed.generation > initialGeneration,
        "Replacement lost capture revision/generation");
    auto pendingState = std::make_shared<State>(); pendingState->silent = true;
    auto pending = control->Submit({}, Factory(pendingState));
    Wait([&] { return pendingState->starts > 0; }); capture.Stop(); capture.Stop();
    Check(pending.wait_for(0s) == std::future_status::ready && pending.get().error == CaptureUpdateError::Cancelled,
        "Stop did not cancel the pending replacement");
    Check(!state->live && !pendingState->live && !state->wrongThread && !pendingState->wrongThread, "Capture teardown escaped its owner");
}
void RecoveryFailure(bool exhausted) {
    auto state = std::make_shared<State>();
    auto control = std::make_shared<CaptureSwitchControl>(CaptureSelection{});
    CaptureSession capture(2, [=] { return std::make_unique<SwitchableCaptureSource>(Factory(state), control); }, [](auto) {});
    capture.EnableDelivery(); Wait([&] { return capture.status().delivered >= 3; });
    if (exhausted) {
        for (uint64_t expected = 2; expected <= 4; ++expected) {
            state->fault = Fault::Lost;
            Wait([&] { return capture.status().generation == expected && capture.status().state == CaptureState::Running; });
        }
    } else state->failRebuild = true;
    state->fault = Fault::Lost; Unavailable(capture, CaptureFailure::Recovery);
    Check(!state->live && state->rebuilt == (exhausted ? 3u : 1u), "Failed recovery retained native state or retried indefinitely");
    state->failRebuild = false; Replace(capture, control, state);
    const auto generation = capture.status().generation;
    state->fault = Fault::Lost;
    Wait([&] { return capture.status().generation > generation && capture.status().state == CaptureState::Running; });
    capture.Stop(); Check(!state->live && !state->wrongThread, "Recovered capture leaked or changed owner");
}
void HealthyRecoveries() {
    auto state = std::make_shared<State>();
    CaptureSession capture(8, Factory(state), [](auto) {});
    capture.EnableDelivery();
    Wait([&] { return capture.status().delivered >= 35; });
    for (uint64_t generation = 2; generation <= 6; ++generation) {
        state->fault = Fault::Lost;
        Wait([&] {
            const auto status = capture.status();
            Check(status.state != CaptureState::Failed, "Healthy capture exhausted its lifetime recovery budget");
            return status.state == CaptureState::Running && status.generation == generation;
        });
        const auto recovered = capture.status().delivered;
        Wait([&] { return capture.status().delivered >= recovered + 30; });
    }
    capture.Stop(); capture.Stop();
    Check(state->rebuilt == 5 && state->retired == 5 && state->starts == 1 && !state->live && !state->wrongThread,
        "Healthy capture recovery lost its owner or restarted the session");
}
void MinimizedRestore() {
    for (const bool replaceable : {false, true}) {
        auto state = std::make_shared<State>();
        auto control = std::make_shared<CaptureSwitchControl>(CaptureSelection{});
        CaptureSession capture(9, [=]() -> std::unique_ptr<ICaptureSource> {
            if (replaceable) return std::make_unique<SwitchableCaptureSource>(Factory(state), control);
            return std::make_unique<Source>(state);
        }, [](auto) {}, 200ms);
        capture.EnableDelivery(); Wait([&] { return capture.status().delivered >= 3; });
        const auto generation = capture.status().generation;
        for (const bool resume : {true, false}) {
            state->minimized = true;
            Wait([&] { return capture.status().state == CaptureState::Minimized; });
            const auto delivered = capture.status().delivered;
            // Poll still produces pixels: minimization must suppress them, and
            // may last longer than the first-frame deadline without failing.
            std::this_thread::sleep_for(400ms);
            Check(capture.status().state == CaptureState::Minimized && capture.status().delivered == delivered,
                "Minimized capture timed out or delivered stale pixels");
            state->silent = true;
            state->minimized = false;
            if (resume) {
                std::this_thread::sleep_for(50ms);
                Check(capture.status().delivered == delivered && state->live == 1,
                    "Restoring capture delivered stale pixels or failed before its deadline");
                state->silent = false;
                Wait([&] { return capture.status().delivered >= delivered + 3; });
                Check(capture.status().state == CaptureState::Running && capture.status().failure == CaptureFailure::None
                    && capture.status().generation == generation && state->rebuilt == 0,
                    "Healthy restore did not resume the existing capture source");
            } else {
                const auto expected = replaceable ? CaptureState::Unavailable : CaptureState::Failed;
                Wait([&] { return capture.status().state == expected; },
                    "Silent restore never reached its missing-frame timeout");
                Check(capture.status().failure == CaptureFailure::StartupTimeout && capture.status().delivered == delivered,
                    "Silent restore lost its failure reason or published stale pixels");
                Wait([&] { return state->live == 0; });
                Check(state->retired == 1 && state->starts == 1 && state->rebuilt == 0,
                    "Silent restore retained or repeatedly reopened the failed source");
                if (replaceable) {
                    auto replacement = std::make_shared<State>();
                    Replace(capture, control, replacement);
                    capture.Stop();
                    Check(!replacement->live && !replacement->wrongThread,
                        "Restore replacement teardown escaped its owner");
                }
            }
        }
        capture.Stop(); capture.Stop();
        Check(!state->live && !state->wrongThread, "Minimized source teardown escaped its owner");
    }
}
void PendingReplacement() {
    auto old = std::make_shared<State>(), next = std::make_shared<State>(); next->silent = true;
    auto control = std::make_shared<CaptureSwitchControl>(CaptureSelection{});
    CaptureSession capture(7, [=] { return std::make_unique<SwitchableCaptureSource>(Factory(old), control); }, [](auto) {});
    capture.EnableDelivery(); Wait([&] { return capture.status().delivered >= 3; });
    auto replacement = control->Submit({}, Factory(next)); Wait([&] { return next->starts == 1; });
    old->fault = Fault::Read; Unavailable(capture, CaptureFailure::Source);
    Check(!old->live && next->live == 1 && replacement.wait_for(0s) == std::future_status::timeout,
        "Source failure discarded its pending replacement");
    const auto before = capture.status().delivered; next->silent = false;
    Check(replacement.wait_for(1s) == std::future_status::ready && replacement.get().error == CaptureUpdateError::None,
        "Pending replacement could not complete after the old source failed");
    Wait([&] { return capture.status().delivered >= before + 3; });
    Check(capture.status().failure == CaptureFailure::None && control->Status().revision == 2,
        "Pending replacement did not clear source failure");
    capture.Stop(); Check(!next->live && !old->wrongThread && !next->wrongThread, "Pending replacement cleanup escaped its owner");
}
void FailureBoundaries() {
    auto state = std::make_shared<State>();
    CaptureSession ordinary(3, Factory(state), [](auto) {});
    Wait([&] { return ordinary.status().state == CaptureState::Running; });
    state->fault = Fault::Read; Wait([&] { return ordinary.status().state == CaptureState::Failed; }); ordinary.Stop();
    Check(ordinary.status().failure == CaptureFailure::Source, "Ordinary source failure was hidden");
    auto control = std::make_shared<CaptureSwitchControl>(CaptureSelection{});
    CaptureSession consumer(4, [=] { return std::make_unique<SwitchableCaptureSource>(Factory(state), control); },
        [](auto) { throw std::runtime_error("Injected consumer failure"); });
    consumer.EnableDelivery(); Wait([&] { return consumer.status().state == CaptureState::Failed; }); consumer.Stop();
    Check(consumer.status().failure == CaptureFailure::Consumer, "Consumer failure was hidden as source unavailability");
    state->failStart = true; control = std::make_shared<CaptureSwitchControl>(CaptureSelection{});
    CaptureSession startup(5, [=] { return std::make_unique<SwitchableCaptureSource>(Factory(state), control); }, [](auto) {});
    Wait([&] { return startup.status().state == CaptureState::Failed; }); startup.Stop();
    state->failStart = false; state->silent = true; control = std::make_shared<CaptureSwitchControl>(CaptureSelection{});
    CaptureSession timeout(6, [=] { return std::make_unique<SwitchableCaptureSource>(Factory(state), control); }, [](auto) {}, 40ms);
    timeout.EnableDelivery(); Unavailable(timeout, CaptureFailure::StartupTimeout);
    state->silent = false; Replace(timeout, control, state); timeout.Stop();
    Check(!state->live && !state->wrongThread, "Failure boundary cleanup escaped its owner");
}
int main() try {
    MinimizedRestore(); RecoveryBudgetBoundaries(); HealthyRecoveries(); ReadFailure(); RecoveryFailure(false); RecoveryFailure(true); PendingReplacement(); FailureBoundaries();
    std::cout << "Capture: minimized restore/timeout, renewed/bounded recovery, read/rebuild failure, replacement, cancellation and failure boundaries passed.\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
