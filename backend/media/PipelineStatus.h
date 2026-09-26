#pragma once
#include "HostMediaSession.h"
#include "PeerConnectionLifecycle.h"
#include <string>
#include "DiagnosticHistory.h"

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
// Safe connection evidence, also available to viewers and after native cleanup.
// No SDP, addresses, candidate strings or credentials are retained here.
struct PeerConnectionStatus {
    std::string peerId;
    PeerRecoveryStatus recovery;
    bool negotiated = false, retained = false;
    uint32_t localCandidates = 0, remoteCandidates = 0;
    uint64_t generation = 0, ageMs = 0;
    DiagnosticRecord current;
    DiagnosticHistorySnapshot events, transportHistory, mediaHistory;
    std::optional<uint64_t> statsAgeMs;
};
}
