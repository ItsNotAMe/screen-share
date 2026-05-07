#include "video/Nv12Convert.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace screenshare {
namespace {

uint8_t ClampToByte(int value)
{
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

void ValidateNv12Frame(const std::byte* data, size_t dataBytes, int width, int height)
{
    if (data == nullptr) {
        throw std::runtime_error("Decoded NV12 frame data is missing");
    }
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("Decoded NV12 frame dimensions are not available");
    }
    if ((width % 2) != 0 || (height % 2) != 0) {
        throw std::runtime_error("Decoded NV12 frame dimensions must be even");
    }

    const uint64_t yPlaneBytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    const uint64_t requiredBytes = yPlaneBytes + yPlaneBytes / 2;
    if (requiredBytes > std::numeric_limits<size_t>::max() || dataBytes < static_cast<size_t>(requiredBytes)) {
        throw std::runtime_error("Decoded NV12 frame data is too small");
    }

    const uint64_t bgraBytes = yPlaneBytes * 4;
    if (bgraBytes > std::numeric_limits<size_t>::max()) {
        throw std::runtime_error("Decoded frame is too large to convert to BGRA");
    }
}

} // namespace

void ConvertNv12ToBgra(const std::byte* data, size_t dataBytes, int width, int height, std::vector<uint8_t>& bgra)
{
    ValidateNv12Frame(data, dataBytes, width, height);

    const size_t yPlaneBytes = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t pixelBytes = yPlaneBytes * 4;
    bgra.resize(pixelBytes);

    const auto* yPlane = reinterpret_cast<const uint8_t*>(data);
    const auto* uvPlane = yPlane + yPlaneBytes;

    for (int y = 0; y < height; ++y) {
        const auto* yRow = yPlane + static_cast<size_t>(y) * width;
        const auto* uvRow = uvPlane + static_cast<size_t>(y / 2) * width;
        auto* bgraRow = bgra.data() + static_cast<size_t>(y) * width * 4;

        for (int x = 0; x < width; ++x) {
            const int luma = static_cast<int>(yRow[x]) - 16;
            const int chromaU = static_cast<int>(uvRow[(x / 2) * 2 + 0]) - 128;
            const int chromaV = static_cast<int>(uvRow[(x / 2) * 2 + 1]) - 128;
            const int c = std::max(0, luma);

            bgraRow[x * 4 + 0] = ClampToByte((298 * c + 516 * chromaU + 128) >> 8);
            bgraRow[x * 4 + 1] = ClampToByte((298 * c - 100 * chromaU - 208 * chromaV + 128) >> 8);
            bgraRow[x * 4 + 2] = ClampToByte((298 * c + 409 * chromaV + 128) >> 8);
            bgraRow[x * 4 + 3] = 255;
        }
    }
}

std::vector<uint8_t> ConvertNv12ToBgra(const std::byte* data, size_t dataBytes, int width, int height)
{
    std::vector<uint8_t> bgra;
    ConvertNv12ToBgra(data, dataBytes, width, height, bgra);
    return bgra;
}

} // namespace screenshare
