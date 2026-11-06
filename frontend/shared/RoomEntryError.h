#pragma once
#include "api/RoomSession.h"
#include <QString>

// Shared by standalone sessions and the Join form after asynchronous cleanup.
// Use only typed, validated reasons; never display a service response body.
inline QString RoomEntryError(screenshare::v2::RoomError error, bool hosting = false) {
    using screenshare::v2::RoomError;
    switch (error) {
    case RoomError::RoomNotFound: return QStringLiteral("This room could not be found. Ask the host for a current invite.");
    case RoomError::RoomFull: return hosting ?
        QStringLiteral("The room service has reached its room limit. Wait for a room to close, then try again.") :
        QStringLiteral("This room is full. Wait for a viewer to leave or ask the host to increase the viewer limit.");
    case RoomError::RoomClosed: return QStringLiteral("This room is closed. Ask the host to start a new room.");
    case RoomError::RateLimited: return QStringLiteral("Too many room requests. Wait a moment before trying again.");
    case RoomError::AdmissionUnconfirmed: return QStringLiteral("Could not confirm whether you entered the room. Wait a moment before trying again.");
    case RoomError::AdmissionDenied: return QStringLiteral("Could not enter the room. Check the room password and try again.");
    case RoomError::Transport: return QStringLiteral("Could not connect to the room. Check your connection and try again.");
    case RoomError::Media: return QStringLiteral("Could not start audio or video. Check device availability and try again.");
    default: return QStringLiteral("Could not enter the room. Check your connection and the room service, then try again.");
    }
}

inline QString RoomSessionError(screenshare::v2::RoomError error, bool hosting, bool wasActive) {
    using screenshare::v2::RoomError;
    if (wasActive && (error==RoomError::Media || error==RoomError::Transport)) {
        const auto cause = error==RoomError::Media ? QStringLiteral("The room's audio or video failed.") :
            QStringLiteral("The connection to the room service was lost.");
        return cause + (hosting ? QStringLiteral(" Stop sharing and start a new room to retry.") :
            QStringLiteral(" Leave and rejoin the room to retry."));
    }
    return RoomEntryError(error, hosting);
}
