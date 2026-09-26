#pragma once
#include "api/video_codecs/video_encoder_factory.h"
#include "media/DiagnosticHistory.h"

namespace screenshare::media {
struct MfHardwareSession;
class MfVideoEncoderFactory final : public webrtc::VideoEncoderFactory {
public:
    explicit MfVideoEncoderFactory(std::shared_ptr<MfHardwareSession> hardware = {}, std::shared_ptr<DiagnosticHistory> diagnostics = {})
        : hardware_(std::move(hardware)), diagnostics_(std::move(diagnostics)) {}
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;
    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat&,
        std::optional<std::string>, std::optional<webrtc::Resolution>) const override;
    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment&,
        const webrtc::SdpVideoFormat&) override;
private:
    std::shared_ptr<MfHardwareSession> hardware_;
    std::shared_ptr<DiagnosticHistory> diagnostics_;
};
}
