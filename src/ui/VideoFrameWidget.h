#pragma once

#include "core/ScreenShareSession.h"

#include <QtGui/QImage>
#include <QtWidgets/QWidget>

class D3DVideoSurface;

class VideoFrameWidget final : public QWidget {
public:
    explicit VideoFrameWidget(QWidget* parent = nullptr);

    void setStatusText(const QString& text);
    void setVideoFrame(const screenshare::SessionEvent::VideoFrame& frame);
    void setSmoothScaling(bool enabled);
    void clearFrame();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    D3DVideoSurface* d3dSurface_ = nullptr;
    QImage image_;
    QString statusText_;
    bool smoothScaling_ = true;
    bool d3dUnavailable_ = false;
};
