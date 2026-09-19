#pragma once
#include "input/v2/InputService.h"
#include <QJsonObject>

namespace screenshare::frontend {
inline const char* InputReason(screenshare::input::Reason reason) {
    using screenshare::input::Reason;
    switch (reason) {
    case Reason::None: return "none";
    case Reason::Unavailable: return "unavailable";
    case Reason::Revoked: return "revoked";
    case Reason::Disconnected: return "disconnected";
    case Reason::Watchdog: return "watchdog";
    case Reason::Backpressure: return "backpressure";
    case Reason::Backend: return "backend";
    case Reason::SourceChanged: return "source-changed";
    case Reason::Ownership: return "ownership";
    }
    return "unknown";
}
inline QJsonObject InputStatus(const screenshare::input::Status& state) {
    auto time = [](auto value) -> QJsonValue { return value ? QJsonValue(qint64(*value)) : QJsonValue(QJsonValue::Null); };
    return {{"peer", QString::fromStdString(state.peer)}, {"ready", state.ready},
        {"requested", state.requested}, {"granted", state.granted}, {"pending", state.grantPending},
        {"revokePending", state.revokePending}, {"reason", int(state.reason)}, {"reasonName", InputReason(state.reason)},
        {"applied", qint64(state.applied)}, {"rejected", qint64(state.rejected)}, {"coalesced", qint64(state.coalesced)},
        {"reliableQueued", int(state.reliableQueued)}, {"stateQueued", int(state.stateQueued)},
        {"transportBlocked", state.transportBlocked}, {"localQueueWaitUs", time(state.queueWaitUs)},
        {"localBackendApplyUs", time(state.backendApplyUs)}};
}
}
