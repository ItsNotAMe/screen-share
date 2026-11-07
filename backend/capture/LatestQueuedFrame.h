#pragma once
#include <utility>

namespace screenshare {
// WGC has a two-frame pool. Prefer a newer queued picture before conversion,
// with bounded work even when the producer keeps filling the pool. The caller
// owns closing the final frame (including when polling throws).
template<class Frame, class Poll>
void SelectLatestQueuedFrame(Frame& current, Poll poll) {
    for (int pending = 0; current && pending < 2; ++pending) {
        auto newer = poll();
        if (!newer) break;
        current.Close();
        current = std::move(newer);
    }
}
}
