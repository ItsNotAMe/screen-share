#pragma once
#include "ui/RoomSessionWindow.h"
#include "shared/RoomProfile.h"
#include "room/qt/RoomDirectory.h"
class QLineEdit;
class QTableWidget;
class QCheckBox;
class QComboBox;
class QLabel;

class RoomBrowserWindow final : public QWidget {
public:
    RoomBrowserWindow(QUrl origin, QtRoomSession::Factory = screenshare::media::WindowsRoomRuntimeFactory,
                      bool diagnosticLoopback = false, QString profileFile = {}, bool enumerateSources = true);
    ~RoomBrowserWindow() override;
    RoomSessionWindow* activeSession() const { return active_.get(); }
    screenshare::room::qt::RoomDirectory& directory() { return directory_; }
    std::function<void()> closed;
    // An application shell can present these existing widgets as pages.
    // Without a presenter the standalone embedding contract remains available.
    std::function<void(QWidget*)> presentPage;
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void closeEvent(QCloseEvent*) override;
private:
    void Launch(bool host);
    void Refresh(const screenshare::room::qt::RoomDirectory::Status&);
    QUrl origin_;
    QtRoomSession::Factory factory_;
    bool loopback_, closing_ = false, closedNotified_ = false;
    RoomProfile profile_;
    screenshare::room::qt::RoomDirectory directory_;
    std::unique_ptr<RoomSessionWindow> active_;
    QLineEdit *nickname_, *name_, *roomId_, *password_;
    QCheckBox* public_;
    QComboBox *source_, *audio_;
    QTableWidget* rooms_;
    QLabel *status_, *error_;
    QPushButton* joinSelected_;
    QPushButton* retry_;
};
int RunRoomBrowserWindow(const QUrl& origin);
