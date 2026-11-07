#pragma once
#include "media/audio/SwitchablePcmCapture.h"
#include <atomic>
#include <iostream>
#include <source_location>

namespace capture_audio_recovery_test {
using namespace screenshare::media;
using namespace std::chrono_literals;
inline void Check(bool value, std::source_location at = std::source_location::current()) {
    if (!value) throw std::runtime_error("Automatic host audio recovery failed at " + std::to_string(at.line()));
}
struct Evidence {
    std::atomic<bool> unavailable{true}, wrongThread{false}, blockActivation{false}, activating{false}, blockRead{false}, reading{false};
    std::atomic<unsigned> created{0}, live{0}, processors{0}, blocks{0};
    std::atomic<int16_t> marker{1200};
};
class Capture final : public PcmCaptureEndpoint {
    std::shared_ptr<Evidence> evidence_;
    std::thread::id owner_ = std::this_thread::get_id();
    SilentPcmCapture pace_;
public:
    explicit Capture(std::shared_ptr<Evidence> evidence) : evidence_(std::move(evidence)) { ++evidence_->created; ++evidence_->live; }
    ~Capture() override { if (owner_ != std::this_thread::get_id()) evidence_->wrongThread = true; --evidence_->live; }
    void Start() override { Start({}); }
    void Start(std::stop_token stop) override {
        if (evidence_->blockActivation) {
            evidence_->activating = true;
            std::mutex mutex; std::condition_variable_any wake; std::unique_lock lock(mutex);
            wake.wait(lock, stop, [] { return false; });
            throw std::runtime_error("Cancelled capture activation");
        }
        if (evidence_->unavailable) throw std::runtime_error("Capture unavailable");
        pace_.Start();
    }
    bool Read(PcmBlock& block, std::stop_token stop) override {
        if (evidence_->blockRead) {
            evidence_->reading = true;
            std::mutex mutex; std::condition_variable_any wake; std::unique_lock lock(mutex);
            wake.wait(lock, stop, [] { return false; }); return false;
        }
        if (!pace_.Read(block, stop)) return false;
        if (evidence_->unavailable) throw std::runtime_error("Capture invalidated");
        block.fill(evidence_->marker); ++evidence_->blocks; return true;
    }
    uint32_t DelayMs() const override { return 10; }
    uint64_t DroppedFrames() const override { return 7; }
};
template<class Predicate> void Pump(SwitchablePcmCapture& capture, PcmBlock& block, Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!ready()) {
        Check(std::chrono::steady_clock::now() < deadline);
        Check(capture.Read(block, {}));
    }
}
inline void Recovery() {
    for (const auto kind : {AudioKind::System, AudioKind::Microphone}) {
        auto evidence = std::make_shared<Evidence>();
        auto factory = [evidence] { return std::make_unique<Capture>(evidence); };
        auto control = std::make_shared<AudioSwitchControl>(AudioSelection{kind, L"pinned-device"}, factory,
            [evidence](std::unique_ptr<PcmCaptureEndpoint> endpoint) { ++evidence->processors; return endpoint; });
        auto now = std::chrono::steady_clock::time_point{};
        {
            SwitchablePcmCapture capture(control, [&] { return now; }); capture.Start();
            Check(control->Status().health.state == AudioEndpointState::Failed);
            PcmBlock block{};
            for (const auto delay : {250ms, 500ms, 1000ms, 2000ms, 4000ms, 5000ms, 5000ms}) {
                const auto attempts = evidence->created.load();
                now += delay - 1ms; Check(capture.Read(block, {}));
                Check(evidence->created == attempts && block.front() == 0);
                now += 1ms; Check(capture.Read(block, {}));
                Pump(capture, block, [&] { return evidence->created == attempts + 1 && evidence->live == 0; });
                Check(capture.Read(block, {})); // Observe completed failure and schedule its backoff.
                Check(control->Status().health.state == AudioEndpointState::Failed && block.front() == 0);
            }
            Check(control->Status().health.failures == 1 && control->Status().revision == 1);
            evidence->unavailable = false; now += 5000ms;
            Pump(capture, block, [&] { return control->Status().health.state == AudioEndpointState::Running; });
            Check(control->Status().health.state == AudioEndpointState::Running && block.front() == evidence->marker);
            Check(control->Status().revision == 1 && control->Status().selected.deviceId == L"pinned-device" && evidence->created == 9);
            Check(evidence->processors == (kind == AudioKind::Microphone ? evidence->created.load() : 0));
            const auto dropped = capture.DroppedFrames();
            evidence->unavailable = true;
            Pump(capture, block, [&] { return control->Status().health.state == AudioEndpointState::Failed; });
            Check(block.front() == 0 && capture.DelayMs() == 0);
            const auto attempts = evidence->created.load();
            evidence->unavailable = false; evidence->marker = 2300;
            now += 249ms; Check(capture.Read(block, {})); Check(evidence->created == attempts && block.front() == 0);
            now += 1ms;
            Pump(capture, block, [&] { return control->Status().health.state == AudioEndpointState::Running; });
            Check(std::all_of(block.begin(), block.end(), [](auto sample) { return sample == 2300; }));
            Check(capture.DroppedFrames() >= dropped + 7 && control->Status().health.failures == 2 && control->Status().revision == 1);
            Check(control->Status().microphoneProcessing == (kind == AudioKind::Microphone));

            // The same factory/selection is retried; Close and stop never reopen it.
            evidence->unavailable = true;
            Pump(capture, block, [&] { return control->Status().health.state == AudioEndpointState::Failed; });
            const auto beforeClose = evidence->created.load(); control->Close(); now += 1h;
            for (int i = 0; i < 5; ++i) Check(capture.Read(block, {}) && block.front() == 0);
            std::stop_source stopped; stopped.request_stop(); Check(!capture.Read(block, stopped.get_token()));
            Check(evidence->created == beforeClose && control->Status().health.state == AudioEndpointState::Inactive);
        }
        Check(evidence->live == 0 && !evidence->wrongThread);
    }
}
inline void OverridesAndTimeout() {
    auto now = std::chrono::steady_clock::time_point{};
    auto evidence = std::make_shared<Evidence>();
    auto factory = [evidence] { return std::make_unique<Capture>(evidence); };
    auto control = std::make_shared<AudioSwitchControl>(AudioSelection{AudioKind::System, L"pinned"}, factory);
    auto capture = std::make_unique<SwitchablePcmCapture>(control, [&] { return now; }); capture->Start();
    PcmBlock block{};
    evidence->unavailable = false; evidence->blockActivation = true; now += 250ms;
    Pump(*capture, block, [&] { return evidence->activating.load(); });
    const auto cancelledAt = std::chrono::steady_clock::now();
    auto muted = control->Submit({AudioKind::None}, {});
    Pump(*capture, block, [&] { return muted.wait_for(0ms) == std::future_status::ready; });
    Check(muted.get().error == AudioUpdateError::None && evidence->live == 0 && evidence->created == 2);
    Check(std::chrono::steady_clock::now() - cancelledAt < 500ms);
    now += 1h;
    for (int i = 0; i < 10; ++i) Check(capture->Read(block, {}) && block.front() == 0);
    Check(evidence->created == 2 && control->Status().health.state == AudioEndpointState::Silent);
    auto replacement = std::make_shared<Evidence>(); replacement->unavailable = false; replacement->marker = 3400;
    auto selected = control->Submit({AudioKind::System, L"replacement"}, [replacement] { return std::make_unique<Capture>(replacement); });
    Pump(*capture, block, [&] { return selected.wait_for(0ms) == std::future_status::ready; });
    Check(selected.get().error == AudioUpdateError::None && block.front() == 3400 &&
        control->Status().selected.deviceId == L"replacement" && control->Status().revision == 3);
    capture.reset(); Check(evidence->live == 0 && replacement->live == 0 && !evidence->wrongThread && !replacement->wrongThread);

    // A recovery producer that starts but supplies no PCM has a bounded lifetime.
    evidence = std::make_shared<Evidence>();
    control = std::make_shared<AudioSwitchControl>(AudioSelection{}, [evidence] { return std::make_unique<Capture>(evidence); });
    capture = std::make_unique<SwitchablePcmCapture>(control, [&] { return now; }); capture->Start();
    evidence->unavailable = false; evidence->blockRead = true; now += 250ms;
    Pump(*capture, block, [&] { return evidence->reading.load(); });
    now += 4999ms; Check(capture->Read(block, {}) && evidence->live == 1 && block.front() == 0);
    now += 1ms; Check(capture->Read(block, {}) && evidence->live == 0 && block.front() == 0);
    evidence->blockRead = false; now += 500ms;
    Pump(*capture, block, [&] { return control->Status().health.state == AudioEndpointState::Running; });
    Check(evidence->created == 3 && block.front() == evidence->marker);
    evidence->unavailable = true;
    Pump(*capture, block, [&] { return control->Status().health.state == AudioEndpointState::Failed; });
    evidence->unavailable = false; // Do not advance the retry clock: manual Retry wins.
    auto retry = control->Submit({}, [evidence] { return std::make_unique<Capture>(evidence); });
    Pump(*capture, block, [&] { return retry.wait_for(0ms) == std::future_status::ready; });
    Check(retry.get().error == AudioUpdateError::None && evidence->created == 4 && control->Status().revision == 2);
    capture.reset(); Check(evidence->live == 0 && !evidence->wrongThread);

    // Close/destruction interrupt an automatic activation without another open.
    evidence = std::make_shared<Evidence>();
    control = std::make_shared<AudioSwitchControl>(AudioSelection{}, [evidence] { return std::make_unique<Capture>(evidence); });
    capture = std::make_unique<SwitchablePcmCapture>(control, [&] { return now; }); capture->Start();
    evidence->blockActivation = true; now += 250ms;
    Pump(*capture, block, [&] { return evidence->activating.load(); });
    control->Close(); now += 1h; Check(capture->Read(block, {}));
    Check(evidence->created == 2 && evidence->live == 0 && control->Status().health.state == AudioEndpointState::Inactive);
    capture.reset(); Check(!evidence->wrongThread);
}
inline void ExcludedSelections() {
    for (const auto selection : {AudioSelection{AudioKind::Process, {}, 123}, AudioSelection{AudioKind::SharedSource}}) {
        auto now = std::chrono::steady_clock::time_point{};
        auto evidence = std::make_shared<Evidence>();
        auto control = std::make_shared<AudioSwitchControl>(selection, [evidence] { return std::make_unique<Capture>(evidence); });
        SwitchablePcmCapture capture(control, [&] { return now; }); capture.Start();
        now += 1h; PcmBlock block{};
        for (int i = 0; i < 5; ++i) Check(capture.Read(block, {}) && block.front() == 0);
        Check(evidence->created == 1 && evidence->live == 0);
    }
}
inline void MissingPcm() {
    for (const auto kind : {AudioKind::System, AudioKind::Microphone}) {
        for (const unsigned stage : {0u, 1u, 2u}) {
            const bool initiallyStalled = stage == 0;
            auto evidence = std::make_shared<Evidence>();
            evidence->unavailable = false; evidence->blockRead = initiallyStalled;
            auto control = std::make_shared<AudioSwitchControl>(AudioSelection{kind, L"pinned-stalled"},
                [evidence] { return std::make_unique<Capture>(evidence); },
                [evidence](std::unique_ptr<PcmCaptureEndpoint> endpoint) { ++evidence->processors; return endpoint; });
            auto now = std::chrono::steady_clock::time_point{};
            {
                SwitchablePcmCapture capture(control, [&] { return now; }); capture.Start();
                PcmBlock block{};
                unsigned attempts = 1;
                uint64_t revision = 1;
                std::wstring deviceId = L"pinned-stalled";
                if (!initiallyStalled) {
                    Pump(capture, block, [&] { return block.front() == evidence->marker; });
                    if (stage == 2) {
                        deviceId = L"selected-stalled";
                        auto selected = control->Submit({kind, deviceId}, [evidence] { return std::make_unique<Capture>(evidence); });
                        Pump(capture, block, [&] { return selected.wait_for(0ms) == std::future_status::ready; });
                        Check(selected.get().error == AudioUpdateError::None);
                        attempts = 2; revision = 2;
                    }
                    evidence->blockRead = true;
                }
                Pump(capture, block, [&] { return evidence->reading.load(); });
                // Consume any blocks produced just before Read stalled. Empty
                // polls must not keep the missing-PCM deadline alive.
                for (int i = 0; i < 8; ++i) Check(capture.Read(block, {}));
                now += 4999ms;
                Check(capture.Read(block, {}) && block.front() == 0 && evidence->live == 1 && evidence->created == attempts);
                now += 1ms; Check(capture.Read(block, {}));
                if (control->Status().health.state != AudioEndpointState::Failed || evidence->live != 0)
                    throw std::runtime_error("Open host audio device stalled indefinitely without PCM recovery");
                Check(block.front() == 0 && capture.DelayMs() == 0 && control->Status().health.failures == 1);
                evidence->blockRead = false;
                // Zero-valued PCM is real audio delivery and must also recover.
                evidence->marker = 0;
                now += 249ms; Check(capture.Read(block, {}) && evidence->created == attempts);
                now += 1ms;
                Pump(capture, block, [&] { return control->Status().health.state == AudioEndpointState::Running; });
                Check(evidence->created == attempts + 1 && block.front() == 0 && control->Status().revision == revision
                    && control->Status().selected.deviceId == deviceId);
                Check(evidence->processors == (kind == AudioKind::Microphone ? attempts + 1 : 0u));

                // A delayed owner must consume newly queued PCM before checking
                // its old deadline. Otherwise scheduler stalls cause false resets.
                const auto produced = evidence->blocks.load();
                const auto waitUntil = std::chrono::steady_clock::now() + 2s;
                while (evidence->blocks < produced + 2) {
                    Check(std::chrono::steady_clock::now() < waitUntil);
                    std::this_thread::sleep_for(1ms);
                }
                now += 1h;
                Check(capture.Read(block, {}) && control->Status().health.state == AudioEndpointState::Running
                    && evidence->created == attempts + 1 && evidence->live == 1);
            }
            Check(evidence->live == 0 && !evidence->wrongThread);
        }
    }
    for (const auto selection : {AudioSelection{AudioKind::Process, {}, 123}, AudioSelection{AudioKind::SharedSource}}) {
        auto evidence = std::make_shared<Evidence>(); evidence->unavailable = false; evidence->blockRead = true;
        auto control = std::make_shared<AudioSwitchControl>(selection, [evidence] { return std::make_unique<Capture>(evidence); });
        auto now = std::chrono::steady_clock::time_point{};
        {
            SwitchablePcmCapture capture(control, [&] { return now; }); capture.Start(); PcmBlock block{};
            Pump(capture, block, [&] { return evidence->reading.load(); });
            // Shared-source owns its inner recovery and explicit process IDs
            // must not acquire a new automatic-reopen policy from this watchdog.
            now += 5s; Check(capture.Read(block, {}) && evidence->live == 1
                && control->Status().health.state == AudioEndpointState::Running);
            now += 1h; Check(capture.Read(block, {}) && evidence->created == 1 && block.front() == 0);
            auto muted = control->Submit({AudioKind::None}, {});
            Pump(capture, block, [&] { return muted.wait_for(0ms) == std::future_status::ready; });
            Check(muted.get().error == AudioUpdateError::None && control->Status().health.state == AudioEndpointState::Silent);
        }
        Check(evidence->live == 0 && !evidence->wrongThread);
    }
}
inline void Run() {
    MissingPcm(); Recovery(); OverridesAndTimeout(); ExcludedSelections();
    std::cout << "Host audio recovery: initial/established/switched PCM stalls, fresh silence, capped retries, pinned targets, microphone processor renewal, mute/change/close priority and activation timeout passed.\n";
}
}
