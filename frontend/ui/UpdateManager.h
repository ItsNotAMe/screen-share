#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include "ui/UpdatePackageSelection.h"

class QNetworkAccessManager;
class QWidget;

class UpdateManager final : public QObject {
public:
    explicit UpdateManager(QWidget* dialogParent, QObject* parent = nullptr);

    void checkForUpdates();

private:
    friend struct UpdateManagerTestAccess;
    struct UpdateInfo {
        QString version, channel, packageUrl, sha256, signatureBase64;
        screenshare::ui::UpdatePackageKind packageKind = screenshare::ui::UpdatePackageKind::PortableZip;
        qint64 sizeBytes = 0;
        QStringList notes;
    };

    void handleManifestReply(class QNetworkReply* reply);
    void showUpdateDialog(const UpdateInfo& update);
    void downloadUpdate(const UpdateInfo& update, class QProgressBar* progress, class QLabel* statusLabel, class QPushButton* installButton, class QPushButton* laterButton);
    bool launchUpdater(const UpdateInfo& update, const QString& packagePath, QString* errorMessage);

    QWidget* dialogParent_ = nullptr;
    QNetworkAccessManager* network_ = nullptr;
    bool checking_ = false;
};
