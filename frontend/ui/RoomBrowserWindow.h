#pragma once
#include "ui/RoomSessionWindow.h"
#include "shared/RoomProfile.h"
#include "room/qt/RoomDirectory.h"
class QLineEdit;
class RoomDirectoryWidget;
class QCheckBox;
class QComboBox;
class QLabel;
class AppShellWindow;
class QSpinBox;
class QBoxLayout;
class QVBoxLayout;
class QListWidget;
class QThread;

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
    std::function<void()> back;
    std::function<void()> returnFromSession;
    std::function<void(const screenshare::room::qt::RoomDirectory::Status&)> directoryChanged;
    void OpenCreate();
    void OpenJoin(const QString& roomId = {});
    void ShowBackButton();
    QString profileName() const { return profile_.nickname(); }
    void OpenPreferences(bool playback, QWidget* owner = nullptr);
    std::function<void()> profileChanged;
    bool keepDirectoryOnHide = false; // Shared home/form navigation keeps one subscription.
protected:
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void closeEvent(QCloseEvent*) override;
private:
    void Launch(bool host);
    void Refresh(const screenshare::room::qt::RoomDirectory::Status&);
    void RefreshSources();
    void RefreshSourceCards(bool selectFirst = false);
    void LoadSourcePreviews();
    void JoinListedRoom(const QString& id);
    void PromptPassword();
    QUrl origin_;
    QtRoomSession::Factory factory_;
    bool loopback_, closing_ = false, closedNotified_ = false;
    bool enumerateSources_ = true;
    RoomProfile profile_;
    screenshare::room::qt::RoomDirectory directory_;
    std::unique_ptr<RoomSessionWindow> active_;
    QLineEdit *name_, *roomId_, *password_;
    QWidget *createPanel_, *joinPanel_;
    QLabel* heading_;
    QComboBox *preset_, *resolution_, *fps_, *bitrate_;
    QSpinBox* viewerLimit_;
    QBoxLayout* createColumns_;
    QCheckBox* public_;
    QComboBox *source_, *audio_;
    RoomDirectoryWidget* rooms_;
    QLabel* error_;
    QWidget *passwordPanel_, *directoryPanel_;
    bool passwordRetry_ = false;
    bool passwordRejected_ = false;
    QVBoxLayout *detailsBody_, *joinBody_;
    QListWidget* sourceCards_;
    bool windowSources_ = false;
    QThread* previewThread_ = nullptr;
    uint64_t previewRevision_ = 0;
};
int RunRoomBrowserWindow(const QUrl& origin, bool normalHome = false,
                        std::function<void(AppShellWindow&)> initializeShell = {});
