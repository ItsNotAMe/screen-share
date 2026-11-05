#pragma once
#include <cstdint>
#include <optional>

namespace screenshare::media {
// Host intent/capture condition, not a remote decode or presentation acknowledgement.
enum class HostShareState : uint8_t { Live, VideoPaused, SourceUnavailable, SourceMinimized, Recovering };
inline const char* HostShareStateName(HostShareState state) {
    switch (state) {
    case HostShareState::Live: return "live";
    case HostShareState::VideoPaused: return "video_paused";
    case HostShareState::SourceUnavailable: return "source_unavailable";
    case HostShareState::SourceMinimized: return "source_minimized";
    case HostShareState::Recovering: return "recovering";
    }
    return "unknown";
}
struct HostShareStatus {
    std::optional<HostShareState> state;
    bool stale = false;
    std::optional<uint32_t> ageSeconds;
};
}
