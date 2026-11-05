#pragma once

#include <QtCore/QString>
#include <QtCore/QByteArray>
#include <QtWidgets/QWidget>

#include <functional>
#include <memory>

class QPushButton;
class QFrame;
class QStackedWidget;
class QShowEvent;
class QSettings;

class AppShellWindow final : public QWidget {
public:
    explicit AppShellWindow(QWidget* parent = nullptr);
    ~AppShellWindow() override;

    int addPage(QWidget* page);
    void setCurrentWidget(QWidget* page);
    void setChromeVisible(bool visible);
    void setProfileName(const QString& name);
    std::function<void()> openProfile;
    std::function<void()> openSettings;
    // Includes joining and worker shutdown, even when settings cover the room.
    std::function<bool()> hasActiveSession;
    void showToast(const QString& message);
    // Handler invoked when the host presses the global panic-revoke hotkey
    // (Ctrl+Alt+Shift+F12). Wired to instantly revoke any active remote control.
    void setPanicHotkeyHandler(std::function<void()> handler);
    // Returning false defers window closure while the page drains its workers.
    void setCloseHandler(std::function<bool()> handler);
    // Normal application startup opts in; diagnostics/tests stay independent.
    // An explicit INI path isolates a profile or test from the user's defaults.
    void enableWindowPlacementPersistence(const QString& settingsFile = {});

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
#ifdef _WIN32
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
#endif

private:
    QWidget* buildTitleBar();
    QPushButton* windowButton(const char* iconName, const QString& objectName);
    void toggleMaximized();
    void updateChromeState();
    bool isTitleControl(const QWidget* widget) const;
    bool isNativeMaximized() const;
    void captureWindowPlacement();
    void saveWindowPlacement();
#ifdef _WIN32
    void applyNativeWindowStyle();
    bool handleMinMaxInfo(void* message, qintptr* result);
    void registerPanicHotkey();
    void unregisterPanicHotkey();
#endif

    QFrame* frame_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QWidget* titleBar_ = nullptr;
    QPushButton* maximizeButton_ = nullptr;
    QPushButton* profileButton_ = nullptr;
    bool initialFocusSet_ = false;
    std::function<void()> panicHotkeyHandler_;
    std::function<bool()> closeHandler_;
    std::unique_ptr<QSettings> placementSettings_;
    QByteArray windowedPlacement_;
    bool placementShown_ = false, placementSaved_ = false;
    bool maximizeOnFirstShow_ = false;
#ifdef _WIN32
    bool panicHotkeyRegistered_ = false;
#endif
};
