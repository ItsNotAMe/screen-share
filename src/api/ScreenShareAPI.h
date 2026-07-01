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
    void ApplyShareSettings(const ShareSessionSettings& settings);
    void ApplyAudioPlaybackSettings(const AudioPlaybackSettings& settings);

    // Remote control. Viewer side: queue an input event, or ask the host for /
    // release control. Host side: set the exact input types one viewer may drive
    // (capabilities is a ControlCapability bitmask; 0 revokes).
    void SendRemoteInput(const RemoteInputEvent& event);
    void RequestControl(uint32_t capabilities);
    void ReleaseControl();
    void SetViewerControl(const std::string& viewerId, uint32_t capabilities);

    SessionStatus GetStatus() const;
    std::vector<SessionDisplayInfo> ListDisplays();
    std::vector<SessionWindowInfo> ListWindows();
    std::vector<SessionAudioDeviceInfo> ListAudioDevices();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace screenshare
