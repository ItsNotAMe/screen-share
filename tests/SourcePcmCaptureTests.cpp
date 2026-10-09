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
public:
    MarkedCapture(int16_t marker, std::function<void()> duringRead) : marker_(marker), duringRead_(std::move(duringRead)) {}
    void Start() override { if (marker_ == 99) throw std::runtime_error("Unavailable process loopback"); }
    bool Read(PcmBlock& block, std::stop_token) override { block.fill(marker_); if (duringRead_) duringRead_(); return true; }
    uint32_t DelayMs() const override { return 17; }
};
int main() {
    try {
        AudioSelection selected{AudioKind::Process, {}, 11};
        bool valid = true, healthy = false;
        std::vector<AudioSelection> opened;
        std::function<void()> duringRead;
        auto resolve = [&] { if (!valid) throw std::runtime_error("Window closed"); return selected; };
        auto factory = [&](AudioSelection source) {
            opened.push_back(source);
            return std::make_unique<MarkedCapture>(source.kind == AudioKind::System ? 1 : int16_t(source.processId), duringRead);
        };
        SourcePcmCapture capture(resolve, factory, [&](bool ready) { healthy = ready; });
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
        selected.processId = 33; read(33); Check(healthy);
        valid = false; read(0); Check(!healthy && opened.back().kind == AudioKind::Process);
        valid = true; read(33); Check(healthy); // A live window from the same app recovers.
        // Selecting a display explicitly switches to system capture.
        selected = {}; read(1); Check(healthy && opened.back().kind == AudioKind::System);
        // A source change during a device read must not send the old PCM block.
        duringRead = [&] { selected.processId = 55; };
        selected = {AudioKind::Process, {}, 44}; read(0);
        duringRead = {}; read(55); Check(opened.back().processId == 55);
        // Recreating the endpoint (e.g. unmute) resolves the current source.
        selected.processId = 66;
        SourcePcmCapture resumed(resolve, factory, [&](bool ready) { healthy = ready; });
        resumed.Start(); Check(resumed.Read(block, {}) && block[0] == 66);
        std::stop_source stopping; stopping.request_stop();
        const auto beforeStop = opened.size(); Check(!capture.Read(block, stopping.get_token()) && opened.size() == beforeStop);
        ValidateAudioSelection({AudioKind::SharedSource});
        for (auto invalid : {AudioSelection{AudioKind::SharedSource, L"device"}, AudioSelection{AudioKind::SharedSource, {}, 12}}) {
            bool rejected = false; try { ValidateAudioSelection(invalid); } catch (const std::invalid_argument&) { rejected = true; } Check(rejected);
        }
        std::cout << "Shared-source audio selection, isolation, failure, transitions and cancellation passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
