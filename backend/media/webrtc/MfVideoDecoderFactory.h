#pragma once

// Internal WebRTC integration header; not part of the portable session API.
#include "api/video_codecs/video_decoder_factory.h"

namespace screenshare::media {
class MfVideoDecoderFactory final : public webrtc::VideoDecoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;
    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat&, bool,
        std::optional<webrtc::Resolution>) const override;
    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment&,
        const webrtc::SdpVideoFormat&) override;
};
} // namespace screenshare::media
