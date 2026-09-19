#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace screenshare::media {
enum class StreamPreset { Gaming, Quality };
enum class SettingsApplyError { None, Invalid, StaleRevision, UnsupportedTopology, SenderRejected };
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
    std::optional<int> aggregateUploadLimitBps;
    bool videoPaused = false; // Session-only; never persisted as a new-room default.
};
struct StreamLimits {
    int maxVideoBitrateBps, initialVideoBitrateBps;
    StreamDegradation degradation;
};
inline constexpr int kViewerAudioAllowanceBps = 128000;
inline StreamLimits ValidateStreamPreferences(const StreamPreferences& value) {
    if ((value.preset != StreamPreset::Gaming && value.preset != StreamPreset::Quality) ||
        (value.resolution != ResolutionMode::Auto && value.resolution != ResolutionMode::Fixed && value.resolution != ResolutionMode::Native) ||
        (value.fpsMode != SettingMode::Auto && value.fpsMode != SettingMode::Manual) ||
        (value.bitrateMode != SettingMode::Auto && value.bitrateMode != SettingMode::Manual) ||
        value.width < 2 || value.height < 2 || value.width > 3840 || value.height > 2160 ||
        value.width % 2 || value.height % 2 || value.fps < 1 || value.fps > 240 ||
        (value.bitrateMode == SettingMode::Manual && !value.bitrateLimitBps) ||
        (value.bitrateLimitBps && (*value.bitrateLimitBps < 1000 || *value.bitrateLimitBps > 100000000)) ||
        (value.aggregateUploadLimitBps && (*value.aggregateUploadLimitBps < 160000 || *value.aggregateUploadLimitBps > 1000000000)))
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
// Application allowance, not a physical-interface shaper. Pending peers reserve
// a share too, avoiding over-allocation while their negotiation completes.
// Audio allowance is per unicast viewer; actual wire usage remains independent.
inline int AllocateViewerVideo(const StreamPreferences& preferences, size_t viewers) {
    if (viewers > 63) throw std::invalid_argument("Invalid viewer count");
    const auto individual = ValidateStreamPreferences(preferences).maxVideoBitrateBps;
    if (preferences.videoPaused) return 0;
    if (!preferences.aggregateUploadLimitBps || !viewers) return individual;
    const auto available = std::max<int64_t>(0, int64_t(*preferences.aggregateUploadLimitBps) * 4 / 5 - int64_t(viewers) * kViewerAudioAllowanceBps);
    const auto share = available / int64_t(viewers);
    return share < 1000 ? 0 : int(std::min<int64_t>(individual, share));
}
}
