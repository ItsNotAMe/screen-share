#pragma once
#include "media/webrtc/D3dVideoFrameBuffer.h"
#include "codec/H264StreamEncoder.h"
#include <atomic>
#include <memory>

namespace screenshare::media {
// One object per host session/selected device. Existing healthy encoders continue;
// a failed implementation is not attempted by new or reset encoders this session.
struct MfHardwareSession {
    explicit MfHardwareSession(std::shared_ptr<D3dVideoDevice> value) : device(std::move(value)) {}
    virtual ~MfHardwareSession() = default;
    // Event-source boundary also permits deterministic missing-output tests.
    virtual std::vector<EncodedPacket> PollOutput(H264StreamEncoder& encoder) { return encoder.PollHardwareOutput(); }
    // Capture recovery calls this before publishing textures from a replacement
    // device. Retirement is permanent; retained old textures stay owned but must
    // never be submitted or read back, including by other viewers.
    void RetireDevice() noexcept { retired_ = true; quarantined = true; }
    bool DeviceRetired() noexcept {
        if (!retired_ && device && FAILED(device->device()->GetDeviceRemovedReason())) RetireDevice();
        return retired_;
    }
    const std::shared_ptr<D3dVideoDevice> device;
    std::atomic<bool> quarantined{false};
    std::atomic<uint64_t> hardwareFrames{0}, softwareFallbacks{0}, maxFrameMicroseconds{0};
private:
    std::atomic<bool> retired_{false};
};
}
