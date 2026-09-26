#pragma once
#include "media/DiagnosticHistory.h"
#include <QJsonArray>
#include <QJsonObject>

inline QJsonObject DiagnosticRecordJson(const screenshare::media::DiagnosticRecord& value) {
    QJsonObject result{{"atMs", qint64(value.atMs)}};
    for (const auto& [key, number] : value.numbers) result[QString::fromStdString(key)] = number;
    for (const auto& [key, label] : value.labels) result[QString::fromStdString(key)] = QString::fromStdString(label);
    return result;
}
inline QJsonObject DiagnosticHistoryJson(const screenshare::media::DiagnosticHistorySnapshot& value) {
    QJsonArray records;
    if (value.records) for (const auto& record : *value.records) records.append(DiagnosticRecordJson(record));
    return {{"records", records}, {"omitted", qint64(value.omitted)},
        {"startedUtcMs", qint64(value.startedUtcMs)}, {"elapsedMs", qint64(value.elapsedMs)},
        {"firstFailure", value.firstFailure ? QJsonValue(DiagnosticRecordJson(*value.firstFailure)) : QJsonValue(QJsonValue::Null)}};
}
