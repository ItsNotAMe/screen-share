#include "api/ScreenShareAPI.h"

#include "runtime/ScreenShareSessionRunner.h"
#include "audio/WasapiCapture.h"
#include "capture/DesktopCapturer.h"
#include "core/SessionRuntimeControl.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace screenshare {
namespace {

void ApplyStreamSettingsToRuntimeRequest(
    RuntimeStreamSettingsRequest& request,
    const StreamSettings& settings)
{
    RuntimeResolutionRequest resolution;
    if (settings.outputResolution &&
        settings.outputResolution->width > 0 &&
        settings.outputResolution->height > 0) {
        resolution.mode = RuntimeResolutionMode::Fixed;
        resolution.width = settings.outputResolution->width;
        resolution.height = settings.outputResolution->height;
        request.resolution = resolution;
    } else if (!settings.adaptResolution) {
        resolution.mode = RuntimeResolutionMode::Native;
        request.resolution = resolution;
    } else {
        resolution.mode = RuntimeResolutionMode::Auto;
        request.resolution = resolution;
    }
    if (settings.fps > 0) {
        request.fps = settings.fps;
    }
    if (settings.bitrateBps > 0) {
        request.bitrateBps = settings.bitrateBps;
    }
    request.adaptBitrate = settings.adaptBitrate;
    request.adaptResolution = settings.adaptResolution;
    request.adaptFps = settings.adaptFps;
}

RuntimeStreamSettingsRequest BuildRuntimeStreamSettingsRequest(const StreamSettings& settings)
{
    RuntimeStreamSettingsRequest request;
    ApplyStreamSettingsToRuntimeRequest(request, settings);
    return request;
}

RuntimeStreamSettingsRequest BuildRuntimeStreamSettingsRequest(const ShareSessionSettings& settings)
{
    RuntimeStreamSettingsRequest request;
    if (settings.roomName) {
        request.roomName = *settings.roomName;
    }
    if (settings.captureSourceType) {
        request.captureSourceType = *settings.captureSourceType == SessionCaptureSourceType::Window
            ? RuntimeCaptureSourceType::Window
            : RuntimeCaptureSourceType::Display;
    }
    if (settings.displayIndex && *settings.displayIndex >= 0) {
        request.displayIndex = *settings.displayIndex;
    }
    if (settings.windowHandle && *settings.windowHandle != 0) {
        request.windowHandle = *settings.windowHandle;
    }
    if (settings.windowProcessId && *settings.windowProcessId != 0) {
        request.windowProcessId = *settings.windowProcessId;
    }
    if (settings.captureSystemAudio) {
        request.captureSystemAudio = *settings.captureSystemAudio;
    }
    if (settings.hostAudioMuted) {
        request.hostAudioMuted = *settings.hostAudioMuted;
    }
    if (settings.hostVideoPaused) {
        request.videoPaused = *settings.hostVideoPaused;
    }
    if (settings.audioDeviceId) {
        request.audioDeviceId = *settings.audioDeviceId;
    }
    ApplyStreamSettingsToRuntimeRequest(request, settings.stream);
    request.bitrateBps = settings.stream.bitrateBps;
    return request;
}

RuntimeAudioPlaybackSettingsRequest BuildRuntimeAudioPlaybackSettingsRequest(const AudioPlaybackSettings& settings)
{
    RuntimeAudioPlaybackSettingsRequest request;
    request.muted = settings.muted;
    if (settings.volumePercent) {
        request.volumePercent = std::clamp(*settings.volumePercent, 0, 200);
    }
    return request;
}

SessionEvent BuildErrorEvent(SessionRole role, SessionState state, std::string message)
{
    SessionEvent event;
    event.type = SessionEventType::Error;
    event.status.role = role;
    event.status.state = state;
    event.status.summary = message;
    event.message = std::move(message);
    return event;
}

std::string LogFieldValue(std::string_view text, std::string_view field)
{
    std::string needle(field);
    needle += '=';

    size_t searchFrom = 0;
    while (searchFrom < text.size()) {
        const size_t position = text.find(needle, searchFrom);
        if (position == std::string_view::npos) {
            return {};
        }
        if (position == 0 || std::isspace(static_cast<unsigned char>(text[position - 1])) != 0) {
            const size_t valueStart = position + needle.size();
            size_t valueEnd = valueStart;
            while (valueEnd < text.size() &&
                std::isspace(static_cast<unsigned char>(text[valueEnd])) == 0) {
                ++valueEnd;
            }
            return std::string(text.substr(valueStart, valueEnd - valueStart));
        }
        searchFrom = position + 1;
    }
    return {};
}

int HexValue(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    return -1;
}

std::string LogTokenDecode(std::string_view text)
{
    if (text.empty() || text == "none") {
        return {};
    }

    std::string decoded;
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '%' && index + 2 < text.size()) {
            const int high = HexValue(text[index + 1]);
            const int low = HexValue(text[index + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        decoded.push_back(static_cast<char>(text[index]));
    }
    return decoded;
}

std::string LowercaseCopy(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

bool ContainsNoCase(std::string_view text, std::string_view needle)
{
    return LowercaseCopy(text).find(LowercaseCopy(needle)) != std::string::npos;
}

SessionAudioDeviceSource ToSessionAudioDeviceSource(AudioCaptureSource source)
{
    switch (source) {
    case AudioCaptureSource::SystemOutput:
    case AudioCaptureSource::ProcessOutput:
        return SessionAudioDeviceSource::SystemOutput;
    case AudioCaptureSource::Microphone:
        return SessionAudioDeviceSource::Microphone;
    }
    return SessionAudioDeviceSource::SystemOutput;
}

std::optional<uint64_t> ParseUint64(const std::string& text)
{
    if (text.empty()) {
        return std::nullopt;
    }
    try {
        size_t parsed = 0;
        const auto value = std::stoull(text, &parsed, 10);
        if (parsed != text.size()) {
            return std::nullopt;
        }
        return static_cast<uint64_t>(value);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int> ParseInt(const std::string& text)
{
    if (text.empty()) {
        return std::nullopt;
    }
    try {
        size_t parsed = 0;
        const int value = std::stoi(text, &parsed, 10);
        if (parsed != text.size()) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> ParseDouble(const std::string& text)
{
    if (text.empty()) {
        return std::nullopt;
    }
    try {
        size_t parsed = 0;
        const double value = std::stod(text, &parsed);
        if (parsed != text.size()) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> ParseYesNo(const std::string& text)
{
    if (text == "yes") {
        return true;
    }
    if (text == "no") {
        return false;
    }
    return std::nullopt;
}

std::optional<SessionResolution> ParseResolution(const std::string& text)
{
    const size_t separator = text.find('x');
    if (separator == std::string::npos) {
        return std::nullopt;
    }
    const auto width = ParseInt(text.substr(0, separator));
    const auto height = ParseInt(text.substr(separator + 1));
    if (!width || !height || *width <= 0 || *height <= 0) {
        return std::nullopt;
    }
    return SessionResolution{*width, *height};
}

bool CounterAdvanced(uint64_t current, uint64_t previous)
{
    return current > previous || (current < previous && current > 0);
}

SessionState ViewerStateFromLog(const std::string& state, bool hasFeedback, bool activeNow)
{
    if (state == "failed") {
        return SessionState::Failed;
    }
    if (activeNow) {
        return SessionState::Live;
    }
    if (hasFeedback) {
        return SessionState::Disconnected;
    }
    return SessionState::Connecting;
}

bool LogFieldPositive(const std::string& line, std::string_view field)
{
    const auto value = ParseUint64(LogFieldValue(line, field));
    return value && *value > 0;
}

SessionIssue DetectSessionIssue(const std::string& line)
{
    if (ContainsNoCase(line, "requires --access-code CODE") ||
        ContainsNoCase(line, "requires an access code") ||
        ContainsNoCase(line, "requires a password") ||
        ContainsNoCase(line, "--access-code CODE or --allow-plaintext")) {
        return SessionIssue::AccessCodeRequired;
    }

    if (ContainsNoCase(line, "could not be decrypted") ||
        ContainsNoCase(line, "invalid_room_password") ||
        ContainsNoCase(line, "room password is incorrect") ||
        ContainsNoCase(line, "access code does not match") ||
        ContainsNoCase(line, "does not match the peer invite fingerprint") ||
        ContainsNoCase(line, "does not match the local invite fingerprint") ||
        LogFieldPositive(line, "access_rejected_datagrams") ||
        LogFieldPositive(line, "crypto_rejected_datagrams") ||
        LogFieldPositive(line, "udp_feedback_access_rejected") ||
        LogFieldPositive(line, "udp_feedback_crypto_rejected")) {
        return SessionIssue::AccessCodeMismatch;
    }

    return SessionIssue::None;
}

} // namespace

class ScreenShareSession::Impl final {
public:
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    Impl() = default;

    void StartShare(const ShareSessionConfig& config, ISessionEventSink& eventSink);
    void StartWatch(const WatchSessionConfig& config, ISessionEventSink& eventSink);
    void Shutdown();
    void Stop();
    void ApplyStreamSettings(const StreamSettings& settings);
    void ApplyShareSettings(const ShareSessionSettings& settings);
    void ApplyAudioPlaybackSettings(const AudioPlaybackSettings& settings);
    SessionStatus GetStatus() const;
    std::vector<SessionDisplayInfo> ListDisplays();
    std::vector<SessionWindowInfo> ListWindows();
    std::vector<SessionAudioDeviceInfo> ListAudioDevices();

private:
    using SessionRunner = std::function<int(const ::ScreenShareRunContext&)>;

    void StartRunner(SessionRole role, ISessionEventSink& eventSink, SessionRunner runner);
    void DetachObserver();
    void JoinWorker();
    void JoinFinishedWorker();
    void HandleOutput(std::string_view text);
    void HandleVideoFrame(SessionEvent::VideoFrame frame);
    void HandleLogLine(const std::string& line);
    void HandleDiagnosticLogLine(const std::string& line);
    void HandleStreamLogLine(const std::string& line);
    void HandleAudioLogLine(const std::string& line);
    void HandleShareLogLine(const std::string& line);
    void HandleWatchLogLine(const std::string& line);
    void EmitStatus(SessionEventType type, std::string message);
    void EmitIssue(SessionIssue issue, std::string message);
    void Notify(SessionEventType type, SessionState state, std::string message);
    SessionStatus BuildStatusLocked() const;

    mutable std::mutex mutex_;
    std::thread worker_;
    MemorySessionRuntimeControl runtimeControl_;
    ISessionEventSink* observer_ = nullptr;
    SessionRole role_ = SessionRole::Share;
    SessionState state_ = SessionState::Idle;
    std::string summary_;
    std::string health_;
    std::string logParseBuffer_;
    SessionStreamStatus streamStatus_;
    SessionAudioStatus audioStatus_;
    std::vector<SessionViewer> viewers_;
    uint64_t watchCompletedFrames_ = 0;
    uint64_t watchPayloadBytes_ = 0;
    uint64_t watchDecodedFrames_ = 0;
    uint64_t watchAudioPackets_ = 0;
    std::string natStatus_;
    std::string natHint_;
    bool previewClosedNotified_ = false;
    bool hostLeftNotified_ = false;
    bool stopRequested_ = false;
};

ScreenShareSession::Impl::~Impl()
{
    Shutdown();
}

void ScreenShareSession::Impl::StartShare(const ShareSessionConfig& config, ISessionEventSink& eventSink)
{
    StartRunner(
        SessionRole::Share,
        eventSink,
        [config](const ScreenShareRunContext& context) {
            return RunShareSession(config, context);
        });
}

void ScreenShareSession::Impl::StartWatch(const WatchSessionConfig& config, ISessionEventSink& eventSink)
{
    StartRunner(
        SessionRole::Watch,
        eventSink,
        [config](const ScreenShareRunContext& context) {
            return RunWatchSession(config, context);
        });
}

void ScreenShareSession::Impl::Stop()
{
    bool shouldNotify = false;
    {
        std::scoped_lock lock(mutex_);
        if (state_ == SessionState::Idle || IsTerminalSessionState(state_)) {
            return;
        }
        state_ = SessionState::Stopping;
        summary_ = "Stopping session";
        stopRequested_ = true;
        shouldNotify = true;
    }

    runtimeControl_.RequestStop();
    if (shouldNotify) {
        Notify(SessionEventType::StateChanged, SessionState::Stopping, "Stopping session");
    }
}

void ScreenShareSession::Impl::Shutdown()
{
    DetachObserver();
    Stop();
    JoinWorker();
}

void ScreenShareSession::Impl::ApplyStreamSettings(const StreamSettings& settings)
{
    runtimeControl_.RequestStreamSettings(BuildRuntimeStreamSettingsRequest(settings));

    SessionState currentState = SessionState::Idle;
    {
        std::scoped_lock lock(mutex_);
        currentState = state_;
    }
    Notify(SessionEventType::SettingsChanged, currentState, "Stream settings queued");
}

void ScreenShareSession::Impl::ApplyShareSettings(const ShareSessionSettings& settings)
{
    runtimeControl_.RequestStreamSettings(BuildRuntimeStreamSettingsRequest(settings));

    SessionState currentState = SessionState::Idle;
    {
        std::scoped_lock lock(mutex_);
        currentState = state_;
    }
    Notify(SessionEventType::SettingsChanged, currentState, "Share settings queued");
}

void ScreenShareSession::Impl::ApplyAudioPlaybackSettings(const AudioPlaybackSettings& settings)
{
    runtimeControl_.RequestAudioPlaybackSettings(BuildRuntimeAudioPlaybackSettingsRequest(settings));

    SessionState currentState = SessionState::Idle;
    {
        std::scoped_lock lock(mutex_);
        currentState = state_;
    }
    Notify(SessionEventType::AudioStatusChanged, currentState, "Audio playback settings queued");
}

SessionStatus ScreenShareSession::Impl::GetStatus() const
{
    std::scoped_lock lock(mutex_);
    return BuildStatusLocked();
}

std::vector<SessionDisplayInfo> ScreenShareSession::Impl::ListDisplays()
{
    const auto displays = DesktopCapturer::EnumerateDisplays();

    std::vector<SessionDisplayInfo> result;
    result.reserve(displays.size());
    for (const auto& display : displays) {
        SessionDisplayInfo info;
        info.index = display.index;
        info.outputName = Narrow(display.outputName);
        info.width = static_cast<int>(display.right - display.left);
        info.height = static_cast<int>(display.bottom - display.top);
        info.left = static_cast<int>(display.left);
        info.top = static_cast<int>(display.top);
        info.adapterName = Narrow(display.adapterName);
        info.attached = display.attachedToDesktop;
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<SessionWindowInfo> ScreenShareSession::Impl::ListWindows()
{
    const auto windows = DesktopCapturer::EnumerateWindows();

    std::vector<SessionWindowInfo> result;
    result.reserve(windows.size());
    for (const auto& window : windows) {
        SessionWindowInfo info;
        info.handle = window.handle;
        info.processId = window.processId;
        info.title = Narrow(window.title);
        info.processName = Narrow(window.processName);
        info.width = static_cast<int>(window.right - window.left);
        info.height = static_cast<int>(window.bottom - window.top);
        info.left = static_cast<int>(window.left);
        info.top = static_cast<int>(window.top);
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<SessionAudioDeviceInfo> ScreenShareSession::Impl::ListAudioDevices()
{
    std::vector<SessionAudioDeviceInfo> result;
    for (const AudioCaptureSource source : {AudioCaptureSource::SystemOutput, AudioCaptureSource::Microphone}) {
        const auto devices = WasapiCapture::EnumerateDevices(source);
        result.reserve(result.size() + devices.size());
        for (const auto& device : devices) {
            SessionAudioDeviceInfo info;
            info.source = ToSessionAudioDeviceSource(device.source);
            info.id = Narrow(device.id);
            info.name = Narrow(device.name);
            info.isDefault = device.isDefault;
            result.push_back(std::move(info));
        }
    }
    return result;
}

void ScreenShareSession::Impl::StartRunner(
    SessionRole role,
    ISessionEventSink& eventSink,
    SessionRunner runner)
{
    JoinFinishedWorker();

    bool alreadyRunning = false;
    SessionState currentState = SessionState::Idle;
    {
        std::scoped_lock lock(mutex_);
        if (worker_.joinable() && !IsTerminalSessionState(state_)) {
            alreadyRunning = true;
            currentState = state_;
        } else {
            observer_ = &eventSink;
            role_ = role;
            state_ = SessionState::Starting;
            summary_ = "Starting session";
            health_.clear();
            logParseBuffer_.clear();
            streamStatus_ = SessionStreamStatus{};
            audioStatus_ = SessionAudioStatus{};
            viewers_.clear();
            watchCompletedFrames_ = 0;
            watchPayloadBytes_ = 0;
            watchDecodedFrames_ = 0;
            watchAudioPackets_ = 0;
            natStatus_.clear();
            natHint_.clear();
            previewClosedNotified_ = false;
            hostLeftNotified_ = false;
            stopRequested_ = false;
            runtimeControl_.Reset();
        }
    }

    if (alreadyRunning) {
        eventSink.OnSessionEvent(BuildErrorEvent(role, currentState, "A session is already running."));
        return;
    }

    Notify(SessionEventType::StateChanged, SessionState::Starting, "Starting session");

    worker_ = std::thread([this, runner = std::move(runner)]() mutable {
        Notify(SessionEventType::StateChanged, SessionState::Connecting, "Connecting session");

        ScreenShareRunContext context;
        context.runtimeControl = &runtimeControl_;
        context.outputHandler = [this](std::string_view text) {
            HandleOutput(text);
        };
        context.videoFrameHandler = [this](SessionEvent::VideoFrame frame) {
            HandleVideoFrame(std::move(frame));
        };

        int exitCode = 1;
        try {
            exitCode = runner(context);
        } catch (const std::exception& error) {
            Notify(SessionEventType::Error, SessionState::Failed, error.what());
            return;
        } catch (...) {
            Notify(SessionEventType::Error, SessionState::Failed, "Session failed with an unknown error.");
            return;
        }

        bool stopRequested = false;
        {
            std::scoped_lock lock(mutex_);
            stopRequested = stopRequested_;
        }

        if (exitCode == 0 || stopRequested) {
            Notify(SessionEventType::StateChanged, SessionState::Stopped, "Session stopped");
        } else {
            Notify(
                SessionEventType::Error,
                SessionState::Failed,
                "Session failed with exit code " + std::to_string(exitCode));
        }
    });
}

void ScreenShareSession::Impl::DetachObserver()
{
    std::scoped_lock lock(mutex_);
    observer_ = nullptr;
}

void ScreenShareSession::Impl::JoinWorker()
{
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

void ScreenShareSession::Impl::JoinFinishedWorker()
{
    bool shouldJoin = false;
    {
        std::scoped_lock lock(mutex_);
        shouldJoin = worker_.joinable() && IsTerminalSessionState(state_);
    }
    if (shouldJoin) {
        JoinWorker();
    }
}

void ScreenShareSession::Impl::HandleOutput(std::string_view text)
{
    Notify(SessionEventType::LogLine, SessionState::Idle, std::string(text));

    std::vector<std::string> lines;
    {
        std::scoped_lock lock(mutex_);
        logParseBuffer_.append(text.data(), text.size());
        constexpr size_t MaxParseBuffer = 32768;
        if (logParseBuffer_.size() > MaxParseBuffer) {
            logParseBuffer_.erase(0, logParseBuffer_.size() - MaxParseBuffer);
        }

        size_t lineStart = 0;
        while (lineStart < logParseBuffer_.size()) {
            const size_t lineEnd = logParseBuffer_.find_first_of("\r\n", lineStart);
            if (lineEnd == std::string::npos) {
                break;
            }
            if (lineEnd > lineStart) {
                lines.push_back(logParseBuffer_.substr(lineStart, lineEnd - lineStart));
            }
            lineStart = lineEnd + 1;
            while (lineStart < logParseBuffer_.size() &&
                (logParseBuffer_[lineStart] == '\r' || logParseBuffer_[lineStart] == '\n')) {
                ++lineStart;
            }
        }
        logParseBuffer_.erase(0, lineStart);
    }

    for (const std::string& line : lines) {
        HandleLogLine(line);
    }
}

void ScreenShareSession::Impl::HandleVideoFrame(SessionEvent::VideoFrame frame)
{
    ISessionEventSink* observer = nullptr;
    SessionStatus status;
    {
        std::scoped_lock lock(mutex_);
        observer = observer_;
        status = BuildStatusLocked();
    }

    if (observer != nullptr) {
        observer->OnSessionVideoFrame(status, std::move(frame));
    }
}

void ScreenShareSession::Impl::HandleLogLine(const std::string& line)
{
    HandleDiagnosticLogLine(line);
    HandleStreamLogLine(line);
    HandleAudioLogLine(line);

    SessionRole role = SessionRole::Share;
    {
        std::scoped_lock lock(mutex_);
        role = role_;
    }
    if (role == SessionRole::Share) {
        HandleShareLogLine(line);
    } else {
        HandleWatchLogLine(line);
    }
}

void ScreenShareSession::Impl::HandleStreamLogLine(const std::string& line)
{
    const auto sourceResolution = ParseResolution(LogFieldValue(line, "source"));
    const auto outputResolution = ParseResolution(LogFieldValue(line, "output"));
    const auto decodedOutputResolution = ParseResolution(LogFieldValue(line, "h264_decoded_output"));
    const auto outputFps = ParseDouble(LogFieldValue(line, "output_fps"));
    const auto completedFps = ParseDouble(LogFieldValue(line, "completed_fps"));
    const auto desktopUpdateFps = ParseDouble(LogFieldValue(line, "desktop_update_fps"));
    const auto bitrateMbps = ParseDouble(LogFieldValue(line, "stream_bitrate_mbps"));
    const auto resolutionScale = ParseDouble(LogFieldValue(line, "resolution_scale"));
    const auto totalOutputFrames = ParseUint64(LogFieldValue(line, "total_output_frames"));
    const auto streamQueueDepth = ParseUint64(LogFieldValue(line, "stream_queue"));
    const auto streamDroppedFrames = ParseUint64(LogFieldValue(line, "stream_dropped"));
    const auto udpQueueMs = ParseUint64(LogFieldValue(line, "udp_queue_ms"));
    const std::string resolutionAdaptation = LogFieldValue(line, "resolution_adaptation");
    const auto resolutionAdaptations = ParseUint64(LogFieldValue(line, "resolution_adaptations"));
    const std::string bitrateAdaptation = LogFieldValue(line, "bitrate_adaptation");
    const auto bitrateAdaptations = ParseUint64(LogFieldValue(line, "bitrate_adaptations"));

    const bool hasStreamFields =
        sourceResolution ||
        outputResolution ||
        decodedOutputResolution ||
        outputFps ||
        completedFps ||
        desktopUpdateFps ||
        bitrateMbps ||
        resolutionScale ||
        totalOutputFrames ||
        streamQueueDepth ||
        streamDroppedFrames ||
        udpQueueMs ||
        !resolutionAdaptation.empty() ||
        resolutionAdaptations ||
        !bitrateAdaptation.empty() ||
        bitrateAdaptations;
    if (!hasStreamFields) {
        return;
    }

    {
        std::scoped_lock lock(mutex_);
        streamStatus_.hasStats = true;
        if (sourceResolution) {
            streamStatus_.sourceResolution = *sourceResolution;
        }
        if (outputResolution) {
            streamStatus_.outputResolution = *outputResolution;
        } else if (decodedOutputResolution) {
            streamStatus_.outputResolution = *decodedOutputResolution;
        }
        if (outputFps) {
            streamStatus_.outputFps = *outputFps;
        } else if (completedFps) {
            streamStatus_.outputFps = *completedFps;
        }
        if (desktopUpdateFps) {
            streamStatus_.desktopUpdateFps = *desktopUpdateFps;
        }
        if (bitrateMbps) {
            streamStatus_.bitrateMbps = *bitrateMbps;
        }
        if (resolutionScale) {
            streamStatus_.resolutionScale = *resolutionScale;
        }
        if (totalOutputFrames) {
            streamStatus_.totalOutputFrames = *totalOutputFrames;
        }
        if (streamQueueDepth) {
            streamStatus_.streamQueueDepth = *streamQueueDepth;
        }
        if (streamDroppedFrames) {
            streamStatus_.streamDroppedFrames = *streamDroppedFrames;
        }
        if (udpQueueMs) {
            streamStatus_.udpQueueMs = *udpQueueMs;
        }
        if (!resolutionAdaptation.empty()) {
            streamStatus_.resolutionAdaptation = resolutionAdaptation;
        }
        if (resolutionAdaptations) {
            streamStatus_.resolutionAdaptations = *resolutionAdaptations;
        }
        if (!bitrateAdaptation.empty()) {
            streamStatus_.bitrateAdaptation = bitrateAdaptation;
        }
        if (bitrateAdaptations) {
            streamStatus_.bitrateAdaptations = *bitrateAdaptations;
        }
    }

    EmitStatus(SessionEventType::StreamStatusChanged, "Stream status updated");
}

void ScreenShareSession::Impl::HandleAudioLogLine(const std::string& line)
{
    const std::string codec = LogFieldValue(line, "audio_codec");
    const std::string format = LogFieldValue(line, "audio_format");
    const std::string captureState = LogFieldValue(line, "audio_capture");
    const auto capturePackets = ParseUint64(LogFieldValue(line, "audio_capture_packets"));
    const auto captureFrames = ParseUint64(LogFieldValue(line, "audio_capture_frames"));
    const auto captureBytes = ParseUint64(LogFieldValue(line, "audio_capture_bytes"));
    const auto captureEmptyPolls = ParseUint64(LogFieldValue(line, "audio_capture_empty_polls"));
    const auto captureDiscontinuities = ParseUint64(LogFieldValue(line, "audio_capture_discontinuities"));
    const auto captureTimestampErrors = ParseUint64(LogFieldValue(line, "audio_capture_timestamp_errors"));
    const auto capturePayloadBitrateMbps = ParseDouble(LogFieldValue(line, "audio_payload_bitrate_mbps"));
    const auto standalonePackets = ParseUint64(LogFieldValue(line, "audio_packets"));
    const auto standaloneFrames = ParseUint64(LogFieldValue(line, "audio_frames"));
    const auto standaloneBytes = ParseUint64(LogFieldValue(line, "audio_bytes"));
    const auto packetsPerSecond = ParseDouble(LogFieldValue(line, "audio_packets_per_second"));
    const auto framesPerSecond = ParseDouble(LogFieldValue(line, "audio_frames_per_second"));
    const auto silentPackets = ParseUint64(LogFieldValue(line, "audio_silent_packets"));
    const auto standaloneSilentPackets = ParseUint64(LogFieldValue(line, "silent_packets"));
    const auto discontinuities = ParseUint64(LogFieldValue(line, "audio_discontinuities"));
    const auto standaloneDiscontinuities = ParseUint64(LogFieldValue(line, "discontinuities"));
    const auto timestampErrors = ParseUint64(LogFieldValue(line, "audio_timestamp_errors"));
    const auto standaloneTimestampErrors = ParseUint64(LogFieldValue(line, "timestamp_errors"));
    const bool standaloneCaptureStats =
        packetsPerSecond ||
        framesPerSecond ||
        !LogFieldValue(line, "peak").empty() ||
        !LogFieldValue(line, "rms").empty();
    const auto udpPackets = ParseUint64(LogFieldValue(line, "audio_udp_packets"));
    const auto udpFrames = ParseUint64(LogFieldValue(line, "audio_udp_frames"));
    const auto udpDatagrams = ParseUint64(LogFieldValue(line, "audio_udp_datagrams"));
    const auto udpQueuedDatagrams = ParseUint64(LogFieldValue(line, "audio_udp_queued_datagrams"));
    const auto udpPendingDatagrams = ParseUint64(LogFieldValue(line, "audio_udp_pending"));
    const auto udpDroppedPackets = ParseUint64(LogFieldValue(line, "audio_udp_dropped_packets"));
    const auto udpWireBytes = ParseUint64(LogFieldValue(line, "audio_udp_wire_bytes"));
    const std::string playbackState = LogFieldValue(line, "audio_playback");
    const auto playbackMuted = ParseYesNo(LogFieldValue(line, "audio_playback_muted"));
    const auto playbackVolumePercent = ParseInt(LogFieldValue(line, "audio_playback_volume_percent"));
    const auto playbackLatencyMs = ParseInt(LogFieldValue(line, "audio_playback_latency_ms"));
    const auto playbackQueuePackets = ParseUint64(LogFieldValue(line, "audio_playback_queue"));
    const auto playbackQueueMs = ParseUint64(LogFieldValue(line, "audio_playback_queue_ms"));
    const auto playbackPackets = ParseUint64(LogFieldValue(line, "audio_playback_packets"));
    const auto playbackFrames = ParseUint64(LogFieldValue(line, "audio_playback_frames"));
    const auto playbackDrops = ParseUint64(LogFieldValue(line, "audio_playback_drops"));
    const auto playbackLatencyDrops = ParseUint64(LogFieldValue(line, "audio_playback_latency_drops"));
    const auto playbackSyncDrops = ParseUint64(LogFieldValue(line, "audio_playback_sync_drops"));
    const auto playbackSyncWaits = ParseUint64(LogFieldValue(line, "audio_playback_sync_waits"));
    const auto playbackMissingPackets = ParseUint64(LogFieldValue(line, "audio_playback_missing"));
    const auto playbackBackpressure = ParseUint64(LogFieldValue(line, "audio_playback_backpressure"));
    const auto renderBufferFullEvents = ParseUint64(LogFieldValue(line, "audio_render_buffer_full"));
    const auto renderPaddingFrames = ParseUint64(LogFieldValue(line, "audio_render_padding"));
    const std::string avSyncState = LogFieldValue(line, "av_sync");
    const std::string avSyncCorrection = LogFieldValue(line, "av_sync_correction");
    const auto avAudioAheadMs = ParseInt(LogFieldValue(line, "av_audio_ahead_ms"));
    const auto avAudioElapsedMs = ParseInt(LogFieldValue(line, "av_audio_elapsed_ms"));
    const auto avVideoElapsedMs = ParseInt(LogFieldValue(line, "av_video_elapsed_ms"));
    const auto avPlayoutAudioAheadMs = ParseInt(LogFieldValue(line, "av_playout_audio_ahead_ms"));
    const auto avAudioPackets = ParseUint64(LogFieldValue(line, "av_audio_packets"));
    const auto avIgnoredAudioPackets = ParseUint64(LogFieldValue(line, "av_ignored_audio_packets"));
    const auto avVideoFrames = ParseUint64(LogFieldValue(line, "av_video_frames"));

    const bool hasAudioFields =
        !codec.empty() ||
        !format.empty() ||
        !captureState.empty() ||
        capturePackets ||
        captureFrames ||
        captureBytes ||
        captureEmptyPolls ||
        captureDiscontinuities ||
        captureTimestampErrors ||
        capturePayloadBitrateMbps ||
        standalonePackets ||
        standaloneFrames ||
        standaloneBytes ||
        packetsPerSecond ||
        framesPerSecond ||
        silentPackets ||
        standaloneSilentPackets ||
        discontinuities ||
        standaloneDiscontinuities ||
        timestampErrors ||
        standaloneTimestampErrors ||
        udpPackets ||
        udpFrames ||
        udpDatagrams ||
        udpQueuedDatagrams ||
        udpPendingDatagrams ||
        udpDroppedPackets ||
        udpWireBytes ||
        !playbackState.empty() ||
        playbackMuted ||
        playbackVolumePercent ||
        playbackLatencyMs ||
        playbackQueuePackets ||
        playbackQueueMs ||
        playbackPackets ||
        playbackFrames ||
        playbackDrops ||
        playbackLatencyDrops ||
        playbackSyncDrops ||
        playbackSyncWaits ||
        playbackMissingPackets ||
        playbackBackpressure ||
        renderBufferFullEvents ||
        renderPaddingFrames ||
        !avSyncState.empty() ||
        !avSyncCorrection.empty() ||
        avAudioAheadMs ||
        avAudioElapsedMs ||
        avVideoElapsedMs ||
        avPlayoutAudioAheadMs ||
        avAudioPackets ||
        avIgnoredAudioPackets ||
        avVideoFrames;
    if (!hasAudioFields) {
        return;
    }

    {
        std::scoped_lock lock(mutex_);
        audioStatus_.hasStats = true;
        if (!codec.empty()) {
            audioStatus_.codec = codec;
        }
        if (!format.empty()) {
            audioStatus_.format = format;
        }
        if (!captureState.empty()) {
            audioStatus_.captureState = captureState;
        }
        if (capturePackets) {
            audioStatus_.capturePackets = *capturePackets;
        } else if (standaloneCaptureStats && standalonePackets) {
            audioStatus_.capturePackets = *standalonePackets;
        }
        if (captureFrames) {
            audioStatus_.captureFrames = *captureFrames;
        } else if (standaloneCaptureStats && standaloneFrames) {
            audioStatus_.captureFrames = *standaloneFrames;
        }
        if (captureBytes) {
            audioStatus_.captureBytes = *captureBytes;
        } else if (standaloneCaptureStats && standaloneBytes) {
            audioStatus_.captureBytes = *standaloneBytes;
        }
        if (captureEmptyPolls) {
            audioStatus_.captureEmptyPolls = *captureEmptyPolls;
        }
        if (captureDiscontinuities) {
            audioStatus_.captureDiscontinuities = *captureDiscontinuities;
        }
        if (captureTimestampErrors) {
            audioStatus_.captureTimestampErrors = *captureTimestampErrors;
        }
        if (packetsPerSecond) {
            audioStatus_.capturePacketsPerSecond = *packetsPerSecond;
        }
        if (framesPerSecond) {
            audioStatus_.captureFramesPerSecond = *framesPerSecond;
        }
        if (capturePayloadBitrateMbps) {
            audioStatus_.payloadBitrateMbps = *capturePayloadBitrateMbps;
        }
        if (standalonePackets) {
            audioStatus_.packets = *standalonePackets;
        }
        if (standaloneFrames) {
            audioStatus_.frames = *standaloneFrames;
        }
        if (standaloneBytes) {
            audioStatus_.bytes = *standaloneBytes;
        }
        if (silentPackets) {
            audioStatus_.silentPackets = *silentPackets;
        } else if (standaloneSilentPackets) {
            audioStatus_.silentPackets = *standaloneSilentPackets;
        }
        if (discontinuities) {
            audioStatus_.discontinuities = *discontinuities;
        } else if (standaloneDiscontinuities) {
            audioStatus_.discontinuities = *standaloneDiscontinuities;
        }
        if (timestampErrors) {
            audioStatus_.timestampErrors = *timestampErrors;
        } else if (standaloneTimestampErrors) {
            audioStatus_.timestampErrors = *standaloneTimestampErrors;
        }
        if (udpPackets) {
            audioStatus_.udpPackets = *udpPackets;
        }
        if (udpFrames) {
            audioStatus_.udpFrames = *udpFrames;
        }
        if (udpDatagrams) {
            audioStatus_.udpDatagrams = *udpDatagrams;
        } else if (udpQueuedDatagrams) {
            audioStatus_.udpDatagrams = *udpQueuedDatagrams;
        }
        if (udpPendingDatagrams) {
            audioStatus_.udpPendingDatagrams = *udpPendingDatagrams;
        }
        if (udpDroppedPackets) {
            audioStatus_.udpDroppedPackets = *udpDroppedPackets;
        }
        if (udpWireBytes) {
            audioStatus_.udpWireBytes = *udpWireBytes;
        }
        if (!playbackState.empty()) {
            audioStatus_.playbackState = playbackState;
        }
        if (playbackMuted) {
            audioStatus_.playbackMuted = *playbackMuted;
        }
        if (playbackVolumePercent) {
            audioStatus_.playbackVolumePercent = *playbackVolumePercent;
        }
        if (playbackLatencyMs) {
            audioStatus_.playbackLatencyMs = *playbackLatencyMs;
        }
        if (playbackQueuePackets) {
            audioStatus_.playbackQueuePackets = *playbackQueuePackets;
        }
        if (playbackQueueMs) {
            audioStatus_.playbackQueueMs = *playbackQueueMs;
        }
        if (playbackPackets) {
            audioStatus_.playbackPackets = *playbackPackets;
        }
        if (playbackFrames) {
            audioStatus_.playbackFrames = *playbackFrames;
        }
        if (playbackDrops) {
            audioStatus_.playbackDrops = *playbackDrops;
        }
        if (playbackLatencyDrops) {
            audioStatus_.playbackLatencyDrops = *playbackLatencyDrops;
        }
        if (playbackSyncDrops) {
            audioStatus_.playbackSyncDrops = *playbackSyncDrops;
        }
        if (playbackSyncWaits) {
            audioStatus_.playbackSyncWaits = *playbackSyncWaits;
        }
        if (playbackMissingPackets) {
            audioStatus_.playbackMissingPackets = *playbackMissingPackets;
        }
        if (playbackBackpressure) {
            audioStatus_.playbackBackpressure = *playbackBackpressure;
        }
        if (renderBufferFullEvents) {
            audioStatus_.renderBufferFullEvents = *renderBufferFullEvents;
        }
        if (renderPaddingFrames) {
            audioStatus_.renderPaddingFrames = *renderPaddingFrames;
        }
        if (!avSyncState.empty()) {
            audioStatus_.avSyncState = avSyncState;
        }
        if (!avSyncCorrection.empty()) {
            audioStatus_.avSyncCorrection = avSyncCorrection;
        }
        if (avAudioAheadMs) {
            audioStatus_.avAudioAheadMs = *avAudioAheadMs;
        }
        if (avAudioElapsedMs) {
            audioStatus_.avAudioElapsedMs = *avAudioElapsedMs;
        }
        if (avVideoElapsedMs) {
            audioStatus_.avVideoElapsedMs = *avVideoElapsedMs;
        }
        if (avPlayoutAudioAheadMs) {
            audioStatus_.avPlayoutAudioAheadMs = *avPlayoutAudioAheadMs;
        }
        if (avAudioPackets) {
            audioStatus_.avAudioPackets = *avAudioPackets;
        }
        if (avIgnoredAudioPackets) {
            audioStatus_.avIgnoredAudioPackets = *avIgnoredAudioPackets;
        }
        if (avVideoFrames) {
            audioStatus_.avVideoFrames = *avVideoFrames;
        }
    }

    EmitStatus(SessionEventType::AudioStatusChanged, "Audio status updated");
}

void ScreenShareSession::Impl::HandleDiagnosticLogLine(const std::string& line)
{
    const std::string natStatus = LogFieldValue(line, "nat_status");
    if (!natStatus.empty()) {
        std::string natHint = LogFieldValue(line, "nat_hint");
        bool changed = false;
        {
            std::scoped_lock lock(mutex_);
            changed = natStatus_ != natStatus || (!natHint.empty() && natHint_ != natHint);
            natStatus_ = natStatus;
            if (!natHint.empty()) {
                natHint_ = std::move(natHint);
            }
        }
        if (changed) {
            EmitStatus(SessionEventType::NatStatusChanged, "NAT status updated");
        }
    }

    if (ContainsNoCase(line, "room_already_open")) {
        EmitIssue(SessionIssue::RoomAlreadyOpen, "Room already open");
    }

    if (line.find("preview_closed=stop") != std::string::npos ||
        line.find("watch_stop=preview_closed") != std::string::npos) {
        bool shouldEmit = false;
        {
            std::scoped_lock lock(mutex_);
            shouldEmit = !previewClosedNotified_;
            previewClosedNotified_ = true;
        }
        if (shouldEmit) {
            EmitIssue(SessionIssue::PreviewClosed, "Preview window closed");
        }
    }

    if (line.find("watch_host_left=") != std::string::npos) {
        bool shouldEmit = false;
        {
            std::scoped_lock lock(mutex_);
            if (!hostLeftNotified_ && !IsTerminalSessionState(state_) && state_ != SessionState::Stopping) {
                hostLeftNotified_ = true;
                state_ = SessionState::Disconnected;
                summary_ = "Host left the room";
                health_ = "host-left";
                shouldEmit = true;
            }
        }
        if (shouldEmit) {
            EmitIssue(SessionIssue::HostLeft, "Host left the room");
        }
    }

    const SessionIssue issue = DetectSessionIssue(line);
    if (issue != SessionIssue::None) {
        EmitIssue(issue, "Session security issue");
    }
}

void ScreenShareSession::Impl::HandleShareLogLine(const std::string& line)
{
    if (line.find("signaling_live_sender_peer=removed") != std::string::npos) {
        const std::string endpoint = LogFieldValue(line, "endpoint");
        if (endpoint.empty()) {
            return;
        }

        bool removed = false;
        {
            std::scoped_lock lock(mutex_);
            const auto oldSize = viewers_.size();
            viewers_.erase(
                std::remove_if(viewers_.begin(), viewers_.end(), [&](const SessionViewer& viewer) {
                    return viewer.endpoint == endpoint;
                }),
                viewers_.end());
            removed = viewers_.size() != oldSize;
            if (removed && viewers_.empty() && !IsTerminalSessionState(state_) && state_ != SessionState::Stopping) {
                state_ = SessionState::Connecting;
                summary_ = "Waiting for viewers";
            }
        }

        if (removed) {
            EmitStatus(SessionEventType::ViewerListChanged, "Viewer left");
        }
        return;
    }

    if (line.find("viewer_target=") == std::string::npos) {
        return;
    }

    const std::string endpoint = LogFieldValue(line, "viewer_endpoint");
    if (endpoint.empty()) {
        return;
    }

    const int group = ParseInt(LogFieldValue(line, "viewer_group")).value_or(-1);
    const std::string name = LogTokenDecode(LogFieldValue(line, "viewer_name"));
    const uint64_t feedbackPackets = ParseUint64(LogFieldValue(line, "viewer_feedback_packets")).value_or(0);
    const bool hasFeedback = feedbackPackets > 0;
    bool activeNow = false;

    {
        std::scoped_lock lock(mutex_);
        if (!hasFeedback && group >= 0) {
            const bool groupAlreadyConnected = std::any_of(
                viewers_.begin(),
                viewers_.end(),
                [&](const SessionViewer& viewer) {
                    return viewer.group == group && viewer.hasFeedback;
                });
            if (groupAlreadyConnected) {
                return;
            }
        }

        auto viewerIt = std::find_if(viewers_.begin(), viewers_.end(), [&](const SessionViewer& viewer) {
            return viewer.group == group && viewer.endpoint == endpoint;
        });
        if (viewerIt == viewers_.end()) {
            SessionViewer viewer;
            viewer.group = group;
            viewer.name = name;
            viewer.endpoint = endpoint;
            viewer.id = std::to_string(group) + ":" + endpoint;
            viewers_.push_back(std::move(viewer));
            viewerIt = std::prev(viewers_.end());
        }

        if (!name.empty()) {
            viewerIt->name = name;
        }
        activeNow = CounterAdvanced(feedbackPackets, viewerIt->feedbackPackets);
        viewerIt->feedbackPackets = feedbackPackets;
        viewerIt->hasFeedback = hasFeedback;
        viewerIt->activeNow = activeNow;
        viewerIt->health = LogFieldValue(line, "viewer_feedback_health");
        viewerIt->sessionFingerprint = LogFieldValue(line, "viewer_feedback_session");
        viewerIt->pendingDatagrams = ParseUint64(LogFieldValue(line, "viewer_pending")).value_or(0);
        viewerIt->queueDelayMs = ParseUint64(LogFieldValue(line, "viewer_queue_ms")).value_or(0);
        viewerIt->completedFrames = ParseUint64(LogFieldValue(line, "viewer_feedback_completed_frames")).value_or(0);
        viewerIt->decodeResyncs = ParseUint64(LogFieldValue(line, "viewer_feedback_resyncs")).value_or(0);
        viewerIt->state = ViewerStateFromLog(LogFieldValue(line, "viewer_state"), hasFeedback, activeNow);

        if (hasFeedback && group >= 0) {
            viewers_.erase(
                std::remove_if(viewers_.begin(), viewers_.end(), [&](const SessionViewer& viewer) {
                    return viewer.group == group && viewer.endpoint != endpoint && !viewer.hasFeedback;
                }),
                viewers_.end());
        }

        if (activeNow && !IsTerminalSessionState(state_) && state_ != SessionState::Stopping) {
            state_ = SessionState::Live;
            summary_ = "Viewer connected";
        }
    }

    EmitStatus(SessionEventType::ViewerListChanged, "Viewer status updated");
}

void ScreenShareSession::Impl::HandleWatchLogLine(const std::string& line)
{
    bool activeNow = false;
    bool waitingForStream = false;
    bool hostLeft = false;
    const std::string receiverHealth = LogFieldValue(line, "receiver_health");
    const auto completedFrames = ParseUint64(LogFieldValue(line, "completed_frames"));
    const auto payloadBytes = ParseUint64(LogFieldValue(line, "payload_bytes"));
    const auto decodedFrames = ParseUint64(LogFieldValue(line, "h264_decoded_frames"));
    const auto audioPackets = ParseUint64(LogFieldValue(line, "audio_packets"));

    {
        std::scoped_lock lock(mutex_);
        hostLeft = hostLeftNotified_;
        if (!hostLeft && !receiverHealth.empty()) {
            health_ = receiverHealth;
        }
        if (completedFrames) {
            activeNow = CounterAdvanced(*completedFrames, watchCompletedFrames_) || activeNow;
            watchCompletedFrames_ = *completedFrames;
        }
        if (payloadBytes) {
            activeNow = CounterAdvanced(*payloadBytes, watchPayloadBytes_) || activeNow;
            watchPayloadBytes_ = *payloadBytes;
        }
        if (decodedFrames) {
            activeNow = CounterAdvanced(*decodedFrames, watchDecodedFrames_) || activeNow;
            watchDecodedFrames_ = *decodedFrames;
        }
        if (audioPackets) {
            activeNow = CounterAdvanced(*audioPackets, watchAudioPackets_) || activeNow;
            watchAudioPackets_ = *audioPackets;
        }

        if (!hostLeft && activeNow && !IsTerminalSessionState(state_) && state_ != SessionState::Stopping) {
            state_ = SessionState::Live;
            summary_ = "Receiving stream";
        } else if (!hostLeft &&
            line.find("waiting_for_stream") != std::string::npos &&
            state_ != SessionState::Live &&
            !IsTerminalSessionState(state_) &&
            state_ != SessionState::Stopping) {
            state_ = SessionState::Connecting;
            summary_ = "Waiting for stream";
            waitingForStream = true;
        }
    }

    if (!hostLeft && (activeNow || waitingForStream)) {
        EmitStatus(SessionEventType::StateChanged, activeNow ? "Receiving stream" : "Receiver status updated");
    }
}

void ScreenShareSession::Impl::EmitStatus(SessionEventType type, std::string message)
{
    ISessionEventSink* observer = nullptr;
    SessionEvent event;
    {
        std::scoped_lock lock(mutex_);
        observer = observer_;
        event.type = type;
        event.status = BuildStatusLocked();
        event.message = std::move(message);
    }

    if (observer != nullptr) {
        observer->OnSessionEvent(event);
    }
}

void ScreenShareSession::Impl::EmitIssue(SessionIssue issue, std::string message)
{
    ISessionEventSink* observer = nullptr;
    SessionEvent event;
    {
        std::scoped_lock lock(mutex_);
        observer = observer_;
        event.type = SessionEventType::Issue;
        event.issue = issue;
        event.status = BuildStatusLocked();
        event.message = std::move(message);
    }

    if (observer != nullptr) {
        observer->OnSessionEvent(event);
    }
}

void ScreenShareSession::Impl::Notify(SessionEventType type, SessionState state, std::string message)
{
    ISessionEventSink* observer = nullptr;
    SessionEvent event;
    {
        std::scoped_lock lock(mutex_);
        if (type == SessionEventType::StateChanged || type == SessionEventType::Error) {
            state_ = state;
            summary_ = message;
        }
        observer = observer_;
        event.type = type;
        event.status = BuildStatusLocked();
        event.message = std::move(message);
    }

    if (observer != nullptr) {
        observer->OnSessionEvent(event);
    }
}

SessionStatus ScreenShareSession::Impl::BuildStatusLocked() const
{
    SessionStatus status;
    status.role = role_;
    status.state = state_;
    status.summary = summary_;
    status.stream = streamStatus_;
    status.audio = audioStatus_;
    status.videoResolution = streamStatus_.outputResolution;
    status.viewers = viewers_;
    status.health = health_;
    status.completedFrames = watchCompletedFrames_;
    status.payloadBytes = watchPayloadBytes_;
    status.decodedFrames = watchDecodedFrames_;
    status.audioPackets = watchAudioPackets_;
    status.natStatus = natStatus_;
    status.natHint = natHint_;
    return status;
}

ScreenShareSession::ScreenShareSession()
    : impl_(std::make_unique<Impl>())
{
}

ScreenShareSession::~ScreenShareSession() = default;

void ScreenShareSession::StartShare(const ShareSessionConfig& config, ISessionEventSink& eventSink)
{
    impl_->StartShare(config, eventSink);
}

void ScreenShareSession::StartWatch(const WatchSessionConfig& config, ISessionEventSink& eventSink)
{
    impl_->StartWatch(config, eventSink);
}

void ScreenShareSession::Shutdown()
{
    impl_->Shutdown();
}

void ScreenShareSession::Stop()
{
    impl_->Stop();
}

void ScreenShareSession::ApplyStreamSettings(const StreamSettings& settings)
{
    impl_->ApplyStreamSettings(settings);
}

void ScreenShareSession::ApplyShareSettings(const ShareSessionSettings& settings)
{
    impl_->ApplyShareSettings(settings);
}

void ScreenShareSession::ApplyAudioPlaybackSettings(const AudioPlaybackSettings& settings)
{
    impl_->ApplyAudioPlaybackSettings(settings);
}

SessionStatus ScreenShareSession::GetStatus() const
{
    return impl_->GetStatus();
}

std::vector<SessionDisplayInfo> ScreenShareSession::ListDisplays()
{
    return impl_->ListDisplays();
}

std::vector<SessionWindowInfo> ScreenShareSession::ListWindows()
{
    return impl_->ListWindows();
}

std::vector<SessionAudioDeviceInfo> ScreenShareSession::ListAudioDevices()
{
    return impl_->ListAudioDevices();
}

} // namespace screenshare
