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
    const std::shared_ptr<D3dVideoDevice> device;
    std::atomic<bool> quarantined{false};
    std::atomic<uint64_t> hardwareFrames{0}, softwareFallbacks{0}, maxFrameMicroseconds{0};
};
}
