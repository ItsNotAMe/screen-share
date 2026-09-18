#pragma once
#include "LatestRoomVideoFrame.h"
#include <QJsonObject>

// Local decoder-callback -> Take handoff only. Excludes conversion, rendering,
// WebRTC buffering and physical display latency. No cross-machine timestamps.
inline QJsonObject FrameQueueDiagnostics(const LatestRoomVideoFrame::Statistics& value) {
    auto age = [](const std::optional<uint64_t>& us) -> QJsonValue {
        return us ? QJsonValue(double(*us) / 1000) : QJsonValue(QJsonValue::Null);
    };
    return {{"schema", 1}, {"scope", "decoded-frame-handoff"},
        {"received", qint64(value.received)}, {"replaced", qint64(value.replaced)},
        {"delivered", qint64(value.delivered)}, {"failed", qint64(value.failed)},
        {"discardedOnStop", qint64(value.discardedOnStop)},
        {"inFlight", qint64(value.inFlight)}, {"pending", qint64(value.pending)},
        {"pendingAgeMs", age(value.pendingAgeUs)}, {"lastWaitMs", age(value.lastWaitUs)},
        {"maxWaitMs", double(value.maxWaitUs) / 1000}};
}
