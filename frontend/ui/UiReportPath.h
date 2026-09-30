#pragma once

#include <QtCore/QString>
#include <functional>
class QUrl;

QString DefaultUiReportsDirectory();
QString ResolveUiReportPath(
    const QString& configuredPath,
    const QString& reportsDirectory = QString());
bool OpenUiLogFolder(const QString& folder, const std::function<bool(const QUrl&)>& openUrl);
