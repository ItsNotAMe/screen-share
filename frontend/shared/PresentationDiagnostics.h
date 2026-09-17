#pragma once
#include "render/FramePresentationBackend.h"
#include <QJsonObject>
#include <QString>

inline QJsonObject PresentationDiagnosticsJson(const FramePresentationSession::Statistics& status) {
    return {{"outcome", screenshare::PresentationOutcomeName(status.outcome)},
        {"lastErrorCode", status.lastError == S_OK ? QJsonValue(QJsonValue::Null) :
            QJsonValue(QString("0x%1").arg(quint32(status.lastError), 8, 16, QLatin1Char('0')))},
        {"busyDrops", qint64(status.busyDrops)}, {"occludedDrops", qint64(status.occludedDrops)},
        {"minimizedDrops", qint64(status.minimizedDrops)}, {"unavailableDrops", qint64(status.unavailableDrops)},
        {"backoffDrops", qint64(status.backoffDrops)}};
}
