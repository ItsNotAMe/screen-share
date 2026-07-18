#pragma once

#include <QtCore/QString>

QString DefaultUiReportsDirectory();
QString ResolveUiReportPath(
    const QString& configuredPath,
    const QString& reportsDirectory = QString());
