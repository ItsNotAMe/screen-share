#include "ui/UpdateManager.h"

#include "updater/UpdateSignature.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPoint>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QSize>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QIcon>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtSvg/QSvgRenderer>
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
#include <array>
#include <cstddef>
#include <span>

#ifndef SCREENSHARE_APP_VERSION
#define SCREENSHARE_APP_VERSION "0.0.0"
#endif

namespace {

// Pinned ECDSA-P256 public key (raw X||Y, 64 bytes) of the release signing key.
// REPLACE the zeros with your key before shipping (see docs/update-signing.md).
// All-zero means "not configured", which disables auto-update (fail closed) so
// an unsigned build can never auto-install anything.
constexpr std::array<unsigned char, 64> kUpdatePublicKeyXy = {};

bool updateSigningConfigured()
{
    return std::any_of(kUpdatePublicKeyXy.begin(), kUpdatePublicKeyXy.end(),
                       [](unsigned char b) { return b != 0; });
}

// Verify the manifest signature over the security-critical fields before an
// update is ever offered/downloaded. The signed message is exactly
// "version\npackageUrl\nsha256"; the signature binds the trusted sha256 (and
// thus the package) to the pinned release key.
bool verifyUpdateSignature(
    const QString& version,
    const QString& packageUrl,
    const QString& sha256,
    const QString& signatureBase64)
{
    if (!updateSigningConfigured() || signatureBase64.isEmpty()) {
        return false;
    }
    const QByteArray signature = QByteArray::fromBase64(
        signatureBase64.toLatin1(),
        QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (signature.size() != 64) {
        return false;
    }
    const QByteArray message = (version + QLatin1Char('\n') + packageUrl + QLatin1Char('\n') + sha256).toUtf8();
    return screenshare::VerifyUpdateManifestSignatureEcdsaP256(
        std::as_bytes(std::span<const unsigned char>(kUpdatePublicKeyXy.data(), kUpdatePublicKeyXy.size())),
        std::as_bytes(std::span<const char>(message.constData(), static_cast<size_t>(message.size()))),
        std::as_bytes(std::span<const char>(signature.constData(), static_cast<size_t>(signature.size()))));
}

} // namespace

#ifndef SCREENSHARE_UPDATE_MANIFEST_URL
#define SCREENSHARE_UPDATE_MANIFEST_URL ""
#endif

namespace {

constexpr qsizetype kMaxManifestBytes = 256 * 1024;

class UpdateDialog final : public QDialog {
public:
    explicit UpdateDialog(QWidget* parent = nullptr)
        : QDialog(parent)
    {
    }

    void enableDrag(QWidget* widget)
    {
        if (widget != nullptr) {
            widget->installEventFilter(this);
        }
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        Q_UNUSED(watched);

        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                dragging_ = true;
                dragOffset_ = mouseEvent->globalPosition().toPoint() - frameGeometry().topLeft();
            }
        } else if (event->type() == QEvent::MouseMove) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (dragging_ && (mouseEvent->buttons() & Qt::LeftButton)) {
                move(mouseEvent->globalPosition().toPoint() - dragOffset_);
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            dragging_ = false;
        }

        return QDialog::eventFilter(watched, event);
    }

private:
    bool dragging_ = false;
    QPoint dragOffset_;
};

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

QStringList normalizedNotes(const QStringList& notes)
{
    QStringList result;
    if (notes.isEmpty()) {
        result.push_back(QStringLiteral("Latest ScreenShare fixes and improvements"));
        return result;
    }

    for (const QString& note : notes) {
        if (!note.trimmed().isEmpty()) {
            result.push_back(note.trimmed());
        }
    }
    return result;
}

QLabel* updateLabel(const QString& text, const char* objectName)
{
    auto* label = new QLabel(text);
    label->setObjectName(objectName);
    label->setWordWrap(true);
    return label;
}

QFrame* updateSeparator(const char* objectName)
{
    auto* line = new QFrame;
    line->setObjectName(objectName);
    line->setFrameShape(QFrame::HLine);
    line->setFixedHeight(1);
    return line;
}

QPixmap updateIconPixmap(const QString& name, const char* color, int size)
{
    QFile file(QStringLiteral(":/screenshare/ui/icons/%1.svg").arg(name));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QByteArray svg = file.readAll();
    svg.replace("currentColor", color);
    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) {
        return {};
    }

    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&painter);
    return pixmap;
}

