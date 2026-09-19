#pragma once
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

namespace screenshare::input {
// Describes the image inside this exact encoded canvas, never the latest stats.
struct FrameMapping {
    uint64_t generation = 0;
    uint16_t width = 0, height = 0, left = 0, top = 0, imageWidth = 0, imageHeight = 0;
    bool operator==(const FrameMapping&) const = default;
    bool Valid() const {
        return generation && width >= 2 && height >= 2 && width <= 3840 && height <= 2160 &&
            imageWidth >= 2 && imageHeight >= 2 && unsigned(left) + imageWidth <= width && unsigned(top) + imageHeight <= height;
    }
    std::optional<std::pair<float,float>> Point(float x, float y) const {
        if (!Valid() || !std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1) return {};
        const double px = double(x) * (width - 1), py = double(y) * (height - 1);
        if (px < left || py < top || px > left + imageWidth - 1 || py > top + imageHeight - 1) return {};
        return std::pair{float((px-left)/(imageWidth-1)),float((py-top)/(imageHeight-1))};
    }
};
}
