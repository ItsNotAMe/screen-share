#pragma once
#include "AudioSelection.h"
namespace screenshare::media {
struct PlaybackSelection { std::wstring deviceId; unsigned volume = 100; bool muted = false; };
struct PlaybackStatus { PlaybackSelection selected; uint64_t revision = 1; AudioEndpointHealth health; };
inline void ValidatePlaybackSelection(const PlaybackSelection& value) {
    if (value.volume > 100 || value.deviceId.size() > 4096 || value.deviceId.find(L'\0') != std::wstring::npos)
        throw std::invalid_argument("Invalid playback selection");
}
}
