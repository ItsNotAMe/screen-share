#pragma once
#include "ui/AppShellWindow.h"
#include "ui/RoomBrowserWindow.h"
#include "ui/ScreenAwakeGuard.h"

// Uses the normal application chrome and the shared room pages. Ownership stays
// here until both media and directory shutdown finish; no nested event loop.
class RoomApplication final : public QObject {
public:
    RoomApplication(QUrl origin,
        QtRoomSession::Factory = screenshare::media::WindowsRoomRuntimeFactory,
        bool diagnosticLoopback = false, QString profileFile = {}, bool enumerateSources = true);
    RoomApplication(RoomSessionConfig,
        QtRoomSession::Factory = screenshare::media::WindowsRoomRuntimeFactory,
        bool diagnosticLoopback = false);
    ~RoomApplication() override;
    void show();
    AppShellWindow& window() { return shell_; }
    RoomBrowserWindow* browser() const { return browser_.get(); }
    RoomSessionWindow* session() const { return browser_ ? browser_->activeSession() : session_.get(); }
    bool keepingScreenAwake() const { return awake_.active(); }
    bool finished() const { return finished_; }
    std::function<void()> closed;
private:
    void Initialize();
    void Present(QWidget*);
    void Finish();
    AppShellWindow shell_;
    ScreenAwakeGuard awake_;
    std::unique_ptr<RoomBrowserWindow> browser_;
    std::unique_ptr<RoomSessionWindow> session_;
    bool closing_ = false, finished_ = false;
};
