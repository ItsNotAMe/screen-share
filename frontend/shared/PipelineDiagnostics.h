#pragma once
#include "api/RoomSession.h"
#include <QJsonObject>

inline const char* SettingsErrorName(screenshare::media::SettingsApplyError value) {
    using enum screenshare::media::SettingsApplyError;
    switch (value) { case None: return "none"; case Invalid: return "invalid";
    case StaleRevision: return "stale-revision"; case UnsupportedTopology: return "unsupported-topology";
    case SenderRejected: return "sender-rejected"; } return "unknown";
}
inline QJsonObject StreamApplicationJson(const screenshare::v2::StreamStatus& stream) {
    int applied = 0, rejected = 0, pending = 0;
    for (const auto& peer : stream.peers) {
        if (peer.rejected) ++rejected;
        else if (peer.appliedRevision == stream.requestedRevision && stream.requestedRevision &&
            (peer.observedRevision == stream.requestedRevision || peer.appliedVideoBitrateBps == 0)) ++applied;
        else ++pending;
    }
    return {{"applied", applied}, {"rejected", rejected}, {"pending", pending},
        {"state", stream.peers.empty() ? "waiting-for-viewers" : rejected && applied ? "partial" :
            rejected ? "rejected" : pending ? "pending" : "applied"}};
}
inline QJsonObject PipelineDiagnosticsJson(const screenshare::v2::StreamStatus& stream) {
    if (!stream.requestedRevision) return {{"captureState", "unknown"}, {"captureFailure", "unknown"},
        {"sourceGeneration", QJsonValue(QJsonValue::Null)}, {"hardwarePipeline", QJsonValue(QJsonValue::Null)}};
    using namespace screenshare::media;
    const char* state = "unknown"; const char* failure = "unknown";
    switch (stream.capture.state) {
    case HostMediaState::Idle: state = "idle"; break; case HostMediaState::Starting: state = "starting"; break;
    case HostMediaState::WaitingForViewers: state = "waiting-for-viewers"; break; case HostMediaState::Running: state = "running"; break;
    case HostMediaState::Recovering: state = "recovering"; break; case HostMediaState::Stopping: state = "stopping"; break;
    case HostMediaState::Stopped: state = "stopped"; break; case HostMediaState::Failed: state = "failed"; break;
    }
    switch (stream.capture.failure) {
    case CaptureFailure::None: failure = "none"; break; case CaptureFailure::Source: failure = "source"; break;
    case CaptureFailure::StartupTimeout: failure = "startup-timeout"; break; case CaptureFailure::Recovery: failure = "recovery"; break;
    case CaptureFailure::Consumer: failure = "consumer"; break;
    }
    const auto& codec = stream.codec;
    return {{"captureState", state}, {"captureFailure", failure}, {"sourceGeneration", qint64(stream.capture.generation)},
        {"hardwarePipeline", codec.available ? QJsonValue(QJsonObject{{"hardwareFrames", qint64(codec.hardwareFrames)},
            {"softwareFallbacks", qint64(codec.softwareFallbacks)}, {"quarantined", codec.quarantined}, {"retired", codec.retired},
            {"fallbackState", codec.retired ? "device-retired" : codec.quarantined ? "hardware-quarantined" : "none"}}) : QJsonValue(QJsonValue::Null)}};
}
inline QJsonObject PeerRecoveryJson(const screenshare::media::PeerRecoveryStatus& value) {
    using namespace screenshare::media;
    const char* state = "unknown"; const char* failure = "unknown";
    switch (value.state) {
    case PeerLifecycleState::Connecting: state = "connecting"; break; case PeerLifecycleState::Connected: state = "connected"; break;
    case PeerLifecycleState::Backoff: state = "backoff"; break; case PeerLifecycleState::Restarting: state = "restarting"; break;
    case PeerLifecycleState::Failed: state = "failed"; break; case PeerLifecycleState::Closed: state = "closed"; break;
    }
    switch (value.failure) {
    case PeerLifecycleFailure::None: failure = "none"; break; case PeerLifecycleFailure::DirectConnectTimeout: failure = "connect-timeout"; break;
    case PeerLifecycleFailure::RestartLimit: failure = "restart-limit"; break; case PeerLifecycleFailure::RemoteClosed: failure = "remote-closed"; break;
    }
    return {{"state", state}, {"failure", failure}, {"restartRevision", qint64(value.restartRevision)},
        {"dispatchFailed", value.dispatchFailed}, {"operationFailed", value.operationFailed}};
}
