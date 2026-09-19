#pragma once
#include <cstdint>

namespace screenshare::media {
enum class SourceScalingPath { Unknown, Unchanged, Gpu, Cpu, CpuReadback };
struct SourceVideoStatus {
    uint64_t observedRevision = 0, dropped = 0, scaled = 0, gpuReadbackFallbacks = 0, gpuScaled = 0;
    uint64_t gpuBusyDrops = 0;
    int width = 0, height = 0, imageLeft = 0, imageTop = 0, imageWidth = 0, imageHeight = 0;
    SourceScalingPath scalingPath = SourceScalingPath::Unknown;
};
}
