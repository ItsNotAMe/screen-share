#include "ui/UiReportPath.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>

QString DefaultUiReportsDirectory()
{
    QString baseDirectory = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (baseDirectory.isEmpty()) {
        baseDirectory = QDir::tempPath();
    }
    return QDir::cleanPath(QDir(baseDirectory).filePath(QStringLiteral("ScreenShare/reports")));
}

QString ResolveUiReportPath(const QString& configuredPath, const QString& reportsDirectory)
{
    const QString trimmedPath = configuredPath.trimmed();
    if (trimmedPath.isEmpty()) {
        return {};
    }

    const QString normalizedPath = QDir::fromNativeSeparators(trimmedPath);
    const QFileInfo configuredInfo(normalizedPath);
    if (configuredInfo.isAbsolute()) {
        return QDir::cleanPath(configuredInfo.absoluteFilePath());
    }

    const QString rootPath = QDir::cleanPath(
        reportsDirectory.isEmpty() ? DefaultUiReportsDirectory() : reportsDirectory);
    const QDir rootDirectory(rootPath);
    QString resolvedPath = QDir::cleanPath(rootDirectory.absoluteFilePath(normalizedPath));
    const QString relativePath = QDir::fromNativeSeparators(rootDirectory.relativeFilePath(resolvedPath));
    if (relativePath == QStringLiteral("..") ||
        relativePath.startsWith(QStringLiteral("../")) ||
        QFileInfo(relativePath).isAbsolute()) {
        QString fileName = configuredInfo.fileName();
        if (fileName.isEmpty()) {
            fileName = QStringLiteral("session-report.zip");
        }
        resolvedPath = QDir::cleanPath(rootDirectory.absoluteFilePath(fileName));
    }
    return resolvedPath;
}
