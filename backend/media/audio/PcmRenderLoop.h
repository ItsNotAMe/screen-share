#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <stop_token>

namespace screenshare::media {
class PcmRenderStalled : public std::runtime_error {
public:
    PcmRenderStalled() : std::runtime_error("PCM render write stalled") {}
};
// One 10 ms stereo block. Native callbacks own padding, buffer commit and the
// existing bounded event wait; tests supply those operations without a device.
template<class Available, class Commit, class Wait, class Now = decltype(&std::chrono::steady_clock::now)>
void WritePcmRenderBlock(std::stop_token stop, Available available, Commit commit, Wait wait,
    Now now = std::chrono::steady_clock::now) {
    // A normal 10 ms block fits comfortably within the 50 ms endpoint reserve.
    // Bound the whole write, so events or tiny partial writes cannot keep a
    // wedged engine alive indefinitely. Check capacity first: a delayed owner
    // may still have a healthy empty buffer when it finally runs again.
    const auto deadline = now() + std::chrono::seconds(1);
    uint32_t offset = 0;
    while (offset < 480 && !stop.stop_requested()) {
        const auto frames = std::min(480u - offset, available());
        if (stop.stop_requested()) return;
        if (now() >= deadline && frames < 480u - offset) throw PcmRenderStalled();
        if (frames) { commit(offset, frames); offset += frames; }
        else wait();
    }
}
}
