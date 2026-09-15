#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>

namespace screenshare::media {
// Portable endpoint contract: 10 ms, 48 kHz, interleaved stereo signed PCM16.
using PcmBlock = std::array<int16_t, 960>;
class PcmCaptureEndpoint {
public:
    virtual ~PcmCaptureEndpoint() = default;
    virtual void Start() = 0;
    virtual bool Read(PcmBlock&, std::stop_token) = 0;
    virtual uint32_t DelayMs() const = 0;
    virtual uint64_t DroppedFrames() const { return 0; }
};
class PcmPlayoutEndpoint {
public:
    virtual ~PcmPlayoutEndpoint() = default;
    virtual void Start() = 0;
    virtual void Write(const PcmBlock&, std::stop_token) = 0;
    virtual uint32_t DelayMs() const = 0;
    virtual uint32_t BufferFrames() const = 0;
    // Zero means the endpoint cannot report the actual engine period.
    virtual uint32_t EnginePeriodUs() const { return 0; }
};
struct PcmEndpointFactories {
    std::function<std::unique_ptr<PcmCaptureEndpoint>()> capture;
    std::function<std::unique_ptr<PcmPlayoutEndpoint>()> playout;
};
}
