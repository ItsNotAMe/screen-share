#include "ui/UpdateManager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

#ifndef SCREENSHARE_APP_VERSION
#define SCREENSHARE_APP_VERSION "0.0.0"
#endif

#ifndef SCREENSHARE_UPDATE_MANIFEST_URL
#define SCREENSHARE_UPDATE_MANIFEST_URL ""
#endif

namespace {

constexpr qsizetype kMaxManifestBytes = 256 * 1024;

QString appVersion()
{
    return QString::fromUtf8(SCREENSHARE_APP_VERSION);
}

QString manifestUrl()
{
    return QString::fromUtf8(SCREENSHARE_UPDATE_MANIFEST_URL);
}

QVector<int> versionParts(const QString& version)
{
    QVector<int> parts;
    static const QRegularExpression numberPattern(QStringLiteral("(\\d+)"));
    auto matches = numberPattern.globalMatch(version);
    while (matches.hasNext() && parts.size() < 8) {
        parts.push_back(matches.next().captured(1).toInt());
    }
    return parts;
}

bool isVersionNewer(const QString& current, const QString& candidate)
{
    const QVector<int> currentParts = versionParts(current);
    const QVector<int> candidateParts = versionParts(candidate);
    const int count = std::max(currentParts.size(), candidateParts.size());
    for (int index = 0; index < count; ++index) {
        const int currentPart = index < currentParts.size() ? currentParts[index] : 0;
        const int candidatePart = index < candidateParts.size() ? candidateParts[index] : 0;
        if (candidatePart != currentPart) {
            return candidatePart > currentPart;
        }
    }
    return false;
}

bool isValidSha256(const QString& value)
{
    static const QRegularExpression hashPattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
    return hashPattern.match(value.trimmed()).hasMatch();
}

QString sha256Hex(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        hash.addData(file.read(1024 * 1024));
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString tempUpdateDir()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (base.isEmpty()) {
        base = QDir::tempPath();
    }
    QDir dir(base);
    dir.mkpath(QStringLiteral("ScreenShare/updates"));
    return dir.filePath(QStringLiteral("ScreenShare/updates"));
}

QString notesText(const QStringList& notes)
{
    if (notes.isEmpty()) {
        return QStringLiteral("This update includes the latest ScreenShare fixes and improvements.");
    }

    QStringList lines;
    for (const QString& note : notes) {
        if (!note.trimmed().isEmpty()) {
            lines.push_back(QStringLiteral("- %1").arg(note.trimmed()));
        }
    }
    return lines.join(QLatin1Char('\n'));
}

QLabel* updateLabel(const QString& text, const char* objectName)
{
    auto* label = new QLabel(text);
    label->setObjectName(objectName);
    label->setWordWrap(true);
    return label;
}

} // namespace

struct UpdateManager::UpdateInfo {
    QString version;
    QString channel;
    QString packageUrl;
    QString sha256;
    qint64 sizeBytes = 0;
    QStringList notes;
};

UpdateManager::UpdateManager(QWidget* dialogParent, QObject* parent)
    : QObject(parent)
    , dialogParent_(dialogParent)
    , network_(new QNetworkAccessManager(this))
{
}

void UpdateManager::checkForUpdates()
{
    if (checking_) {
        return;
    }

    const QUrl url(manifestUrl());
    if (!url.isValid() || url.scheme() != QLatin1String("https")) {
        return;
    }

    checking_ = true;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ScreenShareUi/%1").arg(appVersion()));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = network_->get(request);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handleManifestReply(reply);
    });
}

void UpdateManager::handleManifestReply(QNetworkReply* reply)
{
    checking_ = false;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    const QByteArray body = reply->read(kMaxManifestBytes + 1);
    if (body.size() > kMaxManifestBytes) {
        return;
    }

    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject()) {
        return;
    }

    const QJsonObject root = document.object();
    const QJsonObject assets = root.value(QStringLiteral("assets")).toObject();
    const QJsonObject portableZip = assets.value(QStringLiteral("portableZip")).toObject();

    UpdateInfo update;
    update.version = root.value(QStringLiteral("version")).toString().trimmed();
    update.channel = root.value(QStringLiteral("channel")).toString(QStringLiteral("stable")).trimmed();
    update.packageUrl = portableZip.value(QStringLiteral("url")).toString().trimmed();
    update.sha256 = portableZip.value(QStringLiteral("sha256")).toString().trimmed();
    update.sizeBytes = portableZip.value(QStringLiteral("size")).toVariant().toLongLong();

    const QJsonArray notesArray = root.value(QStringLiteral("notes")).toArray();
    for (const QJsonValue& value : notesArray) {
        const QString note = value.toString().trimmed();
        if (!note.isEmpty()) {
            update.notes.push_back(note);
        }
    }

    const QUrl packageUrl(update.packageUrl);
    if (update.version.isEmpty() ||
        !isVersionNewer(appVersion(), update.version) ||
        !packageUrl.isValid() ||
        packageUrl.scheme() != QLatin1String("https") ||
        !isValidSha256(update.sha256)) {
        return;
    }

    showUpdateDialog(update);
}

