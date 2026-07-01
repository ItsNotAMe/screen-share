#pragma once

#include "core/ScreenShareSession.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace screenshare {

enum class RuntimeResolutionMode {
    Auto,
    Native,
    Fixed,
};

enum class RuntimeCaptureSourceType {
    Display,
    Window,
};

struct RuntimeResolutionRequest {
    RuntimeResolutionMode mode = RuntimeResolutionMode::Auto;
    int width = 0;
    int height = 0;
};

struct RuntimeStreamSettingsRequest {
    std::optional<std::string> roomName;
    std::optional<RuntimeCaptureSourceType> captureSourceType;
    std::optional<int> displayIndex;
    std::optional<uint64_t> windowHandle;
    std::optional<uint32_t> windowProcessId;
    std::optional<RuntimeResolutionRequest> resolution;
    std::optional<int> fps;
    std::optional<uint32_t> bitrateBps;
    std::optional<bool> adaptBitrate;
    std::optional<bool> adaptResolution;
    std::optional<bool> adaptFps;
    std::optional<bool> captureSystemAudio;
    std::optional<bool> hostAudioMuted;
    std::optional<bool> videoPaused;
    std::optional<std::string> audioDeviceId;
    std::optional<bool> lowLatency;
};

struct RuntimeAudioPlaybackSettingsRequest {
    std::optional<bool> muted;
    std::optional<int> volumePercent;
};

// Host-initiated grant/revoke for one viewer. capabilities is a ControlCapability
// bitmask; 0 revokes all control from that viewer.
struct RuntimeViewerControlRequest {
    std::string viewerId;
    uint32_t capabilities = 0;
};

class ISessionRuntimeControl {
public:
    virtual ~ISessionRuntimeControl() = default;

    virtual bool StopRequested() = 0;
    virtual std::optional<RuntimeStreamSettingsRequest> TakeStreamSettingsRequest() = 0;
    virtual std::optional<RuntimeAudioPlaybackSettingsRequest> TakeAudioPlaybackSettingsRequest() = 0;

    // Host: pull the next grant/revoke request the UI queued. Viewer side: queue
    // and drain high-frequency input events (separate from the take-once requests
    // above so input is never silently coalesced into a single latest-only slot).
    virtual std::optional<RuntimeViewerControlRequest> TakeViewerControlRequest() = 0;
    virtual void EnqueueInput(const RemoteInputEvent& event) = 0;
    virtual std::vector<RemoteInputEvent> DrainInput() = 0;
};

class NullSessionRuntimeControl final : public ISessionRuntimeControl {
public:
    bool StopRequested() override;
    std::optional<RuntimeStreamSettingsRequest> TakeStreamSettingsRequest() override;
    std::optional<RuntimeAudioPlaybackSettingsRequest> TakeAudioPlaybackSettingsRequest() override;
    std::optional<RuntimeViewerControlRequest> TakeViewerControlRequest() override;
    void EnqueueInput(const RemoteInputEvent& event) override;
    std::vector<RemoteInputEvent> DrainInput() override;
};

class MemorySessionRuntimeControl final : public ISessionRuntimeControl {
public:
    bool StopRequested() override;
    std::optional<RuntimeStreamSettingsRequest> TakeStreamSettingsRequest() override;
    std::optional<RuntimeAudioPlaybackSettingsRequest> TakeAudioPlaybackSettingsRequest() override;
    std::optional<RuntimeViewerControlRequest> TakeViewerControlRequest() override;
    void EnqueueInput(const RemoteInputEvent& event) override;
    std::vector<RemoteInputEvent> DrainInput() override;

    void RequestStop();
    void ResetStop();
    void RequestStreamSettings(RuntimeStreamSettingsRequest request);
    void RequestAudioPlaybackSettings(RuntimeAudioPlaybackSettingsRequest request);
    void RequestViewerControl(RuntimeViewerControlRequest request);
    void ClearStreamSettingsRequest();
    void Reset();

private:
    std::mutex mutex_;
    bool stopRequested_ = false;
    std::optional<RuntimeStreamSettingsRequest> streamSettingsRequest_;
    std::optional<RuntimeAudioPlaybackSettingsRequest> audioPlaybackSettingsRequest_;
    std::deque<RuntimeViewerControlRequest> viewerControlRequests_;
    std::deque<RemoteInputEvent> inputQueue_;
};

class FileSessionRuntimeControl final : public ISessionRuntimeControl {
public:
    FileSessionRuntimeControl(std::string stopFilePath, std::string controlFilePath);

    bool StopRequested() override;
    std::optional<RuntimeStreamSettingsRequest> TakeStreamSettingsRequest() override;
    std::optional<RuntimeAudioPlaybackSettingsRequest> TakeAudioPlaybackSettingsRequest() override;
    std::optional<RuntimeViewerControlRequest> TakeViewerControlRequest() override;
    void EnqueueInput(const RemoteInputEvent& event) override;
    std::vector<RemoteInputEvent> DrainInput() override;

private:
    std::filesystem::path stopFilePath_;
    std::filesystem::path controlFilePath_;
    std::optional<std::filesystem::file_time_type> lastControlWriteTime_;
    std::string lastControlContent_;
};

std::optional<RuntimeStreamSettingsRequest> ParseRuntimeStreamSettingsRequest(std::string_view content);

} // namespace screenshare
