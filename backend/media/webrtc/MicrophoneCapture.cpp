#include "MicrophoneCapture.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"
#include <stdexcept>

namespace screenshare::media {
namespace {
class MicrophoneCapture final : public PcmCaptureEndpoint {
    std::unique_ptr<PcmCaptureEndpoint> input_;
    webrtc::scoped_refptr<webrtc::AudioProcessing> processor_;
public:
    explicit MicrophoneCapture(std::unique_ptr<PcmCaptureEndpoint> input) : input_(std::move(input)) {
        if (!input_) throw std::invalid_argument("Missing microphone endpoint");
    }
    void Start() override { Start({}); }
    void Start(std::stop_token stop) override {
        webrtc::AudioProcessing::Config config;
        config.high_pass_filter.enabled = true;
        config.noise_suppression.enabled = true;
        config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
        // Digital only; never change the user's Windows microphone level.
        config.gain_controller2.enabled = true;
        config.gain_controller2.adaptive_digital.enabled = true;
        // No synchronized speaker reference in this one-way sharing topology.
        config.echo_canceller.enabled = false;
        processor_ = webrtc::BuiltinAudioProcessingBuilder(config).Build(webrtc::CreateEnvironment());
        if (!processor_ || processor_->Initialize() != 0) throw std::runtime_error("Microphone processing startup failed");
        input_->Start(stop);
    }
    bool Read(PcmBlock& block, std::stop_token stop) override {
        if (!input_->Read(block, stop) || stop.stop_requested()) return false;
        const webrtc::StreamConfig stereo(48000, 2);
        if (!processor_ || processor_->ProcessStream(block.data(), stereo, stereo, block.data()) != 0)
            throw std::runtime_error("Microphone processing failed");
        return true;
    }
    uint32_t DelayMs() const override { return input_->DelayMs(); }
    uint64_t DroppedFrames() const override { return input_->DroppedFrames(); }
};
}
std::unique_ptr<PcmCaptureEndpoint> ProcessMicrophone(std::unique_ptr<PcmCaptureEndpoint> input) {
    return std::make_unique<MicrophoneCapture>(std::move(input));
}
}
