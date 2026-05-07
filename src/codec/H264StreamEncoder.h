#pragma once

#include "capture/DesktopCapturer.h"

#include <cstdint>
#include <vector>

#include <mftransform.h>
#include <wrl/client.h>

namespace screenshare {

struct EncodedPacket {
    int64_t timestamp100ns = 0;
    int64_t duration100ns = 0;
    std::vector<std::byte> bytes;
};

struct H264StreamEncoderConfig {
    int width = 0;
    int height = 0;
    int fps = 60;
    uint32_t bitrate = 12'000'000;
};

class H264StreamEncoder {
public:
    H264StreamEncoder();
    ~H264StreamEncoder();

    H264StreamEncoder(const H264StreamEncoder&) = delete;
    H264StreamEncoder& operator=(const H264StreamEncoder&) = delete;

    void Start(const H264StreamEncoderConfig& config);
    std::vector<EncodedPacket> EncodeFrame(const CapturedFrame& frame);
    std::vector<EncodedPacket> Drain();
    void Stop();

    [[nodiscard]] bool isRunning() const noexcept { return transform_ != nullptr; }

private:
    std::vector<std::byte> ConvertBgraToNv12(const CapturedFrame& frame) const;
    std::vector<EncodedPacket> ReadAvailablePackets();

    H264StreamEncoderConfig config_{};
    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    DWORD inputStreamId_ = 0;
    DWORD outputStreamId_ = 0;
    int64_t frameIndex_ = 0;
    int64_t frameDuration100ns_ = 0;
    bool comInitialized_ = false;
    bool mfStarted_ = false;
};

} // namespace screenshare
