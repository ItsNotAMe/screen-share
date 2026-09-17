#pragma once
#include "media/audio/StereoDownmix.h"
#include "media/webrtc/MicrophoneCapture.h"
#include <vector>

namespace audio_processing_test {
inline void Check(bool ok) { if (!ok) throw std::runtime_error("Audio processing/downmix contract failed"); }
inline void Downmix() {
    using screenshare::media::StereoDownmix;
    auto mix = [](unsigned channels, uint32_t mask, std::vector<int16_t> input) {
        StereoDownmix converter(channels, mask);
        std::vector<int16_t> output(input.size() / channels * 2);
        converter.Convert(std::as_bytes(std::span(input)), output); return output;
    };
    Check(mix(1, 0, {-32768, 0, 32767}) == std::vector<int16_t>({-32768, -32768, 0, 0, 32767, 32767}));
    Check(mix(2, 3, {-32768, 32767, 123, -456}) == std::vector<int16_t>({-32768, 32767, 123, -456}));
    // 5.1 back and side layouts, and 7.1: verify each channel independently.
    for (auto [channels, mask] : {std::pair{6u, 0x3fu}, std::pair{6u, 0x60fu}, std::pair{8u, 0x63fu}}) {
        for (unsigned channel = 0; channel < channels; ++channel) {
            std::vector<int16_t> input(channels); input[channel] = 24000;
            auto output = mix(channels, mask, input);
            if (channel == 0) Check(output[0] == (channels == 6 ? 9941 : 7689));
            if (channel == 2) Check(output[0] == (channels == 6 ? 7029 : 5437));
            if (channel == 3) Check(output[0] == 0 && output[1] == 0); // LFE
            else if (channel == 2) Check(output[0] > 0 && output[0] == output[1]); // center
            else if (channel == 0 || channel == 4 || channel == 6) Check(output[0] > 0 && output[1] == 0);
            else Check(output[0] == 0 && output[1] > 0);
        }
        auto maximum = mix(channels, mask, std::vector<int16_t>(channels, 32767));
        auto minimum = mix(channels, mask, std::vector<int16_t>(channels, -32768));
        Check(maximum[0] == 32767 && maximum[1] == 32767 && minimum[0] == -32768 && minimum[1] == -32768);
        Check(mix(channels, mask, std::vector<int16_t>(channels * 480)) == std::vector<int16_t>(960));
    }
    for (auto [channels, mask] : {std::pair{0u, 0u}, std::pair{9u, 0u}, std::pair{6u, 0u},
            std::pair{6u, 3u}, std::pair{2u, 0x8001u}, std::pair{1u, 8u}}) {
        bool rejected = false; try { StereoDownmix invalid(channels, mask); } catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected);
    }
    bool rejected = false;
    try { std::array<std::byte, 3> shortPacket{}; std::array<int16_t, 2> output{}; StereoDownmix(2, 3).Convert(shortPacket, output); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected);
}
class ConstantCapture final : public screenshare::media::PcmCaptureEndpoint {
public:
    void Start() override {}
    bool Read(screenshare::media::PcmBlock& block, std::stop_token stop) override {
        block.fill(4000); return !stop.stop_requested();
    }
    uint32_t DelayMs() const override { return 12; }
    uint64_t DroppedFrames() const override { return 7; }
};
inline void Microphone() {
    using namespace screenshare::media;
    auto microphone = ProcessMicrophone(std::make_unique<ConstantCapture>());
    microphone->Start();
    PcmBlock processed{}, bypass{}; ConstantCapture system;
    for (int i = 0; i < 150; ++i) Check(microphone->Read(processed, {}));
    system.Read(bypass, {});
    int peak = 0; for (auto sample : processed) peak = std::max(peak, std::abs(int(sample)));
    Check(peak < 100 && bypass.front() == 4000 && microphone->DelayMs() == 12 && microphone->DroppedFrames() == 7);
    std::stop_source stop; stop.request_stop(); Check(!microphone->Read(processed, stop.get_token()));
    bool rejected = false; try { ProcessMicrophone({}); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected);
}
}
