#include "ui/AppShellWindow.h"

#include "ui/Toast.h"
#include "ui/UiStyle.h"

#include <QtCore/QByteArray>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QIODevice>
#include <QtCore/QRectF>
#include <QtGui/QIcon>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtSvg/QSvgRenderer>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>

#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>
#endif

namespace {

QPixmap renderSvgResource(const QString& path, const QSize& size, const QString& color = QString())
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QByteArray svg = file.readAll();
    if (!color.isEmpty()) {
        svg.replace("currentColor", color.toUtf8());
    }

    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) {
        return {};
    }

    QPixmap pixmap(size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(size)));
    return pixmap;
}

#ifdef _WIN32
// Global panic-revoke hotkey: Ctrl+Alt+Shift+F12. Chosen for being extremely
// unlikely to collide with app or OS shortcuts. Registered against the shell
// window so the host can instantly drop remote control from anywhere — the OS
// delivers WM_HOTKEY on the UI thread even while a viewer is flooding input.
constexpr int kPanicHotkeyId = 0xB001;
constexpr unsigned kPanicHotkeyModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
constexpr unsigned kPanicHotkeyVk = VK_F12;

void preferRoundedWindows(HWND hwnd)
{
    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    constexpr DWORD DwmwaWindowCornerPreference = 33;
    constexpr DWORD DwmwcpRound = 2;

    HMODULE module = LoadLibraryW(L"dwmapi.dll");
    if (module == nullptr) {
        return;
    }
    auto* setWindowAttribute = reinterpret_cast<DwmSetWindowAttributeFn>(
        GetProcAddress(module, "DwmSetWindowAttribute"));
    if (setWindowAttribute != nullptr) {
        const DWORD preference = DwmwcpRound;
        setWindowAttribute(hwnd, DwmwaWindowCornerPreference, &preference, sizeof(preference));
    }
    FreeLibrary(module);
}
#endif

} // namespace

AppShellWindow::AppShellWindow(QWidget* parent) : QWidget(parent)
{
    setObjectName("AppShellWindow");
    setWindowTitle("ScreenShare");
    setWindowIcon(QIcon(QStringLiteral(":/screenshare/brand/screenshare-mark.svg")));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint);
    setStyleSheet(uiStyleSheet());

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    frame_ = new QFrame;
    frame_->setObjectName("AppShellFrame");
    auto* frameLayout = new QGridLayout(frame_);
    frameLayout->setContentsMargins(1, 1, 1, 1);
    frameLayout->setSpacing(0);

    stack_ = new QStackedWidget;
    stack_->setObjectName("AppPageStack");
    frameLayout->addWidget(stack_, 0, 0);

    QWidget* titleBar = buildTitleBar();
    frameLayout->addWidget(titleBar, 0, 0, Qt::AlignTop | Qt::AlignRight);
    titleBar->raise();

    root->addWidget(frame_, 1);
}

AppShellWindow::~AppShellWindow()
{
#ifdef _WIN32
    unregisterPanicHotkey();
#endif
}

void AppShellWindow::setPanicHotkeyHandler(std::function<void()> handler)
{
    panicHotkeyHandler_ = std::move(handler);
}

int AppShellWindow::addPage(QWidget* page)
{
    return stack_->addWidget(page);
}

void AppShellWindow::setCurrentWidget(QWidget* page)
{
    stack_->setCurrentWidget(page);
}

void AppShellWindow::showToast(const QString& message)
{
    Toast::show(this, message);
}

void AppShellWindow::setChromeVisible(bool visible)
{
    if (titleBar_ != nullptr) {
        titleBar_->setVisible(visible);
    }
    if (frame_ != nullptr) {
        frame_->setProperty("chromeHidden", !visible);
        frame_->style()->unpolish(frame_);
        frame_->style()->polish(frame_);
    }
}

