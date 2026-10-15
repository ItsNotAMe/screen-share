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
        const auto image=original.scaled(target,Qt::KeepAspectRatio,Qt::SmoothTransformation);
        painter.drawPixmap((width()-image.width())/2,(height()-image.height())/2,image);
    }
};
