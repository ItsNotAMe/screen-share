#include "media/audio/SourcePcmCapture.h"
#include <algorithm>
#include <iostream>
#include <source_location>
#include <vector>
using namespace screenshare::media;
void Check(bool value, std::source_location at = std::source_location::current()) {
    if (!value) throw std::runtime_error("Source audio check failed at " + std::to_string(at.line()));
}
class MarkedCapture final : public PcmCaptureEndpoint {
    int16_t marker_;
    std::function<void()> duringRead_;
    std::function<void()> duringStart_;
    bool* unavailable_;
public:
    MarkedCapture(int16_t marker, std::function<void()> duringRead, std::function<void()> duringStart, bool* unavailable)
        : marker_(marker), duringRead_(std::move(duringRead)), duringStart_(std::move(duringStart)), unavailable_(unavailable) {}
    void Start() override { if (duringStart_) duringStart_(); if (marker_ == 99 || *unavailable_) throw std::runtime_error("Unavailable process loopback"); }
    bool Read(PcmBlock& block, std::stop_token) override {
        if (*unavailable_) throw std::runtime_error("Capture device lost");
        block.fill(marker_); if (duringRead_) duringRead_(); return true;
    }
    uint32_t DelayMs() const override { return 17; }
    uint64_t DroppedFrames() const override { return 1; }
};
int main() {
    try {
        using namespace std::chrono_literals;
        auto now = std::chrono::steady_clock::time_point{};
        AudioSelection selected{AudioKind::Process, {}, 11};
        bool valid = true, healthy = false, unavailable = false;
        unsigned readyCount = 0;
        int16_t markerOverride = 0;
        std::vector<AudioSelection> opened;
        std::function<void()> duringRead, duringStart;
        auto resolve = [&] { if (!valid) throw std::runtime_error("Window closed"); return selected; };
        auto factory = [&](AudioSelection source) {
            opened.push_back(source);
            return std::make_unique<MarkedCapture>(markerOverride ? markerOverride : source.kind == AudioKind::System ? 1 : int16_t(source.processId), duringRead, duringStart, &unavailable);
        };
        SourcePcmCapture capture(resolve, factory, [&](bool ready) { healthy = ready; readyCount += ready; }, [&] { return now; });
        capture.Start();
        PcmBlock block;
        auto read = [&](int16_t marker) {
            Check(capture.Read(block, {}));
            Check(std::all_of(block.begin(), block.end(), [&](auto value) { return value == marker; }));
        };
        read(11); Check(healthy && capture.DelayMs() == 17 && opened.size() == 1);
        read(11); Check(opened.size() == 1);
        // Successful window changes replace the process, never open system audio.
        selected.processId = 22; read(22); Check(opened.size() == 2 && opened.back().kind == AudioKind::Process);
        // An unsupported target remains silent and does not retry every block.
        selected.processId = 99; read(0); Check(!healthy && capture.DelayMs() == 0);
        const auto failedCount = opened.size(); read(0); Check(opened.size() == failedCount && !healthy);
        for (auto delay : {250ms,500ms,1000ms,2000ms,4000ms,5000ms,5000ms}) {
            const auto attempts = opened.size(); now += delay - 1ms; read(0);
            Check(opened.size() == attempts && !healthy);
            now += 1ms; read(0); Check(opened.size() == attempts + 1 && !healthy && opened.back().kind == AudioKind::Process);
        }
        selected.processId = 33; read(33); Check(healthy);
        valid = false; read(0); Check(!healthy && opened.back().kind == AudioKind::Process);
        valid = true; read(33); Check(healthy); // A live window from the same app recovers.
        // Selecting a display explicitly switches to system capture.
        selected = {}; read(1); Check(healthy && opened.back().kind == AudioKind::System);
        // A source change during a device read must not send the old PCM block.
        duringRead = [&] { selected.processId = 55; };
        selected = {AudioKind::Process, {}, 44}; read(0);
        duringRead = {}; read(55); Check(opened.back().processId == 55);
        // Same-target device failures must recover, without cached stale PCM.
        const auto dropsBeforeRecovery = capture.DroppedFrames();
        unavailable = true; read(0); Check(!healthy && capture.DelayMs() == 0 && capture.DroppedFrames() == dropsBeforeRecovery);
        const auto beforeRecovery = opened.size(); unavailable = false; markerOverride = 75;
        now += 249ms; read(0); Check(opened.size() == beforeRecovery);
        now += 1ms; read(75); Check(healthy && opened.size() == beforeRecovery + 1 && opened.back().processId == 55 &&
            capture.DroppedFrames() == dropsBeforeRecovery + 1);
        markerOverride = 0;
        unavailable = true; read(0); unavailable = false;
        now += 250ms; read(55); Check(healthy); // Success resets the capped backoff.
        // A partially filled block from an interrupted endpoint must be silent.
        duringRead = [] { throw std::runtime_error("Interrupted read"); };
        selected.processId = 77; read(0); Check(!healthy);
        duringRead = {}; now += 250ms; read(77); Check(healthy);
        // Recreating the endpoint (e.g. unmute) resolves the current source.
        selected.processId = 66;
        SourcePcmCapture resumed(resolve, factory, [&](bool ready) { healthy = ready; });
        resumed.Start(); Check(resumed.Read(block, {}) && block[0] == 66);
        // Cancellation during activation must not publish readiness or audio.
        std::stop_source activating; selected.processId = 88;
        duringStart = [&] { activating.request_stop(); };
        const auto beforeReady = readyCount;
        Check(!capture.Read(block, activating.get_token()) && readyCount == beforeReady);
        duringStart = {};
        std::stop_source stopping; stopping.request_stop();
        const auto beforeStop = opened.size(); Check(!capture.Read(block, stopping.get_token()) && opened.size() == beforeStop);
        ValidateAudioSelection({AudioKind::SharedSource});
        for (auto invalid : {AudioSelection{AudioKind::SharedSource, L"device"}, AudioSelection{AudioKind::SharedSource, {}, 12}}) {
            bool rejected = false; try { ValidateAudioSelection(invalid); } catch (const std::invalid_argument&) { rejected = true; } Check(rejected);
        }
        std::cout << "Shared-source audio selection, capped automatic recovery, isolation, fresh PCM, transitions and cancellation passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
