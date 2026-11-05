#pragma once
#include "PcmAudioEndpoint.h"
#include "PcmBlockPacer.h"

namespace screenshare::media {
// Device-free source for an audio track that can resume without renegotiation.
// Preserve the 10ms phase across normal wake jitter. Missed slots never replay
// a burst of old silence. Cancellation also interrupts the wait.
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
