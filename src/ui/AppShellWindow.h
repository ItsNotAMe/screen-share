#pragma once

#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <functional>

class QPushButton;
class QFrame;
class QStackedWidget;
class QShowEvent;

class AppShellWindow final : public QWidget {
public:
    explicit AppShellWindow(QWidget* parent = nullptr);
    ~AppShellWindow() override;

    int addPage(QWidget* page);
    void setCurrentWidget(QWidget* page);
    void setChromeVisible(bool visible);
    void showToast(const QString& message);
    // Handler invoked when the host presses the global panic-revoke hotkey
    // (Ctrl+Alt+Shift+F12). Wired to instantly revoke any active remote control.
    void setPanicHotkeyHandler(std::function<void()> handler);

protected:
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
    std::function<void()> panicHotkeyHandler_;
#ifdef _WIN32
    bool panicHotkeyRegistered_ = false;
#endif
};
