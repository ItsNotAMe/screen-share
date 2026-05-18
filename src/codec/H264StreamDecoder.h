#pragma once

#include "codec/H264StreamEncoder.h"

#include <cstdint>
#include <vector>

#include <mftransform.h>
#include <wrl/client.h>

namespace screenshare {

struct DecodedFrameInfo {
    int width = 0;
    int height = 0;
    int codedWidth = 0;
    int codedHeight = 0;
    int64_t timestamp100ns = 0;
    int64_t duration100ns = 0;
    uint32_t bytes = 0;
    std::vector<std::byte> data;
};

class H264StreamDecoder {
public:
    H264StreamDecoder();
    ~H264StreamDecoder();

    H264StreamDecoder(const H264StreamDecoder&) = delete;
    H264StreamDecoder& operator=(const H264StreamDecoder&) = delete;

    void Start();
    std::vector<DecodedFrameInfo> DecodePacket(const EncodedPacket& packet);
    std::vector<DecodedFrameInfo> Drain();
    void Stop();

    [[nodiscard]] bool isRunning() const noexcept { return transform_ != nullptr; }

private:
    bool TryConfigureOutputType();
    std::vector<DecodedFrameInfo> ReadAvailableFrames();

    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    DWORD inputStreamId_ = 0;
    DWORD outputStreamId_ = 0;
    int outputWidth_ = 0;
    int outputHeight_ = 0;
    int outputVisibleLeft_ = 0;
    int outputVisibleTop_ = 0;
    int outputVisibleWidth_ = 0;
    int outputVisibleHeight_ = 0;
    DWORD outputBufferBytes_ = 0;
    bool outputTypeConfigured_ = false;
    bool comInitialized_ = false;
    bool mfStarted_ = false;
};

} // namespace screenshare
