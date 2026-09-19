#include "media/HostMediaSession.h"
#include "media/capture/SyntheticCaptureSource.h"
#include "media/capture/SwitchableCaptureSource.h"
#include <atomic>
#include <iostream>
#include <thread>

using namespace screenshare::media;
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class Predicate> void Wait(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!predicate()) {
        Require(std::chrono::steady_clock::now() < deadline, "Coordinator scenario timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
struct OwnedSource final : ICaptureSource {
    std::atomic<int>& living;
    std::atomic<bool>& wrongThread;
    std::thread::id owner = std::this_thread::get_id();
    SyntheticCaptureSource source{64, 36, 120};
    OwnedSource(std::atomic<int>& count, std::atomic<bool>& wrong) : living(count), wrongThread(wrong) { ++living; }
    ~OwnedSource() override { if (owner != std::this_thread::get_id()) wrongThread = true; --living; }
    void Start() override { source.Start(); }
    std::optional<CaptureSample> Poll() override { return source.Poll(); }
    bool Closed() const override { return false; }
    void Retire() noexcept override {}
    void Rebuild() override { source.Rebuild(); }
};
void CheckSourceSwitching() {
    using namespace std::chrono_literals;
    struct EmptySource final : ICaptureSource {
        void Start() override {} std::optional<CaptureSample> Poll() override { return {}; }
        bool Closed() const override { return false; } void Retire() noexcept override {} void Rebuild() override {}
    };
    auto control = std::make_shared<CaptureSwitchControl>(CaptureSelection{});
    std::atomic<int> living{0}; std::atomic<bool> wrongThread{false}; std::atomic<unsigned> frames{0};
    auto source = [&]() -> std::unique_ptr<ICaptureSource> { return std::make_unique<OwnedSource>(living, wrongThread); };
    CaptureSession capture(1, [=] { return std::make_unique<SwitchableCaptureSource>(source, control); }, [&](auto) { ++frames; });
    capture.EnableDelivery(); Wait([&] { return frames >= 3; });
    auto failed = control->Submit({}, []() -> std::unique_ptr<ICaptureSource> { throw std::runtime_error("Injected startup failure"); });
    Require(failed.wait_for(1s) == std::future_status::ready && failed.get().error == CaptureUpdateError::Failed, "Failed replacement did not resolve");
    auto timed = control->Submit({}, [] { return std::make_unique<EmptySource>(); });
    Require(control->Submit({}, source).get().error == CaptureUpdateError::Busy, "Source changes were not bounded");
    const auto before = frames.load();
    Require(timed.wait_for(6s) == std::future_status::ready && timed.get().error == CaptureUpdateError::Timeout, "Missing-frame replacement did not time out");
    Require(frames > before + 20 && control->Status().revision == 1, "Failed switch interrupted the working source");
    auto changed = control->Submit({CaptureKind::Display, 1, 0, 60}, source);
    Require(changed.wait_for(1s) == std::future_status::ready && changed.get().error == CaptureUpdateError::None && control->Status().revision == 2,
        "Healthy replacement did not commit");
    auto cancelled = control->Submit({}, [] { return std::make_unique<EmptySource>(); }); capture.Stop();
    Require(cancelled.wait_for(0s) == std::future_status::ready && cancelled.get().error == CaptureUpdateError::Cancelled,
        "Stop did not cancel pending capture selection");
    Require(living == 0 && !wrongThread, "Switched source leaked or was destroyed off its owner");
}
void CheckSourceStates() {
    struct StateSource final : ICaptureSource {
        std::atomic<bool>& minimized;
        std::atomic<bool>& closed;
        SyntheticCaptureSource pixels{64, 36, 120};
        StateSource(std::atomic<bool>& paused, std::atomic<bool>& gone) : minimized(paused), closed(gone) {}
        void Start() override { pixels.Start(); }
        std::optional<CaptureSample> Poll() override { return pixels.Poll(); }
        bool Closed() const override { return closed; }
        bool Minimized() const override { return minimized; }
        CaptureSourceInfo Info() const override { return {CaptureImplementation::DesktopDuplication, true}; }
        void Retire() noexcept override {} void Rebuild() override { pixels.Rebuild(); }
    };
    std::atomic<bool> minimized{true}, closed{false};
    HostMediaSession host;
    auto control = std::make_shared<CaptureSwitchControl>(CaptureSelection{});
    const auto started = host.Start([&] {
        return std::make_unique<SwitchableCaptureSource>([&] { return std::make_unique<StateSource>(minimized, closed); }, control);
    }).get();
    Wait([&] { return host.snapshot().state == HostMediaState::Minimized; });
    Require(host.snapshot().captureSource.fallback && host.snapshot().captureSource.implementation == CaptureImplementation::DesktopDuplication,
        "Source wrapper/coordinator dropped backend diagnostics");
    minimized = false;
    Wait([&] { return host.snapshot().state == HostMediaState::WaitingForViewers; });
    auto switched = control->Submit({}, [] { return std::make_unique<SyntheticCaptureSource>(64, 36, 120); });
    // Observation follows the committed source, not the original backend.
    Wait([&] { return host.snapshot().captureSource.implementation == CaptureImplementation::Unknown; });
    Require(switched.get().error == CaptureUpdateError::None, "Observed source switch did not commit");
    host.Stop(started.generation).get();
    host.Start([&] { return std::make_unique<StateSource>(minimized, closed); }).get();
    Wait([&] { return host.snapshot().state == HostMediaState::WaitingForViewers; });
    closed = true;
    Wait([&] { return host.snapshot().state == HostMediaState::SourceClosed; });
    Require(host.snapshot().captureFailure == CaptureFailure::None, "Closed source became host failure");
}
int main() try {
    CheckSourceSwitching();
    CheckSourceStates();
    std::atomic<int> living{0};
    std::atomic<bool> wrongThread{false};
    auto factory = [&] { return std::make_unique<OwnedSource>(living, wrongThread); };
    HostMediaSession session;
    Require(session.snapshot().state == HostMediaState::Idle, "Initial session state is wrong");
    uint64_t previousOperation = 0, previousGeneration = 0;
    for (int i = 0; i < 100; ++i) {
        auto started = session.Start(factory).get();
        Require(started.error == HostOperationError::None && started.generation > previousGeneration && started.operation > previousOperation,
                "Session/operation identities were reused");
        const auto generation = started.generation;
        auto frames = std::make_shared<std::atomic<unsigned>>(0);
        auto added = session.AddViewer(generation, 1, 1, [&, generation, frames](auto sample) {
            if (sample.session != generation) wrongThread = true;
            ++*frames;
        }).get();
        Require(added.error == HostOperationError::None, "Viewer attachment failed");
        Wait([&] { return *frames >= 1 && session.snapshot().state == HostMediaState::Running; });
        auto stopped = session.Stop(generation).get();
        Require(stopped.error == HostOperationError::None && session.snapshot().state == HostMediaState::Stopped && living == 0,
                "Stop returned before source destruction");
        Require(session.Stop(generation).get().error == HostOperationError::None, "Repeated stop failed");
        previousOperation = stopped.operation; previousGeneration = generation;
    }
    auto generation = session.Start(factory).get().generation;
    Require(session.Stop(previousGeneration).get().error == HostOperationError::StaleGeneration,
            "Old stop affected a new session");
    Wait([&] { return session.snapshot().state == HostMediaState::WaitingForViewers; });
    Require(session.AddViewer(previousGeneration, 1, 1, [](auto) {}).get().error == HostOperationError::StaleGeneration,
            "Old viewer operation affected a new session");
    auto healthy = std::make_shared<std::atomic<int>>(0);
    Require(session.AddViewer(generation, 7, 7, [](auto) { throw std::runtime_error("Viewer failed"); }).get().error == HostOperationError::None,
            "Failure test subscriber was not accepted");
    Require(session.AddViewer(generation, 8, 8, [healthy](auto) { ++*healthy; }).get().error == HostOperationError::None,
            "Healthy subscriber was not accepted");
    Wait([&] { return *healthy >= 5 && session.snapshot().lastFailedViewer == 7; });
    Require(session.snapshot().state == HostMediaState::Running && session.snapshot().viewerCount == 1,
            "Viewer failure stopped healthy media");
    Require(session.snapshot().lastFailedConnectionGeneration == 7, "Failure lost its connection identity");
    Require(session.RemoveViewer(generation, 8, 8).get().error == HostOperationError::None, "Removal failed");
    Require(session.AddViewer(generation, 8, 8, [](auto) {}).get().error == HostOperationError::StaleGeneration,
            "Retired attach resurrected a connection");
    Require(session.AddViewer(generation, 8, 9, [healthy](auto) { ++*healthy; }).get().error == HostOperationError::None,
            "Replacement attach failed");
    const auto beforeStaleRemoval = healthy->load();
    Require(session.RemoveViewer(generation, 8, 8).get().error == HostOperationError::StaleGeneration,
            "Delayed cleanup removed the replacement");
    Wait([&] { return *healthy >= beforeStaleRemoval + 3; });
    Require(session.snapshot().viewers.front().connectionGeneration == 9, "Snapshot has stale connection identity");
    session.Stop(generation).get();
    auto failed = session.Start([]() -> std::unique_ptr<ICaptureSource> { throw std::runtime_error("Startup failure"); }).get();
    Wait([&] { return session.snapshot().state == HostMediaState::Failed; });
    Require(session.snapshot().captureFailure == CaptureFailure::Source, "Startup failure was hidden");
    Require(session.Stop(failed.generation).get().error == HostOperationError::None, "Failed-session cleanup failed");

    // Block one callback so removal holds the control worker, then verify queue
    // bounds and that Stop cancels pending work instead of being rejected.
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<bool> entered{false};
    generation = session.Start(factory).get().generation;
    struct Unblock {
        std::promise<void>& release;
        HostMediaSession& session;
        uint64_t generation;
        ~Unblock() { try { release.set_value(); } catch (...) {} session.Stop(generation).get(); }
    } cleanup{release, session, generation};
    auto attached = session.AddViewer(generation, 1, 1, [&](auto) { entered = true; released.wait(); }).get();
    Wait([&] { return entered.load(); });
    auto removing = session.RemoveViewer(generation, 1, 1);
    Wait([&] { return session.snapshot().activeOperation == attached.operation + 1; });
    std::vector<std::future<HostOperationResult>> queued;
    for (int i = 0; i < 80; ++i) queued.push_back(session.AddViewer(generation, i + 2, i + 2, [](auto) {}));
    auto stopping = session.Stop(generation);
    release.set_value();
    Require(removing.get().error == HostOperationError::None && stopping.get().error == HostOperationError::None,
            "Control pressure prevented cancellation");
    int cancelled = 0, capacity = 0;
    for (auto& item : queued) {
        auto error = item.get().error;
        cancelled += error == HostOperationError::Cancelled;
        capacity += error == HostOperationError::Capacity;
        Require(error == HostOperationError::Cancelled || error == HostOperationError::Capacity,
                "Queued viewer survived stop");
    }
    Require(cancelled > 0 && capacity > 0 && living == 0 && !wrongThread, "Queue bound or owner teardown failed");
    std::cout << "{\"passed\":true,\"mode\":\"host-session-coordinator\",\"restart_cycles\":100,"
              << "\"cancelled_commands\":" << cancelled << ",\"capacity_rejections\":" << capacity << "}\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
