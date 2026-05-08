#pragma once

#include "capture/DesktopCapturer.h"

#include <cstdint>
#include <string>
#include <vector>

#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

namespace screenshare {

struct EncodedPacket {
    int64_t timestamp100ns = 0;
    int64_t duration100ns = 0;
    std::vector<std::byte> bytes;
};

enum class H264StreamEncoderBackend {
    Software,
    Hardware,
};

struct H264StreamEncoderConfig {
    int width = 0;
    int height = 0;
    int fps = 60;
    uint32_t bitrate = 12'000'000;
    H264StreamEncoderBackend backend = H264StreamEncoderBackend::Software;
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
    [[nodiscard]] H264StreamEncoderBackend backend() const noexcept { return backend_; }
    [[nodiscard]] const std::string& encoderName() const noexcept { return encoderName_; }

private:
    std::vector<EncodedPacket> ReadAvailablePackets();
    std::vector<EncodedPacket> ReadSyncAvailablePackets();
    std::vector<EncodedPacket> ReadAsyncAvailablePackets();
    std::vector<EncodedPacket> PumpAsyncEvents();
    std::vector<EncodedPacket> WaitForAsyncInput();
    std::vector<EncodedPacket> WaitForAsyncDrain();

    H264StreamEncoderConfig config_{};
    H264StreamEncoderBackend backend_ = H264StreamEncoderBackend::Software;
    std::string encoderName_;
    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> eventGenerator_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgiDeviceManager_;
    DWORD inputStreamId_ = 0;
    DWORD outputStreamId_ = 0;
    int64_t frameIndex_ = 0;
    int64_t frameDuration100ns_ = 0;
    uint32_t pendingAsyncInputs_ = 0;
    uint32_t pendingAsyncOutputs_ = 0;
    bool asyncDrainComplete_ = false;
    bool comInitialized_ = false;
    bool mfStarted_ = false;
};

const char* H264StreamEncoderBackendName(H264StreamEncoderBackend backend);

} // namespace screenshare
