#pragma once

#include "core/ScreenShareSession.h"

#include <QtGui/QImage>
#include <QtWidgets/QWidget>

#include <atomic>
#include <cstdint>
#include <memory>

class D3DVideoSurface;
class D3DFramePresenter;

class VideoFrameWidget final : public QWidget {
public:
    struct PresentationStats {
        std::uint64_t enqueuedFrames = 0;
        std::uint64_t presentedFrames = 0;
        std::uint64_t droppedFrames = 0;
        std::uint32_t queuedFrames = 0;
        double averagePresentMs = 0.0;
        double maxPresentMs = 0.0;
        double lastPresentMs = 0.0;
    };

    explicit VideoFrameWidget(QWidget* parent = nullptr);
    ~VideoFrameWidget() override;

    void setStatusText(const QString& text);
    bool setVideoFrame(screenshare::SessionEvent::VideoFrame frame);
    bool presentVideoFrameAsync(screenshare::SessionEvent::VideoFrame frame);
    void showVideoSurface();
    void setSmoothScaling(bool enabled);
    [[nodiscard]] std::uint64_t presentedFrameCount() const;
    [[nodiscard]] PresentationStats presentationStats() const;
    void clearFrame();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateD3DTarget();

    D3DVideoSurface* d3dSurface_ = nullptr;
    std::shared_ptr<D3DFramePresenter> framePresenter_;
    QImage image_;
    QString statusText_;
    bool smoothScaling_ = true;
    std::atomic<std::uintptr_t> d3dHwnd_{0};
    std::atomic<std::uint32_t> d3dWidth_{1};
    std::atomic<std::uint32_t> d3dHeight_{1};
    std::atomic_bool smoothScalingAtomic_{true};
};
