#pragma once
#include "media/StreamPreferences.h"
#include <QJsonObject>

screenshare::media::StreamPreferences ParseStreamPreferences(const QJsonObject&);
QJsonObject StreamPreferencesJson(const screenshare::media::StreamPreferences&);
