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
    AudioStatusChanged,
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

struct SessionDisplayInfo {
    int index = 0;
    std::string outputName;
    int width = 0;
    int height = 0;
    int left = 0;
    int top = 0;
    std::string adapterName;
    bool attached = true;
};

enum class SessionAudioDeviceSource {
    SystemOutput,
    Microphone,
};

struct SessionAudioDeviceInfo {
    SessionAudioDeviceSource source = SessionAudioDeviceSource::SystemOutput;
    std::string id;
    std::string name;
    bool isDefault = false;
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
    std::string sessionId;
    int displayIndex = 0;
    int seconds = 0;
    uint16_t roomPort = 0;
    uint16_t udpLocalPort = 0;
    std::string roomId;
    std::string roomName;
    std::string roomPassword;
    std::string signalingServerUrl;
    std::string signalingStunServer;
    int signalingTimeoutMs = 5000;
    int signalingSetupSeconds = 5;
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
    std::string sessionId;
    int seconds = 0;
    uint16_t listenPort = 0;
    std::string roomId;
    std::string roomPassword;
    std::string signalingServerUrl;
    std::string signalingStunServer;
    int signalingTimeoutMs = 5000;
    int signalingSetupSeconds = 5;
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

struct SessionAudioStatus {
    bool hasStats = false;
    std::string codec;
    std::string format;
    std::string captureState;
    uint64_t capturePackets = 0;
    uint64_t captureFrames = 0;
    uint64_t captureBytes = 0;
    uint64_t captureEmptyPolls = 0;
    uint64_t captureDiscontinuities = 0;
    uint64_t captureTimestampErrors = 0;
    double capturePacketsPerSecond = 0.0;
    double captureFramesPerSecond = 0.0;
    double payloadBitrateMbps = 0.0;
    uint64_t packets = 0;
    uint64_t frames = 0;
    uint64_t bytes = 0;
    uint64_t silentPackets = 0;
    uint64_t discontinuities = 0;
    uint64_t timestampErrors = 0;
    uint64_t udpPackets = 0;
    uint64_t udpFrames = 0;
    uint64_t udpDatagrams = 0;
    uint64_t udpPendingDatagrams = 0;
    uint64_t udpDroppedPackets = 0;
    uint64_t udpWireBytes = 0;
    std::string playbackState;
    bool playbackMuted = false;
    int playbackVolumePercent = 100;
    int playbackLatencyMs = 0;
    uint64_t playbackQueuePackets = 0;
    uint64_t playbackQueueMs = 0;
    uint64_t playbackPackets = 0;
    uint64_t playbackFrames = 0;
    uint64_t playbackDrops = 0;
    uint64_t playbackLatencyDrops = 0;
    uint64_t playbackSyncDrops = 0;
    uint64_t playbackSyncWaits = 0;
    uint64_t playbackMissingPackets = 0;
    uint64_t playbackBackpressure = 0;
    uint64_t renderBufferFullEvents = 0;
    uint64_t renderPaddingFrames = 0;
    std::string avSyncState;
    std::string avSyncCorrection;
    int avAudioAheadMs = 0;
    int avAudioElapsedMs = 0;
    int avVideoElapsedMs = 0;
    int avPlayoutAudioAheadMs = 0;
    uint64_t avAudioPackets = 0;
    uint64_t avIgnoredAudioPackets = 0;
    uint64_t avVideoFrames = 0;
};

struct SessionStatus {
    SessionRole role = SessionRole::Share;
    SessionState state = SessionState::Idle;
    std::string summary;
    std::optional<SessionResolution> videoResolution;
    SessionStreamStatus stream;
    SessionAudioStatus audio;
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

class ISessionEventSink {
public:
    virtual ~ISessionEventSink() = default;
    virtual void OnSessionEvent(const SessionEvent& event) = 0;
};

class IScreenShareSession {
public:
    virtual ~IScreenShareSession() = default;

    virtual void StartShare(const ShareSessionConfig& config, ISessionEventSink& eventSink) = 0;
    virtual void StartWatch(const WatchSessionConfig& config, ISessionEventSink& eventSink) = 0;
    virtual void Stop() = 0;
    virtual void ApplyStreamSettings(const StreamSettings& settings) = 0;
    virtual SessionStatus GetStatus() const = 0;
    virtual std::vector<SessionDisplayInfo> ListDisplays() = 0;
    virtual std::vector<SessionAudioDeviceInfo> ListAudioDevices() = 0;
};

const char* ToString(SessionState state);
bool IsTerminalSessionState(SessionState state);

} // namespace screenshare
