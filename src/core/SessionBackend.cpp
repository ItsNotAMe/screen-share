#include "core/SessionBackend.h"

namespace screenshare {

const char* ToString(SessionState state)
{
    switch (state) {
    case SessionState::Idle:
        return "idle";
    case SessionState::Starting:
        return "starting";
    case SessionState::Connecting:
        return "connecting";
    case SessionState::Live:
        return "live";
    case SessionState::Recovering:
        return "recovering";
    case SessionState::Disconnected:
        return "disconnected";
    case SessionState::Stopping:
        return "stopping";
    case SessionState::Stopped:
        return "stopped";
    case SessionState::Failed:
        return "failed";
    }
    return "unknown";
}

bool IsTerminalSessionState(SessionState state)
{
    return state == SessionState::Stopped || state == SessionState::Failed;
}

} // namespace screenshare
