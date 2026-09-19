#pragma once
#include "PcmAudioEndpoint.h"
#include "PcmBlockPacer.h"

namespace screenshare::media {
// Consume decoded audio without opening a device or accumulating a replay queue.
class DiscardPcmPlayout final : public PcmPlayoutEndpoint {
    PcmBlockPacer pacer_;
public:
    void Start() override { pacer_.Start(); }
    void Write(const PcmBlock&, std::stop_token stop) override { pacer_.Wait(stop); }
    uint32_t DelayMs() const override { return 0; }
    uint32_t BufferFrames() const override { return 0; }
};
}
