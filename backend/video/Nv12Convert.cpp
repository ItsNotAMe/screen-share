#include "video/Nv12Convert.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace screenshare {
namespace {

uint8_t ClampToByte(int value)
{
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

void ValidateBgraFrame(const std::byte* data, uint32_t rowPitch, int width, int height)
{
    if (data == nullptr) {
        throw std::runtime_error("BGRA frame data is missing");
    }
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("BGRA frame dimensions are not available");
    }
    if ((width % 2) != 0 || (height % 2) != 0) {
        throw std::runtime_error("BGRA frame dimensions must be even for NV12 conversion");
    }

    const uint64_t requiredStride = static_cast<uint64_t>(width) * 4;
    if (rowPitch < requiredStride) {
        throw std::runtime_error("BGRA frame row pitch is too small");
    }
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

std::vector<std::byte> ConvertBgraToNv12(const std::byte* data, uint32_t rowPitch, int width, int height)
{
    ValidateBgraFrame(data, rowPitch, width, height);

    std::vector<std::byte> nv12(static_cast<size_t>(width) * height * 3 / 2);

    auto* yPlane = reinterpret_cast<uint8_t*>(nv12.data());
    auto* uvPlane = yPlane + static_cast<size_t>(width) * height;
    const auto* source = reinterpret_cast<const uint8_t*>(data);

    for (int y = 0; y < height; ++y) {
        const auto* row = source + static_cast<size_t>(rowPitch) * y;
        for (int x = 0; x < width; ++x) {
            const uint8_t b = row[x * 4 + 0];
            const uint8_t g = row[x * 4 + 1];
            const uint8_t r = row[x * 4 + 2];
            const int luma = ((47 * r + 157 * g + 16 * b + 128) >> 8) + 16;
            yPlane[static_cast<size_t>(y) * width + x] = ClampToByte(luma);
        }
    }

    for (int y = 0; y < height; y += 2) {
        const auto* row0 = source + static_cast<size_t>(rowPitch) * y;
        const auto* row1 = source + static_cast<size_t>(rowPitch) * (y + 1);

        for (int x = 0; x < width; x += 2) {
            int uSum = 0;
            int vSum = 0;

            const std::array<const uint8_t*, 4> pixels = {
                row0 + x * 4,
                row0 + (x + 1) * 4,
                row1 + x * 4,
                row1 + (x + 1) * 4,
            };

            for (const uint8_t* pixel : pixels) {
                const uint8_t b = pixel[0];
                const uint8_t g = pixel[1];
                const uint8_t r = pixel[2];
                uSum += ((-26 * r - 87 * g + 112 * b + 128) >> 8) + 128;
                vSum += ((112 * r - 102 * g - 10 * b + 128) >> 8) + 128;
            }

            const size_t uvIndex = static_cast<size_t>(y / 2) * width + x;
            uvPlane[uvIndex + 0] = ClampToByte(uSum / 4);
            uvPlane[uvIndex + 1] = ClampToByte(vSum / 4);
        }
    }

    return nv12;
}

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

            bgraRow[x * 4 + 0] = ClampToByte((298 * c + 541 * chromaU + 128) >> 8);
            bgraRow[x * 4 + 1] = ClampToByte((298 * c - 55 * chromaU - 136 * chromaV + 128) >> 8);
            bgraRow[x * 4 + 2] = ClampToByte((298 * c + 459 * chromaV + 128) >> 8);
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
