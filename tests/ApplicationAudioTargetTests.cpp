#include "media/audio/ApplicationAudioTarget.h"
#include "media/audio/WindowsApplicationAudioTarget.h"
#include "media/audio/SourcePcmCapture.h"
#include "media/audio/WasapiPcmEndpoint.h"
#include <iostream>
#include <source_location>
#include <cmath>

using namespace screenshare::media;
void Check(bool value, std::source_location at = std::source_location::current()) {
    if (!value) throw std::runtime_error("Application audio target check failed at " + std::to_string(at.line()));
}
void Policy() {
    std::vector<AudioProcessIdentity> processes{{10, 1, 100}, {20, 10, 200}, {30, 20, 300},
        {40, 20, 400}, {50, 10, 500}, {60, 1, 600}, {70, 60, 700}, {80, 10, 50}};
    auto target = [&](std::initializer_list<uint32_t> renderers) {
        return ApplicationAudioTarget(10, processes, {renderers.begin(), renderers.size()});
    };
    Check(target({}) == 10); // Playback may begin after sharing starts.
    Check(target({10}) == 10); // Ordinary single-process app.
    Check(target({30}) == 30); // WebView2 audio service, not its silent host.
    Check(target({30, 40}) == 20); // Multiple renderers use their common browser.
    Check(target({30, 50}) == 10); // Separate native helper branches retain app scope.
    Check(target({30, 30, 60, 70}) == 30); // Other apps and duplicate sessions excluded.
    Check(target({80, 60}) == 10); // Child predating root indicates parent PID reuse.
    processes.erase(processes.begin() + 1); // Missing/exited browser breaks ancestry.
    Check(target({30, 40}) == 10);
    processes.push_back({20, 10, 900}); // Recycled browser PID does not adopt old children.
    Check(target({30}) == 10);
    processes.push_back({90, 20, 1000}); // New helper following a browser restart.
    Check(target({90}) == 90);
    Check(ApplicationAudioFamily(999, processes).empty());
}
void Transitions() {
    class MarkedCapture final : public PcmCaptureEndpoint {
        int16_t marker_;
        std::function<void()> change_;
    public:
        MarkedCapture(uint32_t id, std::function<void()> change) : marker_(int16_t(id)), change_(std::move(change)) {}
        void Start() override {}
        bool Read(PcmBlock& block, std::stop_token) override { block.fill(marker_); if (change_) change_(); return true; }
        uint32_t DelayMs() const override { return 0; }
    };
    std::vector<AudioProcessIdentity> processes{{10, 1, 100}, {20, 10, 200}, {30, 20, 300}};
    std::vector<uint32_t> renderers, opened;
    std::function<void()> change;
    bool live = true, healthy = false;
    SourcePcmCapture capture([&] {
        if (!live) throw std::runtime_error("Window owner exited");
        return AudioSelection{AudioKind::Process, {}, ApplicationAudioTarget(10, processes, renderers)};
    }, [&](AudioSelection source) {
        Check(source.kind == AudioKind::Process); opened.push_back(source.processId);
        return std::make_unique<MarkedCapture>(source.processId, change);
    }, [&](bool ready) { healthy = ready; });
    capture.Start();
    PcmBlock block;
    auto read = [&](int16_t marker) {
        Check(capture.Read(block, {}));
        Check(std::all_of(block.begin(), block.end(), [=](auto sample) { return sample == marker; }));
    };
    read(10); // Sharing begins before playback, with the same window throughout.
    renderers = {30}; read(30); Check(healthy && opened.back() == 30);
    read(30); Check(opened.size() == 2); // No reopening on every audio block.
    change = [&] { processes.push_back({40, 20, 400}); renderers = {40}; };
    renderers.clear(); read(0); // Helper restarts during a read: discard old PCM.
    change = {}; read(40); Check(healthy && opened.back() == 40);
    live = false; read(0); Check(!healthy); // Never replace a vanished app with system audio.
}
void LiveApp(uint32_t process) {
    auto resolver = std::make_shared<WindowsApplicationAudioTarget>(process);
    const auto initial = resolver->Resolve();
    bool healthy = false;
    SourcePcmCapture capture([resolver] { return AudioSelection{AudioKind::Process, {}, resolver->Resolve()}; },
        [](AudioSelection source) {
            screenshare::AudioCaptureConfig config;
            config.source = screenshare::AudioCaptureSource::ProcessOutput;
            config.processId = source.processId;
            return WasapiPcmEndpoints(config).capture();
        }, [&](bool ready) { healthy = ready; });
    capture.Start();
    PcmBlock block{};
    double peak = 0, squares = 0;
    uint64_t blocks = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < until) {
        Check(capture.Read(block, {})); Check(healthy);
        ++blocks;
        for (const auto value : block) {
            peak = std::max(peak, std::abs(double(value)) / 32768);
            squares += double(value) * value;
        }
    }
    std::cout << "App-only capture: owner=" << process << " audio_target=" << initial
        << " blocks=" << blocks << " peak=" << peak << " rms=" << std::sqrt(squares / (blocks * block.size())) / 32768
        << "; no audio saved or replayed\n";
    Check(peak > 0.001); // Optional check requires the selected app to be playing.
}
int main(int argc, char** argv) {
    try {
        Policy(); Transitions();
        if (argc == 3 && std::string_view(argv[1]) == "--app") LiveApp(std::stoul(argv[2]));
        else Check(argc == 1);
        std::cout << "Application audio targeting and process isolation passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
