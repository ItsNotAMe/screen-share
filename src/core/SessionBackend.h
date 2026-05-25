#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace screenshare {

enum class SessionRole {
    Share,
    Watch,
};

enum class SessionState {
    Idle,
    Starting,
    Connecting,
    Live,
    Recovering,
    Disconnected,
    Stopping,
    Stopped,
    Failed,
};

enum class ShareConnectionMode {
    Room,
    DirectTargets,
    ManualInvite,
};

enum class WatchConnectionMode {
    Room,
    DirectListen,
    ManualInvite,
};

enum class SessionEventType {
    StateChanged,
    LogLine,
    VideoFrameReady,
    AudioLevelChanged,
    ViewerListChanged,
    NatStatusChanged,
    SettingsChanged,
    StreamStatusChanged,
    Issue,
    Error,
};

enum class SessionIssue {
    None,
    AccessCodeRequired,
    AccessCodeMismatch,
    RoomAlreadyOpen,
    PreviewClosed,
};

struct SessionResolution {
    int width = 0;
    int height = 0;
};

struct StreamSettings {
    std::optional<SessionResolution> outputResolution;
    int fps = 60;
    uint32_t bitrateBps = 0;
    bool adaptBitrate = true;
    bool adaptResolution = true;
    bool adaptFps = false;
};

struct ShareSessionConfig {
    ShareConnectionMode connectionMode = ShareConnectionMode::Room;
    int displayIndex = 0;
    uint16_t roomPort = 0;
    std::string roomId;
    std::string roomName;
    std::string roomPassword;
    std::string signalingStunServer;
    std::string udpAccessCode;
    bool allowPlaintext = false;
    std::string reportPath;
    std::string audioDeviceId;
    std::vector<std::string> targets;
    std::string localInvite;
    std::vector<std::string> watcherInvites;
    bool captureSystemAudio = true;
    bool hostAudioMuted = false;
    StreamSettings stream;
};

struct WatchSessionConfig {
    WatchConnectionMode connectionMode = WatchConnectionMode::Room;
    uint16_t listenPort = 0;
    std::string roomId;
    std::string roomPassword;
    std::string signalingStunServer;
    std::string udpAccessCode;
    bool allowPlaintext = false;
    std::string reportPath;
    bool playAudio = true;
    bool muted = false;
    bool lanAdvertise = false;
    std::string peerInvite;
    int previewLatencyMs = 100;
    int audioPlaybackVolumePercent = 100;
};

struct SessionViewer {
    std::string id;
    std::string endpoint;
    int group = -1;
    SessionState state = SessionState::Idle;
    std::string health;
    std::string sessionFingerprint;
    uint64_t feedbackPackets = 0;
    uint64_t completedFrames = 0;
    uint64_t decodeResyncs = 0;
    uint64_t pendingDatagrams = 0;
    uint64_t queueDelayMs = 0;
    bool hasFeedback = false;
    bool activeNow = false;
};

struct SessionStreamStatus {
    bool hasStats = false;
    std::optional<SessionResolution> sourceResolution;
    std::optional<SessionResolution> outputResolution;
    double outputFps = 0.0;
    double desktopUpdateFps = 0.0;
    double bitrateMbps = 0.0;
    double resolutionScale = 1.0;
    uint64_t totalOutputFrames = 0;
    uint64_t streamQueueDepth = 0;
    uint64_t streamDroppedFrames = 0;
    uint64_t udpQueueMs = 0;
    std::string resolutionAdaptation;
    uint64_t resolutionAdaptations = 0;
    std::string bitrateAdaptation;
    uint64_t bitrateAdaptations = 0;
};

struct SessionStatus {
    SessionRole role = SessionRole::Share;
    SessionState state = SessionState::Idle;
    std::string summary;
    std::optional<SessionResolution> videoResolution;
    SessionStreamStatus stream;
    std::vector<SessionViewer> viewers;
    std::string health;
    uint64_t completedFrames = 0;
    uint64_t payloadBytes = 0;
    uint64_t decodedFrames = 0;
    uint64_t audioPackets = 0;
    std::string natStatus;
    std::string natHint;
};

struct SessionEvent {
    SessionEventType type = SessionEventType::StateChanged;
    SessionStatus status;
    SessionIssue issue = SessionIssue::None;
    std::string message;
};

class ISessionObserver {
public:
    virtual ~ISessionObserver() = default;
    virtual void OnSessionEvent(const SessionEvent& event) = 0;
};

class ISessionBackend {
public:
    virtual ~ISessionBackend() = default;

    virtual void StartShare(const ShareSessionConfig& config, ISessionObserver& observer) = 0;
    virtual void StartWatch(const WatchSessionConfig& config, ISessionObserver& observer) = 0;
    virtual void Stop() = 0;
    virtual void ApplyStreamSettings(const StreamSettings& settings) = 0;
};

const char* ToString(SessionState state);
bool IsTerminalSessionState(SessionState state);

} // namespace screenshare
