#pragma once

#include "core/ScreenShareSession.h"

#include <QtCore/QString>

struct WatchSessionUiState {
    screenshare::WatchSessionConfig config;
    QString roomId;
    QString roomName;
    bool passwordProtected = false;
};
