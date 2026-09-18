#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include "input/v2/FrameMapping.h"
struct ID3D11Texture2D;

namespace screenshare {
// The owner retains both device and immutable visible NV12 texture. CPU access
// is an explicit, cached fallback for offscreen rendering and image assertions.
class NativeNv12Frame {
public:
    virtual ~NativeNv12Frame() = default;
    virtual ID3D11Texture2D* texture() const = 0;
    virtual std::span<const uint8_t> pixels() const = 0;
};
// Packed, visible-aperture NV12. Retained pixels are immutable and their owner
// outlives every queued frame. Legacy producers may still move an owned vector.
struct Nv12VideoFrame {
    int width = 0, height = 0, codedWidth = 0, codedHeight = 0;
    int64_t timestamp100ns = 0, duration100ns = 0;
    input::FrameMapping inputMapping;
    std::vector<uint8_t> nv12;
    std::shared_ptr<const uint8_t> retainedPixels;
    size_t retainedBytes = 0;
    std::shared_ptr<const NativeNv12Frame> native;
    std::span<const uint8_t> pixels() const {
        if (native) return native->pixels();
        return retainedPixels ? std::span<const uint8_t>(retainedPixels.get(), retainedBytes) : std::span<const uint8_t>(nv12);
    }
};
}
