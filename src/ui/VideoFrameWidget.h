#pragma once

#include "core/ScreenShareSession.h"

#include <QtGui/QImage>
#include <QtWidgets/QWidget>

#include <atomic>
#include <cstdint>
#include <functional>
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

    // Remote control: when capture is active, mouse/keyboard events over the
    // preview are mapped to normalized frame coordinates and forwarded to the
    // handler (which the watch window sends to the host).
    void setRemoteInputHandler(std::function<void(const screenshare::RemoteInputEvent&)> handler);
    void setControlCapture(bool enabled, bool mouse, bool keyboard);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

private:
    void updateD3DTarget();
    [[nodiscard]] bool mapToNormalized(const QPoint& pos, float& normX, float& normY) const;
    void emitMouseButton(QMouseEvent* event, bool pressed);
    void emitKey(QKeyEvent* event, bool pressed);

    D3DVideoSurface* d3dSurface_ = nullptr;
    std::shared_ptr<D3DFramePresenter> framePresenter_;
    QImage image_;
    QString statusText_;
    bool smoothScaling_ = true;
    std::atomic<std::uintptr_t> d3dHwnd_{0};
    std::atomic<std::uint32_t> d3dWidth_{1};
    std::atomic<std::uint32_t> d3dHeight_{1};
    std::atomic_bool smoothScalingAtomic_{true};
    std::atomic<int> frameWidth_{0};
    std::atomic<int> frameHeight_{0};
    std::function<void(const screenshare::RemoteInputEvent&)> inputHandler_;
    bool controlActive_ = false;
    bool controlMouse_ = false;
    bool controlKeyboard_ = false;
};
