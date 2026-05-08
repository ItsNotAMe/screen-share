#pragma once

#include <cstddef>
#include <span>

namespace screenshare {

struct H264AccessUnitInfo {
    bool hasIdrSlice = false;
    bool hasSps = false;
    bool hasPps = false;
};

H264AccessUnitInfo InspectH264AccessUnit(std::span<const std::byte> bytes);

} // namespace screenshare
