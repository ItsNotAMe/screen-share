#pragma once

#include "DesktopCapturer.h"

#include <cstdint>
#include <string>

#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

namespace screenshare {

struct H264EncoderConfig {
    std::wstring outputPath;
    int width = 0;
    int height = 0;
    int fps = 60;
    uint32_t bitrate = 12'000'000;
};

class H264FileEncoder {
public:
    H264FileEncoder();
    ~H264FileEncoder();

    H264FileEncoder(const H264FileEncoder&) = delete;
    H264FileEncoder& operator=(const H264FileEncoder&) = delete;

    void Start(const H264EncoderConfig& config);
    void WriteFrame(const CapturedFrame& frame);
    void Stop();

    [[nodiscard]] bool isRunning() const noexcept { return sinkWriter_ != nullptr; }

private:
    H264EncoderConfig config_{};
    Microsoft::WRL::ComPtr<IMFSinkWriter> sinkWriter_;
    DWORD streamIndex_ = 0;
    int64_t frameIndex_ = 0;
    int64_t frameDuration100ns_ = 0;
    bool comInitialized_ = false;
    bool mfStarted_ = false;
};

std::wstring Widen(const std::string& text);

} // namespace screenshare
