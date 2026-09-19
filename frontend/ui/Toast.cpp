#include "ui/Toast.h"
#include "ui/UiStyle.h"
#include <QStylePainter>
#include <QStyleOption>

#include <QtCore/QAbstractAnimation>
#include <QtCore/QEvent>
#include <QtCore/QPropertyAnimation>
#include <QtCore/QTimer>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>

#include <algorithm>

void Toast::show(QWidget* host, const QString& message, int visibleMs)
{
    if (host == nullptr) {
        return;
    }
    for(auto* existing:host->findChildren<QWidget*>("Toast",Qt::FindDirectChildrenOnly)){existing->hide();existing->deleteLater();}
    auto* toast = new Toast(host, message, visibleMs);
    toast->start();
}

Toast::Toast(QWidget* host, const QString& message, int visibleMs)
    : QWidget(host,Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput)
    , host_(host)
    , visibleMs_(visibleMs)
{
    setObjectName("Toast");
    setStyleSheet(uiStyleSheet());
    setFont(host->font());
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_DeleteOnClose, true);
    setFocusPolicy(Qt::NoFocus);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(18, 12, 18, 12);
    label_ = new QLabel(message, this);
    label_->setObjectName("ToastText");
    label_->setWordWrap(true);
    label_->setFixedWidth(std::min(std::max(160,std::min(480,host->width()-80)),label_->fontMetrics().horizontalAdvance(message)+2));
    layout->addWidget(label_);

    setWindowOpacity(0.0);

    host_->installEventFilter(this);
}

void Toast::start()
{
    adjustSize();
    reposition();
    raise();
    QWidget::show();

    auto* fadeIn = new QPropertyAnimation(this, "windowOpacity", this);
    fadeIn->setDuration(160);
    fadeIn->setStartValue(0.0);
    fadeIn->setEndValue(1.0);
    fadeIn->start(QAbstractAnimation::DeleteWhenStopped);

    QTimer::singleShot(visibleMs_, this, [this] {
        auto* fadeOut = new QPropertyAnimation(this, "windowOpacity", this);
        fadeOut->setDuration(280);
        fadeOut->setStartValue(windowOpacity());
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
    move(host_->mapToGlobal(QPoint(std::max(0, x), std::max(0, y))));
}

bool Toast::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == host_ && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
        reposition();
    }
    if(watched==host_ && (event->type()==QEvent::Hide || event->type()==QEvent::WindowDeactivate))close();
    return QWidget::eventFilter(watched, event);
}

void Toast::paintEvent(QPaintEvent*) {
    QStyleOption option;option.initFrom(this);
    QStylePainter painter(this);painter.drawPrimitive(QStyle::PE_Widget,option);
}
