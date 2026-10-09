#include "../tools/webrtc-proof/PublicRoomSessionFixture.h"
#include "media/SignalingExecutor.h"
#include "media/webrtc/SharedAudioCodecs.h"
#include <deque>
#include <mutex>

namespace {
struct Levels {
    std::atomic<unsigned> mode{0};
    std::mutex mutex;
    uint64_t samples = 0;
    double left = 0, right = 0, cross = 0;
    void Reset() { std::lock_guard lock(mutex); samples = 0; left = right = cross = 0; }
};
class Capture final : public PcmCaptureEndpoint {
    std::shared_ptr<Levels> levels_;
    proof::ToneCapture clock_;
    uint64_t position_ = 0;
public:
    explicit Capture(std::shared_ptr<Levels> levels) : levels_(std::move(levels)) {}
    void Start() override { clock_.Start(); }
    bool Read(PcmBlock& block, std::stop_token stop) override {
        if (!clock_.Read(block, stop)) return false;
        const auto mode = levels_->mode.load();
        for (size_t i = 0; i < 480; ++i) {
            const auto sample = int16_t(8000 * std::sin(double(position_++) * 2 * 3.141592653589793 * 1000 / 48000));
            block[2*i] = mode == 2 ? 0 : sample;
            block[2*i+1] = mode == 1 ? 0 : mode == 3 ? -sample : sample;
        }
        return true;
    }
    uint32_t DelayMs() const override { return 0; }
};
class Playout final : public PcmPlayoutEndpoint {
    std::shared_ptr<Levels> levels_;
    std::chrono::steady_clock::time_point next_;
public:
    explicit Playout(std::shared_ptr<Levels> levels) : levels_(std::move(levels)) {}
    void Start() override { next_ = std::chrono::steady_clock::now(); }
    void Write(const PcmBlock& block, std::stop_token) override {
        {
            std::lock_guard lock(levels_->mutex);
            for (size_t i = 0; i < 480; ++i) {
                const double left = block[2*i], right = block[2*i+1];
                levels_->left += left * left; levels_->right += right * right;
                levels_->cross += left * right;
            }
            levels_->samples += 480;
        }
        next_ += 10ms; std::this_thread::sleep_until(next_);
    }
    uint32_t DelayMs() const override { return 0; }
    uint32_t BufferFrames() const override { return 480; }
};
void CodecCompatibility() {
    std::vector<webrtc::AudioCodecSpec> encoders, decoders;
    SharedAudioEncoder::AppendSupportedEncoders(&encoders);
    SharedAudioDecoder::AppendSupportedDecoders(&decoders);
    Check(encoders.size() == 1 && decoders.size() == 1);
    for (const auto& spec : {encoders[0], decoders[0]}) {
        Check(spec.format.parameters.at("stereo") == "1" && spec.format.parameters.at("sprop-stereo") == "1");
        Check(SharedAudioEncoder::SdpToConfig(spec.format)->num_channels == 2);
        Check(SharedAudioDecoder::SdpToConfig(spec.format)->num_channels == 2);
        auto legacy = spec.format; legacy.parameters.erase("stereo"); legacy.parameters.erase("sprop-stereo");
        Check(SharedAudioEncoder::SdpToConfig(legacy)->num_channels == 1);
        Check(SharedAudioDecoder::SdpToConfig(legacy).has_value());
        legacy.parameters["stereo"] = "0";
        Check(SharedAudioEncoder::SdpToConfig(legacy)->num_channels == 1 &&
            SharedAudioDecoder::SdpToConfig(legacy)->num_channels == 1);
    }
}
}

// Exercise the shipped MediaEngine, negotiation, encrypted Opus transport and
// viewer's 100% playback control without opening any physical audio devices.
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE);
    logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
    webrtc::InitializeLogging(std::move(logging));
    webrtc::WinsockInitializer winsock;
    if (winsock.error() || !webrtc::InitializeSSL()) return 1;
    int result = 0;
    try {
        CodecCompatibility();
        SignalingExecutor executor;
        std::unique_ptr<RoomRuntime> host, viewer;
        auto levels = std::make_shared<Levels>();
        auto hostEvidence = std::make_shared<Evidence>(), viewerEvidence = std::make_shared<Evidence>();
        hostEvidence->audioEndpoints = proof::SyntheticAudio(hostEvidence->audio);
        hostEvidence->audioEndpoints->capture = [levels] { return std::make_unique<Capture>(levels); };
        viewerEvidence->audioEndpoints = proof::SyntheticAudio(viewerEvidence->audio);
        viewerEvidence->audioEndpoints->playout = [levels] { return std::make_unique<Playout>(levels); };
        struct Signal { bool toHost; RoomPeerSignal value; };
        std::deque<Signal> pendingSignals;
        unsigned stereoDescriptions = 0;
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
                if (signal.value.kind == RoomPeerSignal::Kind::Offer || signal.value.kind == RoomPeerSignal::Kind::Answer) {
                    Check(signal.value.sdp.find(";stereo=1") != std::string::npos &&
                        signal.value.sdp.find("sprop-stereo=1") != std::string::npos);
                    ++stereoDescriptions;
                }
                Check(signal.toHost ? host->Receive("viewer", std::move(signal.value)) :
                    viewer->Receive("host", std::move(signal.value)));
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
                    [&](const auto&, auto signal) { pendingSignals.push_back({true, std::move(signal)}); return true; }, viewerEvidence);
            });
            wait([&] { return host->Ready("viewer") && viewer->Ready("host"); });
            run([&] { Check(host->Add("viewer") && viewer->Add("host")); });
            wait([&] { return stereoDescriptions == 2 && viewerEvidence->frames >= 10; });
            for (unsigned mode = 0; mode < 4; ++mode) {
                levels->mode = mode;
                const auto settle = std::chrono::steady_clock::now() + 800ms;
                wait([&] { return std::chrono::steady_clock::now() >= settle; });
                levels->Reset();
                wait([&] { std::lock_guard lock(levels->mutex); return levels->samples >= 24000; });
                std::lock_guard lock(levels->mutex);
                const auto left = std::sqrt(levels->left / levels->samples);
                const auto right = std::sqrt(levels->right / levels->samples);
                const auto reference = 8000 / std::sqrt(2.0);
                std::cout << "mode=" << mode << " left_rms=" << left << " right_rms=" << right << " reference=" << reference << '\n';
                auto full = [&](double rms) { Check(rms > reference * 0.85 && rms < reference * 1.15); };
                if (mode != 2) full(left); else Check(left < reference * 0.05);
                if (mode != 1) full(right); else Check(right < reference * 0.05);
                if (mode == 3) Check(levels->cross / std::sqrt(levels->left * levels->right) < -0.95);
            }
            stop();
            std::cout << "Stereo Opus: equal, left-only, right-only and opposite-phase levels preserved at 100% playback.\n";
        } catch (...) { stop(); throw; }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    webrtc::CleanupSSL();
    return result;
}
