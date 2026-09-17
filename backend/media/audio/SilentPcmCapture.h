#pragma once
#include "PcmAudioEndpoint.h"
#include "PcmBlockPacer.h"

namespace screenshare::media {
// Device-free source for an audio track that can resume without renegotiation.
// Pace each block from the actual wake time: a delayed worker never catches up
// by emitting a burst of old silence. Cancellation also interrupts the wait.
class SilentPcmCapture final : public PcmCaptureEndpoint {
    PcmBlockPacer pacer_;
public:
    void Start() override { pacer_.Start(); }
    bool Read(PcmBlock& block, std::stop_token stop) override {
        if (!pacer_.Wait(stop)) return false;
        block.fill(0);
        return true;
    }
    uint32_t DelayMs() const override { return 0; }
};
}
