#pragma once

#include "core/SessionBackend.h"
#include "core/SessionRuntimeControl.h"

#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace screenshare {

class AppSessionBackend final : public ISessionBackend {
public:
    AppSessionBackend() = default;
    ~AppSessionBackend() override;

    AppSessionBackend(const AppSessionBackend&) = delete;
    AppSessionBackend& operator=(const AppSessionBackend&) = delete;

    void StartShare(const ShareSessionConfig& config, ISessionObserver& observer) override;
    void StartWatch(const WatchSessionConfig& config, ISessionObserver& observer) override;
    void StartArguments(
        SessionRole role,
        std::vector<std::string> arguments,
        ISessionObserver& observer,
        std::string executablePath = "ScreenShare");
    void Shutdown();
    void Stop() override;
    void ApplyStreamSettings(const StreamSettings& settings) override;

private:
    void DetachObserver();
    void JoinWorker();
    void JoinFinishedWorker();
    void Notify(SessionEventType type, SessionState state, std::string message);
    SessionStatus BuildStatusLocked() const;

    std::mutex mutex_;
    std::thread worker_;
    MemorySessionRuntimeControl runtimeControl_;
    ISessionObserver* observer_ = nullptr;
    SessionRole role_ = SessionRole::Share;
    SessionState state_ = SessionState::Idle;
    std::string summary_;
    bool stopRequested_ = false;
};

} // namespace screenshare
