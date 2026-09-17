#include "media/webrtc/PcmAudioDeviceModule.h"
#include "media/audio/WasapiPcmEndpoint.h"
#include "SyntheticAudio.h"
#include <condition_variable>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Ownership { std::atomic<unsigned> live{0}, created{0}; std::atomic<bool> wrongThread{false}, unavailable{false}; };
class OwnedCapture final : public screenshare::media::PcmCaptureEndpoint {
    std::shared_ptr<Ownership> ownership_;
    std::thread::id owner_ = std::this_thread::get_id();
    bool stalled_;
    bool stalledStart_;
    proof::ToneCapture tone_;
public:
    OwnedCapture(std::shared_ptr<Ownership> ownership, bool stalled, bool stalledStart = false)
        : ownership_(std::move(ownership)), stalled_(stalled), stalledStart_(stalledStart) { ++ownership_->live; ++ownership_->created; }
    ~OwnedCapture() override { if (owner_ != std::this_thread::get_id()) ownership_->wrongThread = true; --ownership_->live; }
    void Start() override { if (ownership_->unavailable) throw std::runtime_error("Capture unavailable"); tone_.Start(); }
    void Start(std::stop_token stop) override {
        if (stalledStart_) {
            std::mutex mutex; std::condition_variable_any wake; std::unique_lock lock(mutex);
            wake.wait(lock, stop, [] { return false; }); throw std::runtime_error("Activation cancelled");
        }
        Start();
    }
    bool Read(screenshare::media::PcmBlock& block, std::stop_token stop) override {
        if (ownership_->unavailable) throw std::runtime_error("Capture lost");
        if (!stalled_) return tone_.Read(block, stop);
        std::mutex mutex; std::condition_variable_any wake; std::unique_lock lock(mutex);
        wake.wait(lock, stop, [] { return false; }); return false;
    }
    uint32_t DelayMs() const override { return 0; }
};
void Switches() {
    using namespace screenshare::media;
    using namespace std::chrono_literals;
    auto ownership = std::make_shared<Ownership>();
    AudioSwitchControl::Factory good = [ownership] { return std::make_unique<OwnedCapture>(ownership, false); };
    AudioSwitchControl::Factory stalled = [ownership] { return std::make_unique<OwnedCapture>(ownership, true); };
    auto control = std::make_shared<AudioSwitchControl>(AudioSelection{}, good);
    Require(control->Submit({}, good).get().error == AudioUpdateError::Unavailable, "Inactive switch accepted");
    auto capture = std::make_unique<SwitchablePcmCapture>(control); capture->Start();
    PcmBlock block; std::stop_source stop;
    std::this_thread::sleep_for(100ms); capture->Read(block, stop.get_token());
    Require(capture->DroppedFrames() >= 480 && capture->DelayMs() <= 30, "Capture backlog was not bounded");
    auto pump = [&](std::future<AudioUpdateResult>& future) {
        const auto deadline = std::chrono::steady_clock::now() + 6s;
        unsigned nonzero = 0;
        while (future.wait_for(0ms) != std::future_status::ready) {
            Require(std::chrono::steady_clock::now() < deadline && capture->Read(block, stop.get_token()), "Audio handover hung");
            if (std::any_of(block.begin(), block.end(), [](auto value) { return value != 0; })) ++nonzero;
        }
        return nonzero;
    };
    auto failure = control->Submit({}, []() -> std::unique_ptr<PcmCaptureEndpoint> { throw std::runtime_error("Injected failure"); });
    pump(failure); Require(failure.get().error == AudioUpdateError::Failed && control->Status().revision == 1, "Failed source committed");
    auto timeout = control->Submit({}, stalled);
    Require(control->Submit({}, good).get().error == AudioUpdateError::Busy, "Unbounded audio requests");
    Require(pump(timeout) > 100, "Old audio interrupted while candidate stalled");
    Require(timeout.get().error == AudioUpdateError::Timeout && control->Status().revision == 1, "Missing audio did not time out");
    for (int i = 0; i < 3; ++i) {
        auto success = control->Submit({AudioKind::Microphone}, good); pump(success);
        Require(success.get().error == AudioUpdateError::None && control->Status().revision == uint64_t(i + 2), "Audio handover failed");
    }
    auto cancelled = control->Submit({}, stalled); capture->Read(block, stop.get_token());
    const auto beforeStop = std::chrono::steady_clock::now(); capture.reset();
    Require(cancelled.get().error == AudioUpdateError::Cancelled && std::chrono::steady_clock::now() - beforeStop < 300ms, "Audio cancellation hung");
    Require(ownership->live == 0 && !ownership->wrongThread, "Audio endpoint ownership leak");
    // Recording can restart after its last viewer leaves; successful selection persists.
    capture = std::make_unique<SwitchablePcmCapture>(control); capture->Start();
    Require(control->Status().revision == 4 && control->Status().selected.kind == AudioKind::Microphone, "Selection lost on recording restart");
    auto queued = control->Submit({}, good); control->Close();
    Require(queued.get().error == AudioUpdateError::Cancelled, "Queued audio cancellation failed");
    Require(control->Submit({}, good).get().error == AudioUpdateError::Unavailable, "Closed control accepted audio");
    capture.reset(); Require(ownership->live == 0, "Restarted endpoint leaked");
    auto startingControl = std::make_shared<AudioSwitchControl>(AudioSelection{}, good);
    capture = std::make_unique<SwitchablePcmCapture>(startingControl); capture->Start();
    auto starting = startingControl->Submit({}, [ownership] { return std::make_unique<OwnedCapture>(ownership, false, true); });
    capture->Read(block, stop.get_token());
    const auto cancelStart = std::chrono::steady_clock::now(); capture.reset();
    Require(starting.get().error == AudioUpdateError::Cancelled && std::chrono::steady_clock::now() - cancelStart < 300ms && ownership->live == 0,
        "In-flight activation did not cancel promptly");
}
void NoSharedAudio() {
    using namespace screenshare::media;
    using namespace std::chrono_literals;
    auto ownership = std::make_shared<Ownership>();
    AudioSwitchControl::Factory good = [ownership] { return std::make_unique<OwnedCapture>(ownership, false); };
    std::atomic<unsigned> forbiddenCalls{0};
    AudioSwitchControl::Factory forbidden = [&]() -> std::unique_ptr<PcmCaptureEndpoint> {
        ++forbiddenCalls; throw std::runtime_error("None invoked a device factory");
    };
    auto control = std::make_shared<AudioSwitchControl>(AudioSelection{AudioKind::None}, forbidden);
    auto capture = std::make_unique<SwitchablePcmCapture>(control); capture->Start();
    PcmBlock block;
    auto silence = [&] {
        block.fill(123); Require(capture->Read(block, {}), "Silent source stopped");
        Require(std::all_of(block.begin(), block.end(), [](auto sample) { return sample == 0; }), "Silent source leaked audio");
    };
    silence();
    auto pump = [&](std::future<AudioUpdateResult>& result) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (result.wait_for(0ms) != std::future_status::ready) {
            Require(std::chrono::steady_clock::now() < deadline && capture->Read(block, {}), "None handover hung");
        }
    };
    auto failed = control->Submit({}, forbidden); pump(failed);
    Require(failed.get().error == AudioUpdateError::Failed && control->Status().revision == 1, "Failed resume committed");
    silence();
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        auto resume = control->Submit({}, good); pump(resume);
        Require(resume.get().error == AudioUpdateError::None && ownership->live == 1, "Resume did not open capture");
        auto quiet = control->Submit({AudioKind::None}, forbidden); pump(quiet);
        Require(quiet.get().error == AudioUpdateError::None && ownership->live == 0 && !ownership->wrongThread,
            "None retained capture device or destroyed it on the wrong thread");
        silence();
    }
    capture.reset(); capture = std::make_unique<SwitchablePcmCapture>(control); capture->Start(); silence();
    Require(control->Status().selected.kind == AudioKind::None && forbiddenCalls == 1, "None lost on restart or invoked device factory");
    capture.reset();
    // Exercise the production Windows endpoint selector without any audio device.
    screenshare::AudioCaptureConfig config; config.source = screenshare::AudioCaptureSource::None;
    auto endpoint = WasapiPcmEndpoints(config).capture(); endpoint->Start();
    Require(endpoint->Read(block, {}), "Device-free endpoint failed");
    std::this_thread::sleep_for(50ms); // A delayed consumer may read one block, never a catch-up burst.
    endpoint->Read(block, {});
    const auto before = std::chrono::steady_clock::now();
    endpoint->Read(block, {});
    Require(std::chrono::steady_clock::now() - before >= 8ms, "Silent endpoint emitted catch-up burst");
    std::stop_source stopped; stopped.request_stop();
    Require(!endpoint->Read(block, stopped.get_token()), "Silent endpoint ignored cancellation");
    Require(std::all_of(block.begin(), block.end(), [](auto sample) { return sample == 0; }), "Production selector did not silence capture");
}
struct PlaybackEvidence { std::atomic<int> sample{0}, live{0}, created{0}; std::atomic<bool> wrongThread{false}, entered{false}, unavailable{false}; };
class Output final : public screenshare::media::PcmPlayoutEndpoint {
    std::shared_ptr<PlaybackEvidence> evidence_;
    std::thread::id owner_ = std::this_thread::get_id();
    bool fail_, block_, failWrite_;
public:
    Output(std::shared_ptr<PlaybackEvidence> evidence, bool fail = false, bool block = false, bool failWrite = false)
        : evidence_(std::move(evidence)), fail_(fail), block_(block), failWrite_(failWrite) { ++evidence_->live; ++evidence_->created; }
    ~Output() override { if (owner_ != std::this_thread::get_id()) evidence_->wrongThread = true; --evidence_->live; }
    void Start() override { if (fail_ || evidence_->unavailable) throw std::runtime_error("Injected output startup failure"); }
    void Write(const screenshare::media::PcmBlock& block, std::stop_token stop) override {
        if (failWrite_ || evidence_->unavailable) throw std::runtime_error("Injected output write failure");
        if (block_) {
            evidence_->entered = true; std::mutex mutex; std::condition_variable_any wake; std::unique_lock lock(mutex);
            wake.wait(lock, stop, [] { return false; }); return;
        }
        evidence_->sample = block[0];
    }
    uint32_t DelayMs() const override { return 7; }
    uint32_t BufferFrames() const override { return 480; }
    uint32_t EnginePeriodUs() const override { return 10000; }
};
void Playback() {
    using namespace screenshare::media;
    auto evidence = std::make_shared<PlaybackEvidence>();
    PlaybackControl::Factory good = [evidence] { return std::make_unique<Output>(evidence); };
    auto control = std::make_shared<PlaybackControl>(PlaybackSelection{}, good);
    Require(control->Submit({}, good).get().error == AudioUpdateError::Unavailable, "Inactive playback accepted");
    {
        ControlledPcmPlayout output(control); output.Start(); PcmBlock block; block.fill(2000);
        auto gain = control->Submit({L"", 25, false}, good);
        Require(control->Submit({}, good).get().error == AudioUpdateError::Busy, "Playback queue unbounded");
        output.Write(block, {});
        Require(gain.get().error == AudioUpdateError::None && evidence->sample == 500 && evidence->created == 1, "Volume changed endpoint or applied incorrectly");
        auto muted = control->Submit({L"", 25, true}, good); output.Write(block, {});
        Require(muted.get().error == AudioUpdateError::None && evidence->sample == 0 && evidence->created == 1, "Mute did not preserve endpoint");
        auto replacement = control->Submit({L"next", 50, false}, good); output.Write(block, {});
        Require(replacement.get().error == AudioUpdateError::None && evidence->sample == 1000 && evidence->live == 1, "Output handover failed");
        auto failed = control->Submit({L"bad", 100, false}, [evidence] { return std::make_unique<Output>(evidence, true); });
        output.Write(block, {});
        Require(failed.get().error == AudioUpdateError::Failed && evidence->sample == 1000 && control->Status().revision == 4 && evidence->live == 1, "Output rollback failed");
        auto badWrite = control->Submit({L"bad-write", 100, false}, [evidence] { return std::make_unique<Output>(evidence, false, false, true); });
        output.Write(block, {});
        Require(badWrite.get().error == AudioUpdateError::Failed && evidence->sample == 1000 && control->Status().revision == 4 && evidence->live == 1,
            "Failed first write committed replacement");
        Require(output.BufferFrames() == 480 && output.EnginePeriodUs() == 10000 && output.DelayMs() == 7, "Output diagnostics lost");
    }
    Require(evidence->live == 0 && !evidence->wrongThread, "Output ownership leaked");
    {
        ControlledPcmPlayout output(control); output.Start(); PcmBlock block; block.fill(2000); output.Write(block, {});
        Require(evidence->sample == 1000 && control->Status().selected.deviceId == L"next", "Playback settings lost on restart");
        auto pending = control->Submit({}, good); control->Close();
        Require(pending.get().error == AudioUpdateError::Cancelled && control->Submit({}, good).get().error == AudioUpdateError::Unavailable, "Output close ordering failed");
    }
    auto cancelling = std::make_shared<PlaybackControl>(PlaybackSelection{}, good);
    std::promise<void> ready; auto started = ready.get_future();
    std::jthread worker([&](std::stop_token stop) {
        ControlledPcmPlayout output(cancelling); output.Start(); ready.set_value();
        while (!stop.stop_requested()) { PcmBlock block{}; output.Write(block, stop); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    });
    started.get(); auto pending = cancelling->Submit({L"blocked"}, [evidence] { return std::make_unique<Output>(evidence, false, true); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!evidence->entered) { Require(std::chrono::steady_clock::now() < deadline, "Output did not enter blocking write"); std::this_thread::yield(); }
    worker.request_stop(); worker.join();
    Require(pending.get().error == AudioUpdateError::Cancelled && evidence->live == 0 && !evidence->wrongThread, "Output write cancellation failed");
}
void AudioRecovery() {
    using namespace screenshare::media;
    using namespace std::chrono_literals;
    auto ownership = std::make_shared<Ownership>(); ownership->unavailable = true;
    AudioSwitchControl::Factory factory = [ownership] { return std::make_unique<OwnedCapture>(ownership, false); };
    auto control = std::make_shared<AudioSwitchControl>(AudioSelection{}, factory);
    auto capture = std::make_unique<SwitchablePcmCapture>(control); capture->Start();
    PcmBlock block;
    Require(control->Status().health.state == AudioEndpointState::Failed && control->Status().health.failures == 1 && ownership->live == 0,
        "Initial capture failure was hidden or retained its endpoint");
    for (int i = 0; i < 10; ++i) {
        block.fill(2000); Require(capture->Read(block, {}) && std::all_of(block.begin(), block.end(), [](auto sample) { return sample == 0; }),
            "Failed capture did not supply silence");
    }
    Require(ownership->created == 1 && control->Status().revision == 1, "Capture retried without a user command");
    auto pump = [&](std::future<AudioUpdateResult>& result) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (result.wait_for(0ms) != std::future_status::ready)
            Require(std::chrono::steady_clock::now() < deadline && capture->Read(block, {}), "Capture retry hung");
    };
    ownership->unavailable = false;
    auto retry = control->Submit({}, factory); pump(retry);
    Require(retry.get().error == AudioUpdateError::None && control->Status().health.state == AudioEndpointState::Running &&
        ownership->live == 1 && control->Status().health.failures == 1, "Same-source capture retry failed");
    ownership->unavailable = true;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (control->Status().health.state != AudioEndpointState::Failed)
        Require(std::chrono::steady_clock::now() < deadline && capture->Read(block, {}), "Live capture failure stopped the worker");
    Require(control->Status().health.failures == 2 && ownership->live == 0, "Lost capture endpoint was retained");
    retry = control->Submit({}, factory); pump(retry);
    Require(retry.get().error == AudioUpdateError::Failed && control->Status().revision == 2, "Failed capture retry changed selection");
    ownership->unavailable = false; retry = control->Submit({}, factory); pump(retry);
    Require(retry.get().error == AudioUpdateError::None && control->Status().health.failures == 2, "Capture recovery lost failure history");
    control->Close(); capture.reset();
    Require(ownership->live == 0 && !ownership->wrongThread && control->Status().health.state == AudioEndpointState::Inactive, "Capture recovery leaked ownership");

    auto evidence = std::make_shared<PlaybackEvidence>(); evidence->unavailable = true;
    PlaybackControl::Factory outputFactory = [evidence] { return std::make_unique<Output>(evidence); };
    auto playback = std::make_shared<PlaybackControl>(PlaybackSelection{}, outputFactory);
    {
        ControlledPcmPlayout output(playback); output.Start(); block.fill(2000);
        Require(playback->Status().health.state == AudioEndpointState::Failed && evidence->live == 0, "Initial output failure killed recovery or retained device");
        for (int i = 0; i < 10; ++i) output.Write(block, {});
        Require(evidence->created == 1 && evidence->sample == 0 && output.BufferFrames() == 0, "Discard path retried, played or buffered failed output");
        evidence->unavailable = false;
        auto retryOutput = playback->Submit({}, outputFactory); output.Write(block, {});
        Require(retryOutput.get().error == AudioUpdateError::None && evidence->sample == 2000 && evidence->created == 2,
            "Same-device output retry did not reopen endpoint");
        evidence->unavailable = true; output.Write(block, {});
        Require(playback->Status().health.state == AudioEndpointState::Failed && playback->Status().health.failures == 2 && evidence->live == 0,
            "Output failure did not release device and keep worker alive");
        retryOutput = playback->Submit({}, outputFactory); output.Write(block, {});
        Require(retryOutput.get().error == AudioUpdateError::Failed && playback->Status().revision == 2, "Failed output retry committed settings");
        evidence->unavailable = false; retryOutput = playback->Submit({L"", 25, false}, outputFactory); output.Write(block, {});
        Require(retryOutput.get().error == AudioUpdateError::None && evidence->sample == 500 && playback->Status().health.state == AudioEndpointState::Running,
            "Recovered output lost volume or remained failed");
        playback->Close();
    }
    Require(evidence->live == 0 && !evidence->wrongThread && playback->Status().health.state == AudioEndpointState::Inactive, "Output recovery leaked ownership");
}
class Transport final : public webrtc::AudioTransport {
public:
    int32_t RecordedDataIsAvailable(const void* data, size_t frames, size_t bytes, size_t channels,
        uint32_t rate, uint32_t, int32_t, uint32_t, bool, uint32_t& level) override {
        level = 0;
        if (frames != 480 || rate != 48000 || bytes != channels * 2 || (channels != 1 && channels != 2)) { invalid = true; return -1; }
        const auto* pcm = static_cast<const int16_t*>(data);
        if (std::any_of(pcm, pcm + frames * channels, [](int16_t sample) { return std::abs(int(sample)) > 20; })) ++audible;
        else ++silent;
        ++captured; changed.notify_all(); return 0;
    }
    int32_t NeedMorePlayData(size_t frames, size_t bytes, size_t channels, uint32_t rate, void* data,
        size_t& produced, int64_t* elapsed, int64_t* ntp) override {
        produced = 0; *elapsed = *ntp = -1;
        if (frames != 480 || rate != 48000 || bytes != channels * 2) { invalid = true; return -1; }
        auto* pcm = static_cast<int16_t*>(data);
        for (size_t i = 0; i < frames; ++i) {
            const auto value = int16_t(200 * std::sin(double(position_++) * 2 * 3.141592653589793 * 440 / 48000));
            for (size_t c = 0; c < channels; ++c) pcm[i * channels + c] = value;
        }
        produced = frames * channels; ++played; changed.notify_all(); return 0;
    }
    void PullRenderData(int, int, size_t, size_t, void*, int64_t*, int64_t*) override { invalid = true; }
    template<class Predicate> void Wait(Predicate predicate) {
        std::unique_lock lock(mutex);
        Require(changed.wait_for(lock, std::chrono::seconds(4), predicate), "PCM callback timeout");
    }
    std::atomic<unsigned> captured{0}, played{0}, audible{0}, silent{0};
    std::atomic<bool> invalid{false};
    std::mutex mutex;
    std::condition_variable changed;
    uint64_t position_ = 0;
};
void Run(bool wasapi) {
    auto diagnostics = std::make_shared<screenshare::media::PcmAudioDiagnostics>();
    auto evidence = std::make_shared<proof::AudioEvidence>();
    auto endpoints = proof::SyntheticAudio(evidence);
    if (wasapi) {
        screenshare::AudioCaptureConfig config;
        config.source = screenshare::AudioCaptureSource::ProcessOutput;
        config.processId = GetCurrentProcessId(); // Capture only this proof's quiet tone.
        endpoints = screenshare::media::WasapiPcmEndpoints(config);
    }
    auto adm = screenshare::media::CreatePcmAudioDeviceModule(endpoints, diagnostics);
    Transport transport;
    struct Cleanup { webrtc::AudioDeviceModule& adm; ~Cleanup() { adm.Terminate(); } } cleanup{*adm};
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        Require(adm->Init() == 0, "ADM init failed");
        Require(adm->RegisterAudioCallback(&transport) == 0, "Callback registration failed");
        Require(adm->SetStereoRecording(cycle != 1) == 0 && adm->SetStereoPlayout(cycle != 1) == 0, "Stereo selection failed");
        Require(adm->InitPlayout() == 0 && adm->InitRecording() == 0, "PCM initialization failed");
        adm->SetMicrophoneMute(false); adm->SetSpeakerMute(false); adm->SetSpeakerVolume(255);
        const auto heard = transport.audible.load();
        Require(adm->StartPlayout() == 0, "Playout device startup failed");
        Require(adm->StartRecording() == 0, "Capture device startup failed");
        transport.Wait([&] { return transport.audible >= heard + 10; });
        adm->SetMicrophoneMute(true);
        const auto quiet = transport.silent.load();
        transport.Wait([&] { return transport.silent >= quiet + 3; });
        const auto beforeStop = std::chrono::steady_clock::now();
        adm->Terminate(); adm->Terminate();
        Require(std::chrono::steady_clock::now() - beforeStop < std::chrono::milliseconds(300), "PCM stop took too long");
        Require(!adm->Playing() && !adm->Recording() && !adm->Initialized(), "ADM remained active after termination");
        const auto stopped = transport.captured.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Require(transport.captured == stopped, "Callback occurred after termination");
    }
    Require(!transport.invalid && diagnostics->captureErrors == 0 && diagnostics->playoutErrors == 0, "PCM format or endpoint errors");
    Require(diagnostics->playoutBufferFrames > 0, "Actual playout buffer was not reported");
    std::cout << (wasapi ? "WASAPI" : "Synthetic") << " PCM: mono/stereo, capture mute, three lifecycle cycles passed; capture_blocks="
              << diagnostics->capturedBlocks << " output_buffer_frames=" << diagnostics->playoutBufferFrames
              << " output_engine_period_us=" << diagnostics->playoutEnginePeriodUs << '\n';
    // A bad explicit selection fails, and does not substitute another endpoint.
    if (wasapi) {
        screenshare::AudioCaptureConfig invalid;
        invalid.deviceId = L"invalid-device-id-for-proof";
        auto bad = screenshare::media::CreatePcmAudioDeviceModule(
            screenshare::media::WasapiPcmEndpoints(invalid, invalid.deviceId), std::make_shared<screenshare::media::PcmAudioDiagnostics>());
        bad->Init(); bad->InitRecording(); bad->InitPlayout();
        Require(bad->StartRecording() == -1 && bad->StartPlayout() == -1, "Invalid selection silently fell back");
        bad->Terminate();
    }
}
}
int main(int argc, char** argv) {
    try { Switches(); NoSharedAudio(); Playback(); AudioRecovery(); Run(argc == 2 && std::string(argv[1]) == "--wasapi"); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
