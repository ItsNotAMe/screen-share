#include "ui/RoomApplication.h"
#include <QLayout>
#include <QStackedWidget>
#include <QTimer>
#include <algorithm>

RoomApplication::RoomApplication(QUrl origin, QtRoomSession::Factory factory,
    bool loopback, QString profileFile, bool enumerateSources) {
    Initialize();
    browser_ = std::make_unique<RoomBrowserWindow>(std::move(origin), std::move(factory),
        loopback, std::move(profileFile), enumerateSources);
    browser_->presentPage = [this](QWidget* page) { Present(page); };
    browser_->closed = [this] { Finish(); };
    Present(browser_.get());
}

RoomApplication::RoomApplication(RoomSessionConfig config, QtRoomSession::Factory factory, bool loopback) {
    Initialize();
    session_ = std::make_unique<RoomSessionWindow>(std::move(config), std::move(factory), loopback);
    session_->closed = [this] { Finish(); };
    Present(session_.get());
}

void RoomApplication::Initialize() {
    shell_.resize(1000, 820);
    shell_.setMinimumSize(740, 600);
    shell_.setCloseHandler([this] {
        if (finished_) return true;
        if (!closing_) {
            closing_ = true;
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
    auto* stack = shell_.findChild<QStackedWidget*>("AppPageStack");
    if (stack->indexOf(page) < 0) {
        // Reserve the normal title-bar hit area without changing page controls.
        if (auto* layout = page->layout()) {
            auto margins = layout->contentsMargins();
            margins.setTop(std::max(margins.top(), 44));
            layout->setContentsMargins(margins);
        }
        shell_.addPage(page);
    }
    shell_.setCurrentWidget(page);
    // A previously closed child remains explicitly hidden after setCurrentWidget.
    page->show();
    shell_.setWindowTitle(page->windowTitle());
    awake_.setActive(page != browser_.get());
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
    shell_.setCloseHandler({});
    // unique_ptr-owned pages remove themselves from the shell's QObject tree.
    // They are destroyed before the shell; deferred callbacks have this context.
}
