#include "ui/Toast.h"

#include <QtCore/QAbstractAnimation>
#include <QtCore/QEvent>
#include <QtCore/QPropertyAnimation>
#include <QtCore/QTimer>
#include <QtWidgets/QGraphicsOpacityEffect>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>

#include <algorithm>

void Toast::show(QWidget* host, const QString& message, int visibleMs)
{
    if (host == nullptr) {
        return;
    }
    auto* toast = new Toast(host, message, visibleMs);
    toast->start();
}

Toast::Toast(QWidget* host, const QString& message, int visibleMs)
    : QWidget(host)
    , host_(host)
    , visibleMs_(visibleMs)
{
    setObjectName("Toast");
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_DeleteOnClose, true);
    setFocusPolicy(Qt::NoFocus);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(18, 12, 18, 12);
    label_ = new QLabel(message, this);
    label_->setObjectName("ToastText");
    layout->addWidget(label_);

    opacity_ = new QGraphicsOpacityEffect(this);
    opacity_->setOpacity(0.0);
    setGraphicsEffect(opacity_);

    host_->installEventFilter(this);
}

void Toast::start()
{
    adjustSize();
    reposition();
    raise();
    QWidget::show();

    auto* fadeIn = new QPropertyAnimation(opacity_, "opacity", this);
    fadeIn->setDuration(160);
    fadeIn->setStartValue(0.0);
    fadeIn->setEndValue(1.0);
    fadeIn->start(QAbstractAnimation::DeleteWhenStopped);

    QTimer::singleShot(visibleMs_, this, [this] {
        auto* fadeOut = new QPropertyAnimation(opacity_, "opacity", this);
        fadeOut->setDuration(280);
        fadeOut->setStartValue(1.0);
        fadeOut->setEndValue(0.0);
        connect(fadeOut, &QPropertyAnimation::finished, this, &QWidget::close);
        fadeOut->start(QAbstractAnimation::DeleteWhenStopped);
    });
}

void Toast::reposition()
{
    if (host_ == nullptr) {
        return;
    }
    adjustSize();
    const int x = (host_->width() - width()) / 2;
    const int y = host_->height() - height() - 28;
    move(std::max(0, x), std::max(0, y));
}

bool Toast::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == host_ && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
        reposition();
    }
    return QWidget::eventFilter(watched, event);
}
