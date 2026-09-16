#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace screenshare {
// Packed, visible-aperture NV12. Retained pixels are immutable and their owner
// outlives every queued frame. Legacy producers may still move an owned vector.
struct Nv12VideoFrame {
    int width = 0, height = 0, codedWidth = 0, codedHeight = 0;
    int64_t timestamp100ns = 0, duration100ns = 0;
    std::vector<uint8_t> nv12;
    std::shared_ptr<const uint8_t> retainedPixels;
    size_t retainedBytes = 0;
    std::span<const uint8_t> pixels() const {
        return retainedPixels ? std::span<const uint8_t>(retainedPixels.get(), retainedBytes) : std::span<const uint8_t>(nv12);
    }
};
}
