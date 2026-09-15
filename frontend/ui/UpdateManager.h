#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

class QNetworkAccessManager;
class QWidget;

class UpdateManager final : public QObject {
public:
    explicit UpdateManager(QWidget* dialogParent, QObject* parent = nullptr);

    void checkForUpdates();

private:
    struct UpdateInfo;

    void handleManifestReply(class QNetworkReply* reply);
    void showUpdateDialog(const UpdateInfo& update);
    void downloadUpdate(const UpdateInfo& update, class QProgressBar* progress, class QLabel* statusLabel, class QPushButton* installButton, class QPushButton* laterButton);
    bool launchUpdater(const UpdateInfo& update, const QString& packagePath, QString* errorMessage);

    QWidget* dialogParent_ = nullptr;
    QNetworkAccessManager* network_ = nullptr;
    bool checking_ = false;
};
