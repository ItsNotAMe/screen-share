#pragma once
#include <cstdint>

namespace screenshare::media {
enum class AudioEndpointState { Inactive, Running, Silent, Failed };
struct AudioEndpointHealth { AudioEndpointState state = AudioEndpointState::Inactive; uint64_t failures = 0; };
inline const char* AudioEndpointStateName(AudioEndpointState state) {
    switch (state) {
    case AudioEndpointState::Running: return "running";
    case AudioEndpointState::Silent: return "silent";
    case AudioEndpointState::Failed: return "failed";
    default: return "inactive";
    }
}
}
