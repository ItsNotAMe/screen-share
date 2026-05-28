#pragma once

#include <QtWidgets/QWidget>

class QPushButton;
class QFrame;
class QStackedWidget;
class QShowEvent;

class AppShellWindow final : public QWidget {
public:
    explicit AppShellWindow(QWidget* parent = nullptr);

    int addPage(QWidget* page);
    void setCurrentWidget(QWidget* page);
    void setChromeVisible(bool visible);

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
#endif

    QFrame* frame_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QWidget* titleBar_ = nullptr;
    QPushButton* maximizeButton_ = nullptr;
};
