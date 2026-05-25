#pragma once

#include "core/ScreenShareSession.h"
#include "core/SessionRuntimeControl.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace screenshare {

class AppSessionBackend final : public IScreenShareSession {
public:
    AppSessionBackend() = default;
    ~AppSessionBackend() override;

    AppSessionBackend(const AppSessionBackend&) = delete;
    AppSessionBackend& operator=(const AppSessionBackend&) = delete;

    void StartShare(const ShareSessionConfig& config, ISessionEventSink& eventSink) override;
    void StartShare(const ShareSessionConfig& config, ISessionEventSink& eventSink, std::string executablePath);
    void StartWatch(const WatchSessionConfig& config, ISessionEventSink& eventSink) override;
    void StartWatch(const WatchSessionConfig& config, ISessionEventSink& eventSink, std::string executablePath);
    void StartArguments(
        SessionRole role,
        std::vector<std::string> arguments,
        ISessionEventSink& eventSink,
        std::string executablePath = "ScreenShare");
    void Shutdown();
    void Stop() override;
    void ApplyStreamSettings(const StreamSettings& settings) override;
    SessionStatus GetStatus() const override;
    std::vector<SessionDisplayInfo> ListDisplays() override;
    std::vector<SessionAudioDeviceInfo> ListAudioDevices() override;

private:
    void DetachObserver();
    void JoinWorker();
    void JoinFinishedWorker();
    void HandleOutput(std::string_view text);
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
    bool stopRequested_ = false;
};

} // namespace screenshare
