#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace screenshare::media {
enum class StreamPreset { Gaming, Quality };
enum class SettingMode { Auto, Manual };
enum class ResolutionMode { Auto, Fixed, Native };
enum class StreamDegradation { MaintainFps, MaintainResolution, Balanced, Disabled };
struct StreamPreferences {
    StreamPreset preset = StreamPreset::Gaming;
    ResolutionMode resolution = ResolutionMode::Auto;
    int width = 1920, height = 1080;
    SettingMode fpsMode = SettingMode::Manual;
    int fps = 60;
    SettingMode bitrateMode = SettingMode::Auto;
    std::optional<int> bitrateLimitBps;
};
struct StreamLimits {
    int maxVideoBitrateBps, initialVideoBitrateBps;
    StreamDegradation degradation;
};
inline StreamLimits ValidateStreamPreferences(const StreamPreferences& value) {
    if ((value.preset != StreamPreset::Gaming && value.preset != StreamPreset::Quality) ||
        (value.resolution != ResolutionMode::Auto && value.resolution != ResolutionMode::Fixed && value.resolution != ResolutionMode::Native) ||
        (value.fpsMode != SettingMode::Auto && value.fpsMode != SettingMode::Manual) ||
        (value.bitrateMode != SettingMode::Auto && value.bitrateMode != SettingMode::Manual) ||
        value.width < 2 || value.height < 2 || value.width > 3840 || value.height > 2160 ||
        value.width % 2 || value.height % 2 || value.fps < 1 || value.fps > 240 ||
        (value.bitrateMode == SettingMode::Manual && !value.bitrateLimitBps) ||
        (value.bitrateLimitBps && (*value.bitrateLimitBps < 1000 || *value.bitrateLimitBps > 100000000)))
        throw std::invalid_argument("Invalid stream preferences");
    const auto calculated = std::clamp<int64_t>(int64_t(value.width) * value.height * value.fps / 10, 2000000, 40000000);
    const int maximum = value.bitrateLimitBps.value_or(static_cast<int>(calculated));
    const bool autoSize = value.resolution == ResolutionMode::Auto;
    const bool autoFps = value.fpsMode == SettingMode::Auto;
    const auto degradation = !autoSize && !autoFps ? StreamDegradation::Disabled :
        !autoSize ? StreamDegradation::MaintainResolution :
        !autoFps || value.preset == StreamPreset::Gaming ? StreamDegradation::MaintainFps : StreamDegradation::Balanced;
    return {maximum, std::min(3000000, maximum), degradation};
}
}
