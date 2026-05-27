#pragma once

#include "core/ScreenShareSession.h"

#include <QtCore/QString>
#include <QtCore/QVector>

struct ShareResolutionChoice {
    QString text;
    QString value;
};

struct ShareDisplayChoice {
    QString text;
    int value = 0;
};

struct ShareAudioChoice {
    QString text;
    QString value;
};

struct ShareSessionUiState {
    screenshare::ShareSessionConfig config;
    QString roomId;
    QString roomName;
    QString roomLink;
    QString displayText;
    int displayValue = 0;
    QVector<ShareDisplayChoice> displayChoices;
    QString resolutionText;
    QString resolutionValue;
    QVector<ShareResolutionChoice> resolutionChoices;
    QString fpsText;
    int fpsValue = 60;
    QString audioText;
    QString audioDeviceValue;
    QVector<ShareAudioChoice> audioChoices;
    bool captureSystemAudio = true;
    bool passwordProtected = false;
};
