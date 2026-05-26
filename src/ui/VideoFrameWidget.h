#pragma once

#include "core/ScreenShareSession.h"

#include <QtGui/QImage>
#include <QtWidgets/QWidget>

class VideoFrameWidget final : public QWidget {
public:
    explicit VideoFrameWidget(QWidget* parent = nullptr);

    void setStatusText(const QString& text);
    void setVideoFrame(const screenshare::SessionEvent::VideoFrame& frame);
    void setSmoothScaling(bool enabled);
    void clearFrame();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage image_;
    QString statusText_;
    bool smoothScaling_ = true;
};