QLabel* updateIconLabel(const QString& iconName, const char* color, int size, const char* objectName)
{
    auto* label = new QLabel;
    label->setObjectName(objectName);
    label->setFixedSize(size, size);
    label->setAlignment(Qt::AlignCenter);
    label->setPixmap(updateIconPixmap(iconName, color, size - 14));
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

    // Verify the manifest is signed by the pinned release key BEFORE offering
    // the update. Without this, a compromised release host/CDN could serve a
    // matching {malicious package, sha256} pair; the signature binds the
    // trusted sha256 to a key an attacker does not hold. Fails closed.
    const QString signatureBase64 = root.value(QStringLiteral("signature")).toString().trimmed();
    if (!verifyUpdateSignature(update.version, update.packageUrl, update.sha256, signatureBase64)) {
        return;
    }

    showUpdateDialog(update);
}

void UpdateManager::showUpdateDialog(const UpdateInfo& update)
{
    auto* dialog = new UpdateDialog(dialogParent_);
    dialog->setObjectName("UpdateDialog");
    dialog->setWindowTitle(QStringLiteral("ScreenShare update"));
    dialog->setWindowIcon(QIcon(QStringLiteral(":/screenshare/brand/screenshare-mark.svg")));
    dialog->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    dialog->setModal(false);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAttribute(Qt::WA_TranslucentBackground);

    auto* outerLayout = new QVBoxLayout(dialog);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto* frame = new QFrame;
    frame->setObjectName("UpdateDialogFrame");
    outerLayout->addWidget(frame);

    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(22, 18, 22, 22);
    layout->setSpacing(16);

    auto* header = new QFrame;
    header->setObjectName("UpdateHeader");
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(16);

    auto* downloadBadge = updateIconLabel(QStringLiteral("download"), "#28ded3", 54, "UpdateDownloadBadge");
    auto* titleLabel = updateLabel(QStringLiteral("Update Available"), "UpdateTitle");
    auto* closeButton = new QPushButton;
    closeButton->setObjectName("UpdateCloseButton");
    closeButton->setFixedSize(34, 34);
    closeButton->setIcon(QIcon(updateIconPixmap(QStringLiteral("window-close"), "#cfd8d5", 22)));
    closeButton->setIconSize(QSize(22, 22));
    closeButton->setFlat(true);

    headerLayout->addWidget(downloadBadge, 0, Qt::AlignVCenter);
    headerLayout->addWidget(titleLabel, 1, Qt::AlignVCenter);
    headerLayout->addWidget(closeButton, 0, Qt::AlignTop);
    layout->addWidget(header);
    layout->addWidget(updateSeparator("UpdateSeparator"));
    dialog->enableDrag(header);
    dialog->enableDrag(downloadBadge);
    dialog->enableDrag(titleLabel);
    QObject::connect(closeButton, &QPushButton::clicked, dialog, &QDialog::close);

    auto* versionLabel = updateLabel(
        QStringLiteral("Version <span style=\"color:#28ded3; font-size:18pt; font-weight:800;\">%1</span> is ready to download.")
            .arg(update.version.toHtmlEscaped()),
        "UpdateVersion");
    versionLabel->setTextFormat(Qt::RichText);
    layout->addWidget(versionLabel);

    auto* notesFrame = new QFrame;
    notesFrame->setObjectName("UpdateNotesFrame");
    auto* notesLayout = new QVBoxLayout(notesFrame);
    notesLayout->setContentsMargins(18, 2, 18, 2);
    notesLayout->setSpacing(0);

    const QStringList notes = normalizedNotes(update.notes);
    for (int index = 0; index < notes.size(); ++index) {
        auto* row = new QFrame;
        row->setObjectName("UpdateNoteRow");
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(16);

        auto* bullet = new QLabel;
        bullet->setObjectName("UpdateBullet");
        bullet->setFixedSize(8, 8);
        auto* noteLabel = updateLabel(notes[index], "UpdateNote");
        rowLayout->addWidget(bullet, 0, Qt::AlignVCenter);
        rowLayout->addWidget(noteLabel, 1);
        notesLayout->addWidget(row);

        if (index + 1 < notes.size()) {
            notesLayout->addWidget(updateSeparator("UpdateNoteDivider"));
        }
    }
    layout->addWidget(notesFrame);

    auto* securityRow = new QFrame;
    securityRow->setObjectName("UpdateSecurityRow");
    auto* securityLayout = new QHBoxLayout(securityRow);
    securityLayout->setContentsMargins(0, 0, 0, 0);
    securityLayout->setSpacing(14);
    securityLayout->addWidget(updateIconLabel(QStringLiteral("shield"), "#28ded3", 38, "UpdateSecurityIcon"), 0, Qt::AlignVCenter);
    securityLayout->addWidget(updateLabel(QStringLiteral("Package is verified before install"), "UpdateSecurity"), 1);
    layout->addWidget(securityRow);

    auto* statusLabel = updateLabel(QStringLiteral("Ready to download"), "UpdateStatus");
    statusLabel->hide();
    layout->addWidget(statusLabel);

    auto* progress = new QProgressBar;
    progress->setObjectName("UpdateProgress");
    progress->setRange(0, 100);
    progress->setValue(0);
    progress->hide();
    layout->addWidget(progress);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(18);
    auto* laterButton = new QPushButton(QStringLiteral("Later"));
    laterButton->setObjectName("UpdateSecondary");
    laterButton->setMinimumHeight(56);
    auto* installButton = new QPushButton(QStringLiteral("Download Update"));
    installButton->setObjectName("UpdatePrimary");
    installButton->setMinimumHeight(56);
    installButton->setIcon(QIcon(updateIconPixmap(QStringLiteral("download"), "#ffffff", 24)));
    installButton->setIconSize(QSize(24, 24));
    buttons->addWidget(laterButton, 1);
    buttons->addWidget(installButton, 1);
    layout->addLayout(buttons);

    QObject::connect(laterButton, &QPushButton::clicked, dialog, &QDialog::close);
    QObject::connect(installButton, &QPushButton::clicked, this, [this, update, progress, statusLabel, installButton, laterButton] {
        downloadUpdate(update, progress, statusLabel, installButton, laterButton);
    });

    dialog->setFixedSize(500, 480);
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
    installButton->setText(QStringLiteral("Downloading..."));
    laterButton->setEnabled(false);
    progress->show();
    statusLabel->show();
    statusLabel->setText(QStringLiteral("Downloading update..."));

    const QString outputDir = tempUpdateDir();
    const QString outputPath = QDir(outputDir).filePath(QStringLiteral("ScreenShare-%1-update.zip").arg(update.version));
    QFile::remove(outputPath);

    auto* output = new QFile(outputPath, this);
    if (!output->open(QIODevice::WriteOnly)) {
        statusLabel->setText(QStringLiteral("Could not create the update package file."));
        installButton->setText(QStringLiteral("Download Update"));
        installButton->setEnabled(true);
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
            installButton->setText(QStringLiteral("Download Update"));
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
            installButton->setText(QStringLiteral("Download Update"));
            installButton->setEnabled(true);
            laterButton->setEnabled(true);
            return;
        }

        QString errorMessage;
        if (!launchUpdater(update, outputPath, &errorMessage)) {
            statusLabel->setText(errorMessage);
            installButton->setText(QStringLiteral("Download Update"));
            installButton->setEnabled(true);
            laterButton->setEnabled(true);
            return;
        }

        installButton->setText(QStringLiteral("Installing..."));
        statusLabel->setText(QStringLiteral("Installing after ScreenShare closes..."));
        QTimer::singleShot(100, qApp, &QCoreApplication::quit);
    });
}

bool UpdateManager::launchUpdater(const UpdateInfo& update, const QString& packagePath, QString* errorMessage)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QDir appDirectory(appDir);
    if (QFileInfo::exists(appDirectory.filePath(QStringLiteral("CMakeCache.txt"))) ||
        appDirectory.exists(QStringLiteral("CMakeFiles"))) {
        *errorMessage = QStringLiteral("Updates cannot be installed from a CMake build folder. Extract the portable zip to a separate app folder and run ScreenShareUi.exe there.");
        return false;
    }

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
        << QStringLiteral("--restart") << QCoreApplication::applicationFilePath()
        // The helper re-verifies this hash immediately before extraction to
        // close the TOCTOU window while the app exits.
        << QStringLiteral("--sha256") << update.sha256;

    if (!QProcess::startDetached(tempHelper, arguments, tempUpdateDir())) {
        *errorMessage = QStringLiteral("Could not start the updater helper.");
        return false;
    }

    return true;
}
