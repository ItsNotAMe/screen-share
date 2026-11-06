#pragma once
#include "api/RoomSession.h"
#include <QString>

// Report only an authoritative remote closure. Local Leave and host Stop remain
// quiet; removal retains its existing persistent explanation on the Join form.
inline QString RoomEndNotice(const screenshare::v2::RoomStatus& status) {
    using screenshare::v2::RoomCloseReason;
    if (status.host || !status.closeReason) return {};
    switch (*status.closeReason) {
    case RoomCloseReason::HostLeft: return QStringLiteral("The host ended the room.");
    case RoomCloseReason::HostExpired: return QStringLiteral("The host disconnected and the room expired.");
    case RoomCloseReason::ServerShutdown: return QStringLiteral("The room service closed this room. Ask the host for a new invite.");
    case RoomCloseReason::RemovedByHost: return {};
    }
    return {};
}
