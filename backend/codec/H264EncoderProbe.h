#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace screenshare {

struct H264EncoderProbeConfig {
    int width = 1280;
    int height = 720;
    int fps = 60;
    uint32_t bitrate = 8'000'000;
};

struct H264EncoderProbeInfo {
    std::string friendlyName;
    std::string clsid;
    std::string hardwareUrl;
    bool hardware = false;
    bool async = false;
    bool asyncUnlocked = false;
    bool d3d11Aware = false;
    bool d3dManagerAccepted = false;
    bool streamTypesAccepted = false;
    std::string activationError;
    std::string d3dManagerError;
    std::string streamTypeError;
};

std::vector<H264EncoderProbeInfo> ProbeH264Encoders(const H264EncoderProbeConfig& config);

} // namespace screenshare
