#include "../tools/webrtc-proof/PublicRoomSessionFixture.h"
#include "media/audio/PcmBlockPacer.h"
#include "media/SignalingExecutor.h"
#include <deque>

namespace {
struct OutputState { std::atomic<unsigned> stallMs{0}, stalls{0}; };
class Output final : public PcmPlayoutEndpoint {
    std::shared_ptr<OutputState> state_;
    PcmBlockPacer pacer_;
public:
    explicit Output(std::shared_ptr<OutputState> state) : state_(std::move(state)) {}
    void Start() override { pacer_.Start(); }
    void Write(const PcmBlock&, std::stop_token stop) override {
        if (const auto ms = state_->stallMs.exchange(0)) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(1ms);
            pacer_.Start(); ++state_->stalls;
        }
        pacer_.Wait(stop);
    }
    uint32_t DelayMs() const override { return 0; }
    uint32_t BufferFrames() const override { return 480; }
};
struct Video final : webrtc::VideoSinkInterface<webrtc::VideoFrame> {
    std::atomic<int64_t> ntp{0};
    std::atomic<unsigned> frames{0};
    void OnFrame(const webrtc::VideoFrame& frame) override { ntp = frame.ntp_time_ms(); ++frames; }
};
}

// Real production room runtime, H264/Opus and encrypted loopback transport.
// Inject only a stalled output device; no physical audio or input is used.
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE);
    logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
    webrtc::InitializeLogging(std::move(logging));
    webrtc::WinsockInitializer winsock;
    if (winsock.error() || !webrtc::InitializeSSL()) return 1;
    int result = 0;
    try {
        SignalingExecutor executor;
        std::unique_ptr<RoomRuntime> host, viewer;
        auto output = std::make_shared<OutputState>();
        auto video = std::make_shared<Video>();
        auto hostEvidence = std::make_shared<Evidence>(), viewerEvidence = std::make_shared<Evidence>();
        viewerEvidence->audioDiagnostics->events = std::make_shared<DiagnosticHistory>();
        StreamPreferences preferences;
        preferences.preset = StreamPreset::Quality;
        preferences.resolution = ResolutionMode::Fixed;
        preferences.width = 640; preferences.height = 360; preferences.fps = 30;
        hostEvidence->preferences = preferences;
        viewerEvidence->audioEndpoints = proof::SyntheticAudio(viewerEvidence->audio);
        viewerEvidence->audioEndpoints->playout = [output] { return std::make_unique<Output>(output); };
        struct Signal { bool toHost; RoomPeerSignal value; };
        std::deque<Signal> pendingSignals;
        auto run = [&](auto work) {
            std::exception_ptr failure;
            auto task = executor.Post([&] { try { work(); } catch (...) { failure = std::current_exception(); } });
            Check(Get(task).error == ExecutorError::None);
            if (failure) std::rethrow_exception(failure);
        };
        auto tick = [&] {
            host->Advance(); viewer->Advance();
            while (!pendingSignals.empty()) {
                auto signal = std::move(pendingSignals.front()); pendingSignals.pop_front();
                Check(signal.toHost ? host->Receive("viewer", std::move(signal.value)) : viewer->Receive("host", std::move(signal.value)));
            }
        };
        auto wait = [&](auto predicate) {
            Wait([&] { bool done = false; run([&] { tick(); done = predicate(); }); return done; });
        };
        auto stop = [&] {
            std::vector<std::shared_future<void>> stops;
            run([&] { for (auto* peer : {host.get(), viewer.get()}) if (peer) stops.push_back(peer->BeginStop()); });
            Wait([&] {
                run([&] { for (auto* peer : {host.get(), viewer.get()}) if (peer) peer->Advance(); });
                return std::all_of(stops.begin(), stops.end(), [](auto& future) { return future.wait_for(0ms) == std::future_status::ready; });
            });
            for (auto& future : stops) future.get();
            run([&] { viewer.reset(); host.reset(); });
        };
        try {
            run([&] {
                host = std::make_unique<Runtime>(RoomIdentity{true, "test", "host"},
                    [&](const auto&, auto signal) { pendingSignals.push_back({false, std::move(signal)}); return true; }, hostEvidence);
                viewer = std::make_unique<Runtime>(RoomIdentity{false, "test", "viewer"},
                    [&](const auto&, auto signal) { pendingSignals.push_back({true, std::move(signal)}); return true; }, viewerEvidence, video);
            });
            wait([&] { return host->Ready("viewer") && viewer->Ready("host"); });
            run([&] { Check(host->Add("viewer") && viewer->Add("host")); });
            wait([&] { return video->frames > 150 && video->ntp > 0 && viewerEvidence->audioDiagnostics->lastPlayoutNtpMs > 0; });
            const auto skew = [&] { return viewerEvidence->audioDiagnostics->lastPlayoutNtpMs.load() - video->ntp.load(); };
            auto averageSkew = [&] {
                int64_t sum = 0;
                unsigned samples = 0, last = video->frames.load();
                wait([&] {
                    const auto frames = video->frames.load();
                    if (frames != last) { last = frames; sum += skew(); ++samples; }
                    return samples == 10;
                });
                return sum / int64_t(samples);
            };
            for (const auto preset : {StreamPreset::Quality, StreamPreset::Gaming}) {
                preferences.preset = preset;
                run([&] { Check(host->UpdateStreamPreferences(preferences).error == StreamUpdateError::None); });
                const auto settle = std::chrono::steady_clock::now() + 2s;
                wait([&] { return std::chrono::steady_clock::now() >= settle; });
                const auto baseline = averageSkew();
                std::cout << "Preset: " << (preset == StreamPreset::Quality ? "quality" : "gaming") << std::endl;
                std::cout << "Baseline audio-minus-video ms: " << baseline << std::endl;
                for (const auto stall : {600u, 400u, 1200u}) {
                    const auto count = output->stalls.load();
                    output->stallMs = stall;
                    wait([&] {
                        const auto timing = viewerEvidence->audioDiagnostics->playoutTiming.Read();
                        return timing.stage == AudioPlayoutStage::DeviceWrite && timing.ageUs >= 100000;
                    });
                    wait([&] { return output->stalls > count; });
                    const auto deadline = std::chrono::steady_clock::now() + 2500ms;
                    const auto frames = video->frames.load();
                    wait([&] { return std::chrono::steady_clock::now() >= deadline; });
                    const auto after = averageSkew();
                    std::cout << "Stall ms: " << stall << " recovered skew ms: " << after << " frames: " << video->frames - frames
                        << " max PCM callback gap ms: " << viewerEvidence->audioDiagnostics->maxPlayoutGapMs << std::endl;
                    run([&] {
                        const auto connections = viewer->StreamSettings().connections;
                        if (connections.empty()) return;
                        const auto records = connections.front().transportHistory.records;
                        if (!records || records->size() < 2) return;
                        const auto& a = (*records)[records->size() - 2].numbers;
                        const auto& b = records->back().numbers;
                        for (const auto* kind : {"audio", "video"}) {
                            const auto prefix = std::string(kind) + "Receive";
                            const auto count = b.at(prefix + "JitterBufferEmitted") - a.at(prefix + "JitterBufferEmitted");
                            if (count <= 0) continue;
                            for (const auto* metric : {"JitterBufferSeconds", "JitterBufferTargetSeconds", "JitterBufferMinimumSeconds"}) {
                                if (b.contains(prefix + metric) && a.contains(prefix + metric))
                                    std::cout << kind << ' ' << metric << '=' << 1000 * (b.at(prefix + metric) - a.at(prefix + metric)) / count << ' ';
                            }
                        }
                        std::cout << std::endl;
                    });
                    Check(std::abs(after - baseline) <= 120 && std::abs(after) <= 150);
                    Check(video->frames >= frames + 25);
                    Check(host->FailedPeers().empty() && viewer->FailedPeers().empty());
                    Check(viewerEvidence->audioDiagnostics->maxPlayoutWriteUs >= uint64_t(stall) * 1000);
                }
            }
            const auto events = viewerEvidence->audioDiagnostics->events->Read().records;
            Check(std::any_of(events->begin(), events->end(), [](const auto& event) {
                return event.labels.at("event") == "audio-playout-stall" && event.numbers.at("writeUs") >= 400000;
            }));
            stop();
            Check(viewerEvidence->audioDiagnostics->playoutTiming.Read().stage == AudioPlayoutStage::Stopped);
        } catch (...) { stop(); throw; }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    webrtc::CleanupSSL();
    return result;
}
