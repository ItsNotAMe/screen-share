#include "ui/UiReportPath.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>

#include <iostream>

namespace {

bool Check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

} // namespace

int main()
{
    QTemporaryDir temporaryDirectory;
    if (!Check(temporaryDirectory.isValid(), "Could not create the test directory.")) {
        return 1;
    }

    const QString reportsDirectory = QDir(temporaryDirectory.path()).filePath("ScreenShare/reports");
    bool passed = true;
    passed &= Check(
        ResolveUiReportPath(QString(), reportsDirectory).isEmpty(),
        "An empty report path must remain disabled.");
    passed &= Check(
        ResolveUiReportPath("sender-report.zip", reportsDirectory) ==
            QDir::cleanPath(QDir(reportsDirectory).absoluteFilePath("sender-report.zip")),
        "A relative report filename must resolve below the app report directory.");
    passed &= Check(
        ResolveUiReportPath("rooms/receiver-report.zip", reportsDirectory) ==
            QDir::cleanPath(QDir(reportsDirectory).absoluteFilePath("rooms/receiver-report.zip")),
        "A relative report subdirectory must remain below the app report directory.");

    const QString absolutePath = QDir(temporaryDirectory.path()).absoluteFilePath("explicit-report.zip");
    passed &= Check(
        ResolveUiReportPath(absolutePath, reportsDirectory) == QDir::cleanPath(absolutePath),
        "An explicit absolute report path must remain unchanged.");
    passed &= Check(
        ResolveUiReportPath("../../escaped-report.zip", reportsDirectory) ==
            QDir::cleanPath(QDir(reportsDirectory).absoluteFilePath("escaped-report.zip")),
        "A relative report path must not escape the app report directory.");

    const QString defaultDirectory = QDir::fromNativeSeparators(DefaultUiReportsDirectory());
    passed &= Check(
        QFileInfo(defaultDirectory).isAbsolute() && defaultDirectory.endsWith("/ScreenShare/reports"),
        "The default report directory must be an absolute ScreenShare-specific location.");
    return passed ? 0 : 1;
}
