#include "media/capture/CaptureSession.h"
#include "media/capture/SyntheticCaptureSource.h"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <mutex>
#include <thread>

using namespace screenshare::media;
using Clock = std::chrono::steady_clock;
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class Predicate> void Wait(Predicate predicate) {
    const auto deadline = Clock::now() + std::chrono::seconds(4);
    while (!predicate()) {
        Require(Clock::now() < deadline, "Scenario deadline exceeded");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
struct Evidence {
    std::atomic<int> living{0}, retired{0}, rebuilt{0};
    std::atomic<bool> loss{false}, closed{false}, wrongThread{false}, silent{false};
};
class ObservedSource final : public ICaptureSource {
public:
    explicit ObservedSource(Evidence& e) : evidence_(e), owner_(std::this_thread::get_id()), source_(64, 36, 120) { ++e.living; }
    ~ObservedSource() override { Check(); --evidence_.living; }
    void Start() override { Check(); source_.Start(); }
    std::optional<CaptureSample> Poll() override {
        Check();
        if (evidence_.loss.exchange(false)) throw CaptureLost();
        if (evidence_.silent) return std::nullopt;
        return source_.Poll();
    }
    bool Closed() const override { return evidence_.closed; }
    void Retire() noexcept override { Check(); ++evidence_.retired; }
    void Rebuild() override { Check(); ++evidence_.rebuilt; source_.Rebuild(); }
private:
    void Check() noexcept { if (owner_ != std::this_thread::get_id()) evidence_.wrongThread = true; }
    Evidence& evidence_;
    std::thread::id owner_;
    SyntheticCaptureSource source_;
};
int main() try {
    Evidence evidence;
    std::vector<double> ages;
    std::mutex samples;
    uint64_t previousSequence = 0;
    CaptureSession session(42, [&] { return std::make_unique<ObservedSource>(evidence); }, [&](auto sample) {
        Require(sample.session == 42 && sample.sequence > previousSequence, "Session/sequence identity lost");
        previousSequence = sample.sequence;
        auto pixels = std::dynamic_pointer_cast<SyntheticCaptureResource>(sample.resource);
        Require(pixels && pixels->luma.size() == 64 * 36, "No owned synthetic pixels");
        std::lock_guard lock(samples);
        ages.push_back(std::chrono::duration<double, std::milli>(Clock::now() - sample.capturedAt).count());
    });
    Wait([&] { return session.status().state == CaptureState::Running; });
    Require(session.status().delivered == 0, "Delivered before subscriber enabled");
    session.EnableDelivery();
    Wait([&] { return session.status().delivered >= 20; });
    for (uint64_t generation = 2; generation <= 4; ++generation) {
        evidence.loss = true;
        Wait([&] { return session.status().state == CaptureState::Recovering; });
        Wait([&] { return session.status().generation == generation; });
    }
    evidence.loss = true;
    Wait([&] { return session.status().state == CaptureState::Failed; });
    session.Stop(); session.Stop();
    Require(session.status().failure == CaptureFailure::Recovery, "Recovery failure not classified");
    Require(evidence.rebuilt == 3 && evidence.living == 0 && !evidence.wrongThread, "Recovery ownership/budget failed");

    // Restart storms use new generation owners; joining is the callback barrier.
    for (uint64_t generation = 43; generation < 143; ++generation) {
        std::atomic<int> calls{0};
        CaptureSession restart(generation, [&] { return std::make_unique<ObservedSource>(evidence); },
            [&](auto sample) { Require(sample.session == generation, "Stale session frame"); ++calls; });
        restart.EnableDelivery();
        Wait([&] { return calls > 0; });
        restart.Stop();
        const int finished = calls;
        Require(restart.status().state == CaptureState::Stopped && calls == finished && evidence.living == 0,
                "Stop failed to join source/callback");
    }
    // A slow consumer must not cause a burst of old source frames.
    std::atomic<int> slowFrames{0};
    CaptureSession slow(143, [] { return std::make_unique<SyntheticCaptureSource>(64, 36, 120); },
        [&](auto sample) {
            Require(Clock::now() - sample.capturedAt < std::chrono::milliseconds(100), "Old raw-frame backlog");
            ++slowFrames;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        });
    slow.EnableDelivery();
    Wait([&] { return slowFrames >= 10; }); slow.Stop();

    evidence.loss = false;
    CaptureSession cancel(144, [&] { return std::make_unique<ObservedSource>(evidence); }, [](auto) {});
    Wait([&] { return cancel.status().state == CaptureState::Running; });
    evidence.loss = true;
    Wait([&] { return cancel.status().state == CaptureState::Recovering; });
    const auto before = Clock::now(); cancel.Stop();
    const double stopMs = std::chrono::duration<double, std::milli>(Clock::now() - before).count();
    Require(stopMs < 200, "Stop waited for retry timer");
    evidence.closed = true;
    CaptureSession close(145, [&] { return std::make_unique<ObservedSource>(evidence); }, [](auto) {});
    Wait([&] { return close.status().state == CaptureState::Closed; }); close.Stop();
    CaptureSession throwing(146, [] { return std::make_unique<SyntheticCaptureSource>(); },
                            [](auto) { throw std::runtime_error("Consumer failure"); });
    throwing.EnableDelivery();
    Wait([&] { return throwing.status().state == CaptureState::Failed; }); throwing.Stop();
    Require(throwing.status().failure == CaptureFailure::Consumer, "Consumer failure not classified");
    evidence.closed = false; evidence.silent = true;
    CaptureSession silent(147, [&] { return std::make_unique<ObservedSource>(evidence); }, [](auto) {},
                          std::chrono::milliseconds(50));
    Wait([&] { return silent.status().state == CaptureState::Failed; }); silent.Stop();
    Require(silent.status().failure == CaptureFailure::StartupTimeout, "Missing source did not time out");
    CaptureSession* callbackOwner = nullptr;
    CaptureSession callbackStop(148, [] { return std::make_unique<SyntheticCaptureSource>(); },
                               [&](auto) { callbackOwner->RequestStop(); });
    callbackOwner = &callbackStop;
    callbackStop.EnableDelivery();
    Wait([&] { return callbackStop.status().state == CaptureState::Stopped; }); callbackStop.Stop();
    Require(evidence.living == 0 && !evidence.wrongThread, "Leaked source or wrong destruction thread");
    std::sort(ages.begin(), ages.end());
    auto percentile = [&](double p) { return ages[static_cast<size_t>((ages.size() - 1) * p)]; };
    std::cout << "{\"passed\":true,\"mode\":\"headless-capture-session\",\"restart_cycles\":100,"
              << "\"samples\":" << ages.size() << ",\"capture_to_callback_ms\":{\"p50\":" << percentile(.50)
              << ",\"p95\":" << percentile(.95) << ",\"p99\":" << percentile(.99)
              << "},\"backoff_stop_ms\":" << stopMs << "}\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