QWidget* AppShellWindow::buildTitleBar()
{
    auto* frame = new QWidget;
    frame->setObjectName("AppTitleBar");
    frame->setFixedSize(140, 38);
    titleBar_ = frame;

    auto* layout = new QHBoxLayout(frame);
    layout->setContentsMargins(0, 0, 8, 0);
    layout->setSpacing(4);

    auto* minimizeButton = windowButton("window-minimize", "WindowControlButton");
    connect(minimizeButton, &QPushButton::clicked, this, [this] {
#ifdef _WIN32
        ShowWindow(reinterpret_cast<HWND>(winId()), SW_MINIMIZE);
#else
        showMinimized();
#endif
    });
    layout->addWidget(minimizeButton);

    maximizeButton_ = windowButton("window-maximize", "WindowControlButton");
    connect(maximizeButton_, &QPushButton::clicked, this, [this] {
        toggleMaximized();
    });
    layout->addWidget(maximizeButton_);

    auto* closeButton = windowButton("window-close", "WindowCloseButton");
    connect(closeButton, &QPushButton::clicked, this, [this] {
        close();
    });
    layout->addWidget(closeButton);

    return frame;
}

QPushButton* AppShellWindow::windowButton(const char* iconName, const QString& objectName)
{
    auto* button = new QPushButton;
    button->setObjectName(objectName);
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedSize(42, 32);
    const QPixmap pixmap = renderSvgResource(
        QStringLiteral(":/screenshare/ui/icons/%1.svg").arg(QString::fromUtf8(iconName)),
        QSize(18, 18),
        QStringLiteral("#d7e0dd"));
    if (!pixmap.isNull()) {
        button->setIcon(QIcon(pixmap));
        button->setIconSize(QSize(18, 18));
    }
    return button;
}

void AppShellWindow::toggleMaximized()
{
#ifdef _WIN32
    applyNativeWindowStyle();
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd != nullptr) {
        ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        updateChromeState();
        return;
    }
#endif
    isMaximized() ? showNormal() : showMaximized();
    updateChromeState();
}

void AppShellWindow::updateChromeState()
{
    const bool maximized = isNativeMaximized();
    if (maximizeButton_ != nullptr) {
        const char* iconName = maximized ? "window-restore" : "window-maximize";
        const QPixmap pixmap = renderSvgResource(
            QStringLiteral(":/screenshare/ui/icons/%1.svg").arg(QString::fromUtf8(iconName)),
            QSize(18, 18),
            QStringLiteral("#d7e0dd"));
        maximizeButton_->setIcon(QIcon(pixmap));
    }
    if (frame_ != nullptr) {
        frame_->setProperty("maximized", maximized);
        frame_->style()->unpolish(frame_);
        frame_->style()->polish(frame_);
    }
}

bool AppShellWindow::isTitleControl(const QWidget* widget) const
{
    const QWidget* current = widget;
    while (current != nullptr && current != titleBar_) {
        if (qobject_cast<const QPushButton*>(current) != nullptr) {
            return true;
        }
        current = current->parentWidget();
    }
    return false;
}

bool AppShellWindow::isNativeMaximized() const
{
#ifdef _WIN32
    const HWND hwnd = reinterpret_cast<HWND>(const_cast<AppShellWindow*>(this)->winId());
    return hwnd != nullptr && IsZoomed(hwnd) != FALSE;
#else
    return isMaximized();
#endif
}

void AppShellWindow::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        updateChromeState();
    }
}

void AppShellWindow::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
#ifdef _WIN32
    applyNativeWindowStyle();
    registerPanicHotkey();
#endif
    updateChromeState();
}

#ifdef _WIN32
void AppShellWindow::applyNativeWindowStyle()
{
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd == nullptr) {
        return;
    }

    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style |= WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);
    preferRoundedWindows(hwnd);
    SetWindowPos(
        hwnd,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
}

void AppShellWindow::registerPanicHotkey()
{
    if (panicHotkeyRegistered_) {
        return;
    }
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd == nullptr) {
        return;
    }
    // Best-effort: if the combination is already taken system-wide, the panic
    // key is simply unavailable (the in-app revoke toggles still work). Never
    // fail the session over it.
    if (RegisterHotKey(hwnd, kPanicHotkeyId, kPanicHotkeyModifiers, kPanicHotkeyVk) != 0) {
        panicHotkeyRegistered_ = true;
    }
}

