#pragma once

#include "capture/DesktopCapturer.h"

#include <cstddef>
#include <cstdint>
#include <deque>
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

enum class H264StreamEncoderInputMode {
    Memory,
    Direct3D,
};

struct H264StreamEncoderConfig {
    int width = 0;
    int height = 0;
    int fps = 60;
    uint32_t bitrate = 12'000'000;
    uint32_t keyframeIntervalFrames = 0;
    H264StreamEncoderBackend backend = H264StreamEncoderBackend::Software;
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
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
    [[nodiscard]] H264StreamEncoderInputMode lastInputMode() const noexcept { return lastInputMode_; }
    [[nodiscard]] size_t queuedInputCount() const noexcept { return queuedAsyncInputs_.size(); }
    [[nodiscard]] uint64_t droppedInputFrames() const noexcept { return droppedAsyncInputs_; }

private:
    std::vector<EncodedPacket> ReadAvailablePackets();
    std::vector<EncodedPacket> ReadSyncAvailablePackets();
    std::vector<EncodedPacket> ReadAsyncAvailablePackets();
    std::vector<EncodedPacket> PumpAsyncEvents();
    std::vector<EncodedPacket> QueueAsyncInput(Microsoft::WRL::ComPtr<IMFSample> sample);
    std::vector<EncodedPacket> SubmitQueuedAsyncInputs();
    std::vector<EncodedPacket> WaitForAsyncInputRequest();
    std::vector<EncodedPacket> WaitForAsyncQueue();
    std::vector<EncodedPacket> WaitForAsyncDrain();

    H264StreamEncoderConfig config_{};
    H264StreamEncoderBackend backend_ = H264StreamEncoderBackend::Software;
    H264StreamEncoderInputMode lastInputMode_ = H264StreamEncoderInputMode::Memory;
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
    std::deque<Microsoft::WRL::ComPtr<IMFSample>> queuedAsyncInputs_;
    size_t maxQueuedAsyncInputs_ = 8;
    uint64_t droppedAsyncInputs_ = 0;
    bool asyncDrainComplete_ = false;
    bool comInitialized_ = false;
    bool mfStarted_ = false;
};

const char* H264StreamEncoderBackendName(H264StreamEncoderBackend backend);
const char* H264StreamEncoderInputModeName(H264StreamEncoderInputMode inputMode);

} // namespace screenshare
