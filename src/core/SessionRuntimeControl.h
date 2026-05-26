#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace screenshare {

enum class RuntimeResolutionMode {
    Auto,
    Native,
    Fixed,
};

struct RuntimeResolutionRequest {
    RuntimeResolutionMode mode = RuntimeResolutionMode::Auto;
    int width = 0;
    int height = 0;
};

struct RuntimeStreamSettingsRequest {
    std::optional<RuntimeResolutionRequest> resolution;
};

struct RuntimeAudioPlaybackSettingsRequest {
    std::optional<bool> muted;
    std::optional<int> volumePercent;
};

class ISessionRuntimeControl {
public:
    virtual ~ISessionRuntimeControl() = default;

    virtual bool StopRequested() = 0;
    virtual std::optional<RuntimeStreamSettingsRequest> TakeStreamSettingsRequest() = 0;
    virtual std::optional<RuntimeAudioPlaybackSettingsRequest> TakeAudioPlaybackSettingsRequest() = 0;
};

class NullSessionRuntimeControl final : public ISessionRuntimeControl {
public:
    bool StopRequested() override;
    std::optional<RuntimeStreamSettingsRequest> TakeStreamSettingsRequest() override;
    std::optional<RuntimeAudioPlaybackSettingsRequest> TakeAudioPlaybackSettingsRequest() override;
};

class MemorySessionRuntimeControl final : public ISessionRuntimeControl {
public:
    bool StopRequested() override;
    std::optional<RuntimeStreamSettingsRequest> TakeStreamSettingsRequest() override;
    std::optional<RuntimeAudioPlaybackSettingsRequest> TakeAudioPlaybackSettingsRequest() override;

    void RequestStop();
    void ResetStop();
    void RequestStreamSettings(RuntimeStreamSettingsRequest request);
    void RequestAudioPlaybackSettings(RuntimeAudioPlaybackSettingsRequest request);
    void ClearStreamSettingsRequest();
    void Reset();

private:
    std::mutex mutex_;
    bool stopRequested_ = false;
    std::optional<RuntimeStreamSettingsRequest> streamSettingsRequest_;
    std::optional<RuntimeAudioPlaybackSettingsRequest> audioPlaybackSettingsRequest_;
};

class FileSessionRuntimeControl final : public ISessionRuntimeControl {
public:
    FileSessionRuntimeControl(std::string stopFilePath, std::string controlFilePath);

    bool StopRequested() override;
    std::optional<RuntimeStreamSettingsRequest> TakeStreamSettingsRequest() override;
    std::optional<RuntimeAudioPlaybackSettingsRequest> TakeAudioPlaybackSettingsRequest() override;

private:
    std::filesystem::path stopFilePath_;
    std::filesystem::path controlFilePath_;
    std::optional<std::filesystem::file_time_type> lastControlWriteTime_;
    std::string lastControlContent_;
};

std::optional<RuntimeStreamSettingsRequest> ParseRuntimeStreamSettingsRequest(std::string_view content);

} // namespace screenshare
