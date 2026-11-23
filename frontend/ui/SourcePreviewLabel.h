#pragma once
#include <QLabel>
#include <QPainter>

class SourcePreviewLabel : public QLabel {
public:
    using QLabel::QLabel;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);painter.fillRect(rect(),QColor("#080e0b"));
        if(pixmap().isNull()) {painter.setPen(QColor("#a3b5af"));painter.drawText(rect().adjusted(20,20,-20,-20),Qt::AlignCenter|Qt::TextWordWrap,text());return;}
        const auto original=pixmap();const auto target=original.width()<=80?QSize(80,80):size();
        const auto fitted=original.size().scaled(target,Qt::KeepAspectRatio);
        // Paint the original pixels directly. A logical-size intermediate loses
        // detail when Qt then enlarges it for a high-DPI backing surface.
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawPixmap(QRect(QPoint((width()-fitted.width())/2,(height()-fitted.height())/2),fitted),original);
    }
};
