#include "ui/RoomApplication.h"
#include <QLayout>
#include <QStackedWidget>
#include <QTimer>
#include <algorithm>

RoomApplication::RoomApplication(QUrl origin, QtRoomSession::Factory factory,
    bool loopback, QString profileFile, bool enumerateSources, bool normalHome) : origin_(origin) {
    Initialize();
    browser_ = std::make_unique<RoomBrowserWindow>(std::move(origin), std::move(factory),
        loopback, std::move(profileFile), enumerateSources);
    browser_->presentPage = [this](QWidget* page) { Present(page); };
    browser_->closed = [this] { Finish(); };
    shell_.setProfileName(browser_->profileName());
    browser_->profileChanged = [this] { shell_.setProfileName(browser_->profileName()); };
    shell_.openProfile = [this] { browser_->OpenPreferences(false, &shell_); };
    shell_.openSettings = [this] { browser_->OpenPreferences(true, &shell_); };
    if (normalHome) {
        HomeWindow::Actions actions;
        actions.createRoom = [this] { OpenRoom(true); };
        actions.joinRoom = [this] { OpenRoom(false); };
        actions.openRoom = [this](const QString& id) {
            if(closing_ || browser_->activeSession())return;
            browser_->OpenJoin(id);
            browser_->JoinListedRoom(id);
            if(!browser_->activeSession())Present(browser_.get());
        };
        actions.requestRooms = [this] {
            // Only an explicit Refresh replaces the live subscription.
            if (browser_->directory().status().phase == screenshare::room::qt::RoomDirectory::Phase::Connecting) return;
            browser_->directory().Stop();
            browser_->directory().Start(origin_);
        };
        home_ = std::make_unique<HomeWindow>(std::move(actions));
        browser_->back = browser_->returnFromSession = [this] { ShowHome(); };
        browser_->ShowBackButton();
        browser_->directoryChanged = [this](const auto& state) {
            using Directory = screenshare::room::qt::RoomDirectory;
            QVector<HomeActiveRoom> rooms;
            for (const auto& room : state.rooms)
                rooms.push_back({room.id, room.name, room.viewers, room.password, 0, room.status == "open", room.hostNickname});
            home_->setPushedRooms(rooms, state.phase == Directory::Phase::Ready ? QString{} :
                state.phase == Directory::Phase::Failed ? "Room list unavailable. Reconnect to try again." : "Connecting to room list…");
        };
        ShowHome();
    } else Present(browser_.get());
}

void RoomApplication::ShowHome() {
    if (closing_ || !home_) return;
    Present(home_.get());
    browser_->directory().Start(origin_);
}
void RoomApplication::OpenRoom(bool host, const QString& roomId) {
    if (closing_ || !browser_ || browser_->activeSession()) return;
    if (host) browser_->OpenCreate(); else browser_->OpenJoin(roomId);
    Present(browser_.get());
}

RoomApplication::RoomApplication(RoomSessionConfig config, QtRoomSession::Factory factory, bool loopback) {
    Initialize();
    session_ = std::make_unique<RoomSessionWindow>(std::move(config), std::move(factory), loopback);
    session_->closed = [this] { Finish(); };
    Present(session_.get());
}

void RoomApplication::Initialize() {
    shell_.hasActiveSession = [this] { return session() != nullptr || closing_; };
    shell_.setPanicHotkeyHandler([this] {
        if (session_) session_->revokeControl();
        if (browser_ && browser_->activeSession()) browser_->activeSession()->revokeControl();
    });
    shell_.resize(1000, 820);
    shell_.setMinimumSize(740, 600);
    shell_.setCloseHandler([this] {
        if (finished_) return true;
        if (!closing_) {
            closing_ = true;
            if (home_) home_->setEnabled(false);
            // Disable actions during drain, but keep chrome/event processing live.
            if (browser_) {
                browser_->setEnabled(false);
                if (browser_->activeSession()) browser_->activeSession()->setEnabled(false);
                browser_->close();
            }
            else if (session_) { session_->setEnabled(false); session_->close(); }
        }
        return false;
    });
}

void RoomApplication::Present(QWidget* page) {
    if (closing_ || finished_) return;
    if (browser_) browser_->keepDirectoryOnHide = page == home_.get();
    auto* stack = shell_.findChild<QStackedWidget*>("AppPageStack");
    if (stack->indexOf(page) < 0) {
        shell_.addPage(page);
    }
    shell_.setCurrentWidget(page);
    // A previously closed child remains explicitly hidden after setCurrentWidget.
    page->show();
    shell_.setWindowTitle(page->windowTitle());
    awake_.setActive(page != browser_.get() && page != home_.get());
}

void RoomApplication::Finish() {
    if (finished_) return;
    finished_ = true;
    awake_.setActive(false);
    // Never recurse through QWidget::closeEvent or delete a page in its callback.
    QTimer::singleShot(0, this, [this] {
        shell_.close();
        if (closed) closed();
    });
}

void RoomApplication::show() { if (!finished_) shell_.show(); }

RoomApplication::~RoomApplication() {
    shell_.hasActiveSession = {};
    shell_.setPanicHotkeyHandler({});
    shell_.setCloseHandler({});
    // unique_ptr-owned pages remove themselves from the shell's QObject tree.
    // They are destroyed before the shell; deferred callbacks have this context.
}