void AppShellWindow::unregisterPanicHotkey()
{
    if (!panicHotkeyRegistered_) {
        return;
    }
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd != nullptr) {
        UnregisterHotKey(hwnd, kPanicHotkeyId);
    }
    panicHotkeyRegistered_ = false;
}

bool AppShellWindow::handleMinMaxInfo(void* message, qintptr* result)
{
    MSG* msg = static_cast<MSG*>(message);
    auto* info = reinterpret_cast<MINMAXINFO*>(msg->lParam);
    if (info == nullptr) {
        return false;
    }

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    const HMONITOR handle = MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST);
    if (handle == nullptr || !GetMonitorInfo(handle, &monitor)) {
        return false;
    }

    const RECT work = monitor.rcWork;
    const RECT full = monitor.rcMonitor;
    info->ptMaxPosition.x = work.left - full.left;
    info->ptMaxPosition.y = work.top - full.top;
    info->ptMaxSize.x = work.right - work.left;
    info->ptMaxSize.y = work.bottom - work.top;
    *result = 0;
    return true;
}

bool AppShellWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(eventType);
    MSG* msg = static_cast<MSG*>(message);
    if (msg == nullptr) {
        return QWidget::nativeEvent(eventType, message, result);
    }

    switch (msg->message) {
    case WM_HOTKEY:
        if (msg->wParam == kPanicHotkeyId) {
            if (panicHotkeyHandler_) {
                panicHotkeyHandler_();
            }
            *result = 0;
            return true;
        }
        break;
    case WM_GETMINMAXINFO:
        return handleMinMaxInfo(message, result);
    case WM_NCCALCSIZE:
        if (msg->wParam == TRUE) {
            *result = 0;
            return true;
        }
        break;
    case WM_SIZE:
        updateChromeState();
        break;
    case WM_NCHITTEST: {
        const QPoint globalPos(GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam));
        const QPoint localPos = mapFromGlobal(globalPos);
        QWidget* child = childAt(localPos);
        if (isTitleControl(child)) {
            *result = HTCLIENT;
            return true;
        }

        const LRESULT nativeHit = DefWindowProc(msg->hwnd, msg->message, msg->wParam, msg->lParam);
        if (nativeHit != HTCLIENT && nativeHit != HTMINBUTTON && nativeHit != HTMAXBUTTON && nativeHit != HTCLOSE) {
            *result = nativeHit;
            return true;
        }

        if (!IsZoomed(msg->hwnd)) {
            int border = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            if (border < 8) {
                border = 8;
            }

            const bool left = localPos.x() >= 0 && localPos.x() < border;
            const bool right = localPos.x() <= width() && localPos.x() >= width() - border;
            const bool top = localPos.y() >= 0 && localPos.y() < border;
            const bool bottom = localPos.y() <= height() && localPos.y() >= height() - border;
            if (top && left) {
                *result = HTTOPLEFT;
                return true;
            }
            if (top && right) {
                *result = HTTOPRIGHT;
                return true;
            }
            if (bottom && left) {
                *result = HTBOTTOMLEFT;
                return true;
            }
            if (bottom && right) {
                *result = HTBOTTOMRIGHT;
                return true;
            }
            if (left) {
                *result = HTLEFT;
                return true;
            }
            if (right) {
                *result = HTRIGHT;
                return true;
            }
            if (top) {
                *result = HTTOP;
                return true;
            }
            if (bottom) {
                *result = HTBOTTOM;
                return true;
            }
        }

        constexpr int dragHeight = 54;
        if (localPos.y() >= 0 && localPos.y() < dragHeight) {
            if (!isTitleControl(child)) {
                *result = HTCAPTION;
                return true;
            }
        }
        break;
    }
    default:
        break;
    }

    return QWidget::nativeEvent(eventType, message, result);
}
#endif
