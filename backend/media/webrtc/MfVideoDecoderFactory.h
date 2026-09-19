#pragma once

// Internal WebRTC integration header; not part of the portable session API.
#include "api/video_codecs/video_decoder_factory.h"
#include <functional>
#include <atomic>

namespace screenshare::media {
class D3dVideoDevice;
class MfVideoDecoderFactory final : public webrtc::VideoDecoderFactory {
public:
    using DeviceFactory = std::function<std::shared_ptr<D3dVideoDevice>()>;
    explicit MfVideoDecoderFactory(bool preferHardware = false, DeviceFactory deviceFactory = {})
        : preferHardware_(preferHardware), deviceFactory_(std::move(deviceFactory)) {}
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;
    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat&, bool,
        std::optional<webrtc::Resolution>) const override;
    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment&,
        const webrtc::SdpVideoFormat&) override;
private:
    bool preferHardware_;
    DeviceFactory deviceFactory_;
    std::shared_ptr<std::atomic<bool>> hardwareQuarantined_ = std::make_shared<std::atomic<bool>>(false);
};
} // namespace screenshare::media