void UpdateManager::showUpdateDialog(const UpdateInfo& update)
{
    auto* dialog = new QDialog(dialogParent_);
    dialog->setObjectName("UpdateDialog");
    dialog->setWindowTitle(QStringLiteral("ScreenShare update"));
    dialog->setModal(false);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    auto* outerLayout = new QVBoxLayout(dialog);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto* frame = new QFrame;
    frame->setObjectName("UpdateDialogFrame");
    outerLayout->addWidget(frame);

    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(22, 20, 22, 18);
    layout->setSpacing(14);

    layout->addWidget(updateLabel(QStringLiteral("Update available"), "UpdateTitle"));
    layout->addWidget(updateLabel(
        QStringLiteral("ScreenShare %1 is ready. You are running %2.")
            .arg(update.version, appVersion()),
        "UpdateBody"));

    auto* notesFrame = new QFrame;
    notesFrame->setObjectName("UpdateNotesFrame");
    auto* notesLayout = new QVBoxLayout(notesFrame);
    notesLayout->setContentsMargins(14, 12, 14, 12);
    notesLayout->setSpacing(8);
    notesLayout->addWidget(updateLabel(QStringLiteral("What changed"), "UpdateSectionTitle"));
    notesLayout->addWidget(updateLabel(notesText(update.notes), "UpdateNote"));
    layout->addWidget(notesFrame);

    layout->addWidget(updateLabel(
        QStringLiteral("The package is downloaded over HTTPS and must match the release SHA-256 hash before installation."),
        "UpdateSecurity"));

    auto* statusLabel = updateLabel(QStringLiteral("Ready to download"), "UpdateBody");
    layout->addWidget(statusLabel);

    auto* progress = new QProgressBar;
    progress->setObjectName("UpdateProgress");
    progress->setRange(0, 100);
    progress->setValue(0);
    progress->hide();
    layout->addWidget(progress);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* laterButton = new QPushButton(QStringLiteral("Later"));
    laterButton->setObjectName("UpdateSecondary");
    auto* installButton = new QPushButton(QStringLiteral("Download and install"));
    installButton->setObjectName("UpdatePrimary");
    buttons->addWidget(laterButton);
    buttons->addWidget(installButton);
    layout->addLayout(buttons);

    QObject::connect(laterButton, &QPushButton::clicked, dialog, &QDialog::close);
    QObject::connect(installButton, &QPushButton::clicked, this, [this, update, progress, statusLabel, installButton, laterButton] {
        downloadUpdate(update, progress, statusLabel, installButton, laterButton);
    });

    dialog->resize(470, 360);
    dialog->show();
}

void UpdateManager::downloadUpdate(
    const UpdateInfo& update,
    QProgressBar* progress,
    QLabel* statusLabel,
    QPushButton* installButton,
    QPushButton* laterButton)
{
    installButton->setEnabled(false);
    laterButton->setEnabled(false);
    progress->show();
    statusLabel->setText(QStringLiteral("Downloading update..."));

    const QString outputDir = tempUpdateDir();
    const QString outputPath = QDir(outputDir).filePath(QStringLiteral("ScreenShare-%1-update.zip").arg(update.version));
    QFile::remove(outputPath);

    auto* output = new QFile(outputPath, this);
    if (!output->open(QIODevice::WriteOnly)) {
        statusLabel->setText(QStringLiteral("Could not create the update package file."));
        laterButton->setEnabled(true);
        output->deleteLater();
        return;
    }

    QNetworkRequest request(QUrl(update.packageUrl));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ScreenShareUi/%1").arg(appVersion()));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = network_->get(request);
    output->setParent(reply);

    QObject::connect(reply, &QNetworkReply::readyRead, this, [reply, output] {
        output->write(reply->readAll());
    });
    QObject::connect(reply, &QNetworkReply::downloadProgress, this, [progress](qint64 received, qint64 total) {
        if (total > 0) {
            progress->setValue(static_cast<int>((received * 100) / total));
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, output, outputPath, update, progress, statusLabel, installButton, laterButton] {
        output->write(reply->readAll());
        output->close();
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            QFile::remove(outputPath);
            statusLabel->setText(QStringLiteral("Download failed. Check your connection and try again."));
            installButton->setEnabled(true);
            laterButton->setEnabled(true);
            return;
        }

        progress->setValue(100);
        statusLabel->setText(QStringLiteral("Verifying package..."));
        const QString actualHash = sha256Hex(outputPath);
        if (actualHash.compare(update.sha256, Qt::CaseInsensitive) != 0) {
            QFile::remove(outputPath);
            statusLabel->setText(QStringLiteral("Update verification failed. The package hash did not match the release manifest."));
            laterButton->setEnabled(true);
            return;
        }

        QString errorMessage;
        if (!launchUpdater(update, outputPath, &errorMessage)) {
            statusLabel->setText(errorMessage);
            laterButton->setEnabled(true);
            return;
        }

        statusLabel->setText(QStringLiteral("Installing after ScreenShare closes..."));
        QTimer::singleShot(100, qApp, &QCoreApplication::quit);
    });
}

bool UpdateManager::launchUpdater(const UpdateInfo& update, const QString& packagePath, QString* errorMessage)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString helperPath = QDir(appDir).filePath(QStringLiteral("ScreenShareUpdater.exe"));
    if (!QFileInfo::exists(helperPath)) {
        *errorMessage = QStringLiteral("ScreenShareUpdater.exe was not found beside the app.");
        return false;
    }

    const QString tempHelper = QDir(tempUpdateDir()).filePath(QStringLiteral("ScreenShareUpdater-%1.exe").arg(update.version));
    QFile::remove(tempHelper);
    if (!QFile::copy(helperPath, tempHelper)) {
        *errorMessage = QStringLiteral("Could not stage the updater helper.");
        return false;
    }

    QStringList arguments;
    arguments
        << QStringLiteral("--pid") << QString::number(QCoreApplication::applicationPid())
        << QStringLiteral("--package") << packagePath
        << QStringLiteral("--target") << appDir
        << QStringLiteral("--restart") << QCoreApplication::applicationFilePath();

    if (!QProcess::startDetached(tempHelper, arguments, appDir)) {
        *errorMessage = QStringLiteral("Could not start the updater helper.");
        return false;
    }

    return true;
}
