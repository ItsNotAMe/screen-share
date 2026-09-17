#pragma once
#include "HostMediaSession.h"
#include "PeerConnectionLifecycle.h"

namespace screenshare::media {
struct CapturePipelineStatus {
    HostMediaState state = HostMediaState::Idle;
    CaptureFailure failure = CaptureFailure::None;
    uint64_t generation = 0;
    CaptureSourceInfo source;
};
struct CodecPipelineStatus {
    bool available = false, quarantined = false, retired = false;
    uint64_t hardwareFrames = 0, softwareFallbacks = 0;
};
struct PeerRecoveryStatus {
    PeerLifecycleState state = PeerLifecycleState::Connecting;
    PeerLifecycleFailure failure = PeerLifecycleFailure::None;
    uint64_t restartRevision = 0;
    bool dispatchFailed = false, operationFailed = false;
};
}
