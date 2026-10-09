#pragma once
#include "CaptureSelection.h"
#include "AudioEndpointHealth.h"
#include <string>

namespace screenshare::media {
enum class AudioKind { System, Microphone, Process, None, SharedSource };
inline const char* AudioKindName(AudioKind kind) {
    switch (kind) {
    case AudioKind::System: return "system";
    case AudioKind::Microphone: return "microphone";
    case AudioKind::Process: return "process";
    case AudioKind::None: return "none";
    case AudioKind::SharedSource: return "shared";
    default: return "unknown";
    }
}
struct AudioSelection {
    AudioKind kind = AudioKind::System;
    std::wstring deviceId;
    uint32_t processId = 0;
    bool operator==(const AudioSelection&) const = default;
};
using AudioUpdateError = CaptureUpdateError;
using AudioUpdateResult = CaptureUpdateResult;
struct AudioSelectionStatus {
    AudioSelection selected; uint64_t revision = 0; AudioEndpointHealth health;
    bool microphoneProcessing = false; // Configured capture-local speech processing, not AEC.
};
inline void ValidateAudioSelection(const AudioSelection& value) {
    if ((value.kind != AudioKind::System && value.kind != AudioKind::Microphone && value.kind != AudioKind::Process && value.kind != AudioKind::None && value.kind != AudioKind::SharedSource) ||
        ((value.kind == AudioKind::None || value.kind == AudioKind::SharedSource) && !value.deviceId.empty()) ||
        value.deviceId.size() > 4096 || value.deviceId.find(L'\0') != std::wstring::npos ||
        (value.kind == AudioKind::Process ? (!value.processId || !value.deviceId.empty()) : value.processId != 0))
        throw std::invalid_argument("Invalid audio selection");
}
}
