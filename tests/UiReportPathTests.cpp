#include "ui/UiReportPath.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUrl>

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
    QString openedFolder;
    const auto logFolder=QDir(temporaryDirectory.path()).filePath("new/logs with spaces");
    passed &= Check(!QFileInfo::exists(logFolder), "The log folder fixture should start absent.");
    passed &= Check(OpenUiLogFolder(logFolder,[&](const QUrl& url) {
        openedFolder=url.toLocalFile();return url.isLocalFile() && QFileInfo(openedFolder).isDir();
    }) && openedFolder==logFolder, "Opening logs must create/open the folder before any report exists.");
    passed &= Check(!OpenUiLogFolder(logFolder,[](const QUrl&) {return false;}) && QFileInfo(logFolder).isDir(),
        "An Explorer failure must retain the folder for a retry.");
    QFile blocker(QDir(temporaryDirectory.path()).filePath("not-a-folder"));
    passed &= Check(blocker.open(QIODevice::WriteOnly), "Could not create the blocked folder fixture.");blocker.close();
    bool called=false;
    passed &= Check(!OpenUiLogFolder(blocker.fileName(),[&](const QUrl&) {called=true;return true;}) && !called,
        "A path occupied by a file must fail without asking Explorer to open it.");
    return passed ? 0 : 1;
}
