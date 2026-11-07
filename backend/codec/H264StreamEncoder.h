#pragma once
#include <memory>

#include "capture/DesktopCapturer.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <span>
#include <vector>

#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

namespace screenshare {

struct EncodedPacket {
    int64_t timestamp100ns = 0;
    int64_t duration100ns = 0;
    int64_t senderQpc100ns = 0;
    bool isKeyframe = false;
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
    // H.264 level_idc; zero retains automatic level selection for legacy callers.
    uint32_t levelIdc = 0;
    // Software slice workers; zero preserves Media Foundation's automatic count.
    uint32_t softwareWorkerThreads = 0;
    // The caller owns readiness, submission bounds and output deadlines.
    bool externalHardwareScheduling = false;
    int64_t startFrameIndex = 0;
    H264StreamEncoderBackend backend = H264StreamEncoderBackend::Software;
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
};

// Borrowed, potentially padded planes. EncodeNv12 copies them into an owned MF
// sample before returning; the caller retains the source for the call only.
struct Nv12FrameView {
    int width = 0, height = 0;
    std::span<const uint8_t> y, uv;
    int strideY = 0, strideUv = 0;
};

class H264StreamEncoder {
public:
    H264StreamEncoder();
    ~H264StreamEncoder();

    H264StreamEncoder(const H264StreamEncoder&) = delete;
    H264StreamEncoder& operator=(const H264StreamEncoder&) = delete;

    void Start(const H264StreamEncoderConfig& config);
    std::vector<EncodedPacket> EncodeFrame(const CapturedFrame& frame);
    std::vector<EncodedPacket> EncodeNv12(const Nv12FrameView& frame);
    std::vector<EncodedPacket> Drain();
    bool TryUpdateBitrate(uint32_t bitrate);
    bool RequestKeyframe();
    std::vector<EncodedPacket> PollHardwareOutput();
    bool HardwareAcceptsInput() const noexcept;
    bool TrySubmitHardwareFrame(const CapturedFrame& frame, int64_t timestamp100ns);
    void Stop();

    [[nodiscard]] bool isRunning() const noexcept { return transform_ != nullptr; }
    [[nodiscard]] H264StreamEncoderBackend backend() const noexcept { return backend_; }
    [[nodiscard]] uint32_t bitrate() const noexcept { return config_.bitrate; }
    [[nodiscard]] const std::string& encoderName() const noexcept { return encoderName_; }
    [[nodiscard]] H264StreamEncoderInputMode lastInputMode() const noexcept { return lastInputMode_; }
    // EncodeFrame completes one submission; no internal raw-frame queue remains.
    [[nodiscard]] size_t queuedInputCount() const noexcept { return 0; }
    [[nodiscard]] uint64_t droppedInputFrames() const noexcept { return 0; }
    // Requested codec property, queried on the codec owner. Zero means automatic
    // selection or an implementation that does not expose this optional setting.
    [[nodiscard]] uint32_t softwareWorkerThreads() const;

private:
    std::vector<EncodedPacket> EncodeSoftwareSample(IMFSample* sample);
    std::vector<EncodedPacket> ReadAvailablePackets();
    std::vector<EncodedPacket> ReadSyncAvailablePackets();
    std::vector<EncodedPacket> ReadAsyncAvailablePackets();
    std::vector<EncodedPacket> PumpAsyncEvents();
    std::vector<EncodedPacket> WaitForAsyncInputRequest();
    std::vector<EncodedPacket> WaitForAsyncDrain();
    void AttachSenderQpc(EncodedPacket& packet);

    H264StreamEncoderConfig config_{};
    H264StreamEncoderBackend backend_ = H264StreamEncoderBackend::Software;
    H264StreamEncoderInputMode lastInputMode_ = H264StreamEncoderInputMode::Memory;
    std::string encoderName_;
    // Activation shutdown is distinct from releasing the transform interfaces.
    std::shared_ptr<IMFActivate> activation_;
    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> eventGenerator_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgiDeviceManager_;
    DWORD inputStreamId_ = 0;
    DWORD outputStreamId_ = 0;
    int64_t frameIndex_ = 0;
    int64_t frameDuration100ns_ = 0;
    uint32_t pendingAsyncInputs_ = 0;
    uint32_t pendingAsyncOutputs_ = 0;
    std::map<int64_t, int64_t> senderQpcBySampleTime_;
    bool asyncDrainComplete_ = false;
    bool comInitialized_ = false;
    bool mfStarted_ = false;
};

const char* H264StreamEncoderBackendName(H264StreamEncoderBackend backend);
const char* H264StreamEncoderInputModeName(H264StreamEncoderInputMode inputMode);

} // namespace screenshare
