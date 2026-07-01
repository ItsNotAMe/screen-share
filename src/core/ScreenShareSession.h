#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
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
    ControlStateChanged,
    ControlRequested,
    Issue,
    Error,
};

enum class SessionIssue {
    None,
    AccessCodeRequired,
    AccessCodeMismatch,
    RoomAlreadyOpen,
    PreviewClosed,
    HostLeft,
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

struct SessionWindowInfo {
    uint64_t handle = 0;
    uint32_t processId = 0;
    std::string title;
    std::string processName;
    int width = 0;
    int height = 0;
    int left = 0;
    int top = 0;
};

enum class SessionCaptureSourceType {
    Display,
    Window,
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
    bool lowLatency = false; // disable send pacing/queue buffering for minimal latency
};

struct ShareSessionSettings {
    std::optional<std::string> roomName;
    std::optional<SessionCaptureSourceType> captureSourceType;
    std::optional<int> displayIndex;
    std::optional<uint64_t> windowHandle;
    std::optional<uint32_t> windowProcessId;
    std::optional<bool> captureSystemAudio;
    std::optional<bool> hostAudioMuted;
    std::optional<bool> hostVideoPaused;
    std::optional<std::string> audioDeviceId;
    StreamSettings stream;
};

struct AudioPlaybackSettings {
    std::optional<bool> muted;
    std::optional<int> volumePercent;
};

struct ShareSessionConfig {
    ShareConnectionMode connectionMode = ShareConnectionMode::Room;
    std::string sessionId;
    SessionCaptureSourceType captureSourceType = SessionCaptureSourceType::Display;
    int displayIndex = 0;
    uint64_t windowHandle = 0;
    uint32_t windowProcessId = 0;
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
    bool hostVideoPaused = false;
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
    int previewLatencyMs = 40;
    bool emitVideoFrames = false;
    int audioPlaybackVolumePercent = 100;
};

// Per-input-type remote-control permission bitmask. Bit values match
// udp_protocol::ControlCapabilityFlags so they cross the wire unchanged.
enum ControlCapability : uint32_t {
    ControlCapabilityNone = 0,
    ControlCapabilityMouse = 1U << 0,
    ControlCapabilityKeyboard = 1U << 1,
    ControlCapabilityGamepad = 1U << 2, // reserved for v2 (gamepad injection)
};

enum class RemoteInputKind {
    MouseMove,
    MouseButton,
    MouseScroll,
    Key,
    RequestControl,  // viewer asks the host for control
    ReleaseControl,  // viewer gives control back
};

// A single viewer-originated remote-control event, queued by the viewer UI and
// drained by the watch runtime, which turns it into a control packet.
struct RemoteInputEvent {
    RemoteInputKind kind = RemoteInputKind::MouseMove;
    float normX = 0.0f; // [0..1] across the video frame
    float normY = 0.0f;
    int button = 0;     // mouse button id (0=left,1=right,2=middle,3=x1,4=x2)
    bool pressed = false;
    int scrollX = 0;    // wheel deltas (WHEEL_DELTA units)
    int scrollY = 0;
    int key = 0;        // virtual-key code
    int scancode = 0;
    uint32_t requestedCapabilities = 0; // for RequestControl
};

struct SessionViewer {
    std::string id;
    std::string name;
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
    bool requestingControl = false;   // viewer has an outstanding control request
    uint32_t grantedCapabilities = 0; // ControlCapability bits the host granted
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
    // Remote control. On the host: the viewer currently holding control and the
    // input types granted to it. On the viewer: the input types the host granted
    // to this client (0 = none / view-only).
    std::string controllerViewerId;
    uint32_t controlCapabilities = 0;
};

struct SessionEvent {
    SessionEventType type = SessionEventType::StateChanged;
    SessionStatus status;
    SessionIssue issue = SessionIssue::None;
    std::string message;
    // For ControlRequested (host): the requesting viewer and the input types it
    // asked for. For ControlStateChanged (viewer): the types now granted to us.
    std::string controlViewerId;
    uint32_t controlCapabilities = 0;
    struct VideoFrame {
        int width = 0;
        int height = 0;
        int codedWidth = 0;
        int codedHeight = 0;
        int64_t timestamp100ns = 0;
        int64_t duration100ns = 0;
        std::vector<std::uint8_t> nv12;
    } videoFrame;
};

class ISessionEventSink {
public:
    virtual ~ISessionEventSink() = default;
    virtual void OnSessionEvent(const SessionEvent& event) = 0;
    virtual void OnSessionVideoFrame(const SessionStatus& status, SessionEvent::VideoFrame frame)
    {
        SessionEvent event;
        event.type = SessionEventType::VideoFrameReady;
        event.status = status;
        event.videoFrame = std::move(frame);
        OnSessionEvent(event);
    }
};

const char* ToString(SessionState state);
bool IsTerminalSessionState(SessionState state);

} // namespace screenshare
