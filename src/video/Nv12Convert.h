#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace screenshare {

void ConvertNv12ToBgra(const std::byte* data, size_t dataBytes, int width, int height, std::vector<uint8_t>& bgra);
std::vector<uint8_t> ConvertNv12ToBgra(const std::byte* data, size_t dataBytes, int width, int height);

} // namespace screenshare
