#pragma once

#include "core/ScreenShareSession.h"

#include <memory>
#include <string>
#include <vector>

namespace screenshare {

class ScreenShareSession final {
public:
    ScreenShareSession();
    ~ScreenShareSession();

    ScreenShareSession(const ScreenShareSession&) = delete;
    ScreenShareSession& operator=(const ScreenShareSession&) = delete;

    void StartShare(const ShareSessionConfig& config, ISessionEventSink& eventSink);
    void StartWatch(const WatchSessionConfig& config, ISessionEventSink& eventSink);
    void Shutdown();
    void Stop();
    void ApplyStreamSettings(const StreamSettings& settings);
    SessionStatus GetStatus() const;
    std::vector<SessionDisplayInfo> ListDisplays();
    std::vector<SessionAudioDeviceInfo> ListAudioDevices();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace screenshare
