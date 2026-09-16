#include "media/webrtc/PcmAudioDeviceModule.h"
#include "media/audio/WasapiPcmEndpoint.h"
#include "SyntheticAudio.h"
#include <condition_variable>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Ownership { std::atomic<unsigned> live{0}; std::atomic<bool> wrongThread{false}; };
class OwnedCapture final : public screenshare::media::PcmCaptureEndpoint {
    std::shared_ptr<Ownership> ownership_;
    std::thread::id owner_ = std::this_thread::get_id();
    bool stalled_;
    bool stalledStart_;
    proof::ToneCapture tone_;
public:
    OwnedCapture(std::shared_ptr<Ownership> ownership, bool stalled, bool stalledStart = false)
        : ownership_(std::move(ownership)), stalled_(stalled), stalledStart_(stalledStart) { ++ownership_->live; }
    ~OwnedCapture() override { if (owner_ != std::this_thread::get_id()) ownership_->wrongThread = true; --ownership_->live; }
    void Start() override { tone_.Start(); }
    void Start(std::stop_token stop) override {
        if (stalledStart_) {
            std::mutex mutex; std::condition_variable_any wake; std::unique_lock lock(mutex);
            wake.wait(lock, stop, [] { return false; }); throw std::runtime_error("Activation cancelled");
        }
        Start();
    }
    bool Read(screenshare::media::PcmBlock& block, std::stop_token stop) override {
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
    try { Switches(); Run(argc == 2 && std::string(argv[1]) == "--wasapi"); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
