#pragma once

#include "core/ScreenShareSession.h"

#include <QtCore/QString>
#include <QtCore/QVector>

#include <cstdint>

struct ShareResolutionChoice {
    QString text;
    QString value;
};

struct ShareDisplayChoice {
    QString text;
    QString value;
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
    QString displaySourceValue;
    int displayValue = 0;
    uint64_t windowHandle = 0;
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
