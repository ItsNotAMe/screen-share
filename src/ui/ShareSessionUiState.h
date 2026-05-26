#pragma once

#include "core/ScreenShareSession.h"

#include <QtCore/QString>
#include <QtCore/QVector>

struct ShareResolutionChoice {
    QString text;
    QString value;
};

struct ShareSessionUiState {
    screenshare::ShareSessionConfig config;
    QString roomId;
    QString roomName;
    QString roomLink;
    QString displayText;
    QString resolutionText;
    QString resolutionValue;
    QVector<ShareResolutionChoice> resolutionChoices;
    QString fpsText;
    QString audioText;
    bool passwordProtected = false;
};
