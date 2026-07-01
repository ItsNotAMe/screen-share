#include "ui/VideoFrameWidget.h"

#include "render/Nv12D3D11Presenter.h"

#include <QtGui/QPainter>
#include <QtGui/QPaintEngine>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QResizeEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QSizePolicy>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

class D3DFramePresenter final {
public:
    D3DFramePresenter()
        : worker_([this] {
              run();
          })
    {
    }

    ~D3DFramePresenter()
    {
        {
            std::scoped_lock lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    D3DFramePresenter(const D3DFramePresenter&) = delete;
    D3DFramePresenter& operator=(const D3DFramePresenter&) = delete;

    void enqueue(
        HWND hwnd,
        std::uint32_t width,
        std::uint32_t height,
        bool smoothScaling,
        screenshare::SessionEvent::VideoFrame frame)
    {
        {
            std::scoped_lock lock(mutex_);
            hwnd_ = hwnd;
            width_ = width;
            height_ = height;
            smoothScaling_ = smoothScaling;
            if (pendingFrame_.has_value()) {
                droppedFrames_.fetch_add(1, std::memory_order_relaxed);
            }
            pendingFrame_ = std::move(frame);
            enqueuedFrames_.fetch_add(1, std::memory_order_relaxed);
            queuedFrames_.store(1, std::memory_order_release);
        }
        condition_.notify_one();
    }

    void resize(HWND hwnd, std::uint32_t width, std::uint32_t height)
    {
        {
            std::scoped_lock lock(mutex_);
            hwnd_ = hwnd;
            width_ = width;
            height_ = height;
            resizePending_ = true;
        }
        condition_.notify_one();
    }

    void setSmoothScaling(bool enabled)
    {
        {
            std::scoped_lock lock(mutex_);
            smoothScaling_ = enabled;
            smoothPending_ = true;
        }
        condition_.notify_one();
    }

    void clear()
    {
        {
            std::scoped_lock lock(mutex_);
            pendingFrame_.reset();
            queuedFrames_.store(0, std::memory_order_release);
            clearPending_ = true;
        }
        condition_.notify_one();
    }

    [[nodiscard]] std::uint64_t presentedFrameCount() const noexcept
    {
        return presentedFrames_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] VideoFrameWidget::PresentationStats stats() const noexcept
    {
        VideoFrameWidget::PresentationStats snapshot;
        snapshot.enqueuedFrames = enqueuedFrames_.load(std::memory_order_relaxed);
        snapshot.presentedFrames = presentedFrames_.load(std::memory_order_relaxed);
        snapshot.droppedFrames = droppedFrames_.load(std::memory_order_relaxed);
        snapshot.queuedFrames = queuedFrames_.load(std::memory_order_acquire);
        snapshot.lastPresentMs =
            static_cast<double>(lastPresentMicros_.load(std::memory_order_relaxed)) / 1000.0;
        snapshot.maxPresentMs =
            static_cast<double>(maxPresentMicros_.load(std::memory_order_relaxed)) / 1000.0;

        const std::uint64_t presented = snapshot.presentedFrames;
        if (presented > 0) {
            snapshot.averagePresentMs =
                static_cast<double>(totalPresentMicros_.load(std::memory_order_relaxed)) /
                static_cast<double>(presented) /
                1000.0;
        }
        return snapshot;
    }

private:
    struct Work {
        HWND hwnd = nullptr;
        std::uint32_t width = 1;
        std::uint32_t height = 1;
        bool smoothScaling = true;
        bool resizePending = false;
        bool smoothPending = false;
        bool clearPending = false;
        std::optional<screenshare::SessionEvent::VideoFrame> frame;
    };

    static void recordMax(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept
    {
        std::uint64_t current = target.load(std::memory_order_relaxed);
        while (current < value &&
               !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
        }
    }

    void run()
    {
        screenshare::Nv12D3D11Presenter presenter;
        for (;;) {
            Work work;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] {
                    return stopping_ ||
                           pendingFrame_.has_value() ||
                           resizePending_ ||
                           smoothPending_ ||
                           clearPending_;
                });
                if (stopping_) {
                    break;
                }

                work.hwnd = hwnd_;
                work.width = width_;
                work.height = height_;
                work.smoothScaling = smoothScaling_;
                work.resizePending = resizePending_;
                work.smoothPending = smoothPending_;
                work.clearPending = clearPending_;
                work.frame = std::move(pendingFrame_);
                pendingFrame_.reset();
                queuedFrames_.store(0, std::memory_order_release);
                resizePending_ = false;
                smoothPending_ = false;
                clearPending_ = false;
            }

            try {
                if (work.hwnd == nullptr || IsWindow(work.hwnd) == 0) {
                    presenter.Reset();
                    continue;
                }

                presenter.Attach(work.hwnd);
                if (work.smoothPending) {
                    presenter.SetLinearSampling(work.smoothScaling);
                }
                if (work.resizePending) {
                    presenter.Resize(work.width, work.height);
                }
                if (work.clearPending) {
                    presenter.Clear();
                }
                if (work.frame) {
                    presenter.Resize(work.width, work.height);
                    presenter.SetLinearSampling(work.smoothScaling);
                    const auto presentStarted = std::chrono::steady_clock::now();
                    presenter.Present(screenshare::Nv12D3D11Presenter::FrameView{
                        work.frame->width,
                        work.frame->height,
                        work.frame->nv12.data(),
                        work.frame->nv12.size(),
                    });
                    const auto presentElapsed = std::chrono::steady_clock::now() - presentStarted;
                    const auto presentMicros = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(presentElapsed).count());
                    totalPresentMicros_.fetch_add(presentMicros, std::memory_order_relaxed);
                    lastPresentMicros_.store(presentMicros, std::memory_order_relaxed);
                    recordMax(maxPresentMicros_, presentMicros);
                    presentedFrames_.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (const std::exception&) {
                presenter.Reset();
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<screenshare::SessionEvent::VideoFrame> pendingFrame_;
    HWND hwnd_ = nullptr;
    std::uint32_t width_ = 1;
    std::uint32_t height_ = 1;
    bool smoothScaling_ = true;
    bool resizePending_ = false;
    bool smoothPending_ = false;
    bool clearPending_ = false;
    bool stopping_ = false;
    std::atomic<std::uint64_t> enqueuedFrames_{0};
    std::atomic<std::uint64_t> presentedFrames_{0};
    std::atomic<std::uint64_t> droppedFrames_{0};
    std::atomic<std::uint32_t> queuedFrames_{0};
    std::atomic<std::uint64_t> totalPresentMicros_{0};
    std::atomic<std::uint64_t> lastPresentMicros_{0};
    std::atomic<std::uint64_t> maxPresentMicros_{0};
    std::thread worker_;
};

class D3DVideoSurface final : public QWidget {
public:
    explicit D3DVideoSurface(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setObjectName("D3DVideoSurface");
        setAttribute(Qt::WA_DontCreateNativeAncestors);
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_PaintOnScreen);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAutoFillBackground(false);
        hide();
    }

    QPaintEngine* paintEngine() const override
    {
        return nullptr;
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
    }

    void paintEvent(QPaintEvent*) override
    {
    }

};

namespace {

int clampByte(int value)
{
    return std::clamp(value, 0, 255);
}

QImage nv12ToRgb(const screenshare::SessionEvent::VideoFrame& frame)
{
    if (frame.width <= 0 || frame.height <= 0) {
        return {};
    }

    const qsizetype lumaBytes = static_cast<qsizetype>(frame.width) * frame.height;
    const qsizetype requiredBytes = lumaBytes + lumaBytes / 2;
    if (static_cast<qsizetype>(frame.nv12.size()) < requiredBytes) {
        return {};
    }

    QImage image(frame.width, frame.height, QImage::Format_RGB888);
    if (image.isNull()) {
        return {};
    }

    const auto* yPlane = frame.nv12.data();
    const auto* uvPlane = yPlane + lumaBytes;
    for (int y = 0; y < frame.height; ++y) {
        auto* output = image.scanLine(y);
        const auto* yRow = yPlane + static_cast<qsizetype>(y) * frame.width;
        const auto* uvRow = uvPlane + static_cast<qsizetype>(y / 2) * frame.width;
        for (int x = 0; x < frame.width; ++x) {
            const int luma = static_cast<int>(yRow[x]);
            const int u = static_cast<int>(uvRow[x & ~1]) - 128;
            const int v = static_cast<int>(uvRow[(x & ~1) + 1]) - 128;
            const int c = std::max(0, luma - 16);

            output[x * 3 + 0] = static_cast<uchar>(clampByte((298 * c + 459 * v + 128) >> 8));
            output[x * 3 + 1] = static_cast<uchar>(clampByte((298 * c - 55 * u - 136 * v + 128) >> 8));
            output[x * 3 + 2] = static_cast<uchar>(clampByte((298 * c + 541 * u + 128) >> 8));
        }
    }

    return image;
}

} // namespace

VideoFrameWidget::VideoFrameWidget(QWidget* parent) : QWidget(parent)
{
    setObjectName("VideoFrameWidget");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    framePresenter_ = std::make_shared<D3DFramePresenter>();
    d3dSurface_ = new D3DVideoSurface(this);
    d3dSurface_->setGeometry(rect());
    // The D3D surface is a native child window, so it receives mouse/keyboard
    // events instead of this widget while video is rendering. Filter its events
    // so remote-control capture works over live video, not just the QImage path.
    d3dSurface_->installEventFilter(this);
    updateD3DTarget();
}

VideoFrameWidget::~VideoFrameWidget() = default;

void VideoFrameWidget::setStatusText(const QString& text)
{
    if (statusText_ == text) {
        return;
    }
    statusText_ = text;
    update();
}

bool VideoFrameWidget::setVideoFrame(screenshare::SessionEvent::VideoFrame frame)
{
    if (frame.width > 0 && frame.height > 0) {
        frameWidth_.store(frame.width, std::memory_order_relaxed);
        frameHeight_.store(frame.height, std::memory_order_relaxed);
    }
    if (d3dSurface_ == nullptr) {
        QImage image = nv12ToRgb(frame);
        if (image.isNull()) {
            return false;
        }
        image_ = std::move(image);
        update();
        return true;
    }

    showVideoSurface();
    return presentVideoFrameAsync(std::move(frame));
}

bool VideoFrameWidget::presentVideoFrameAsync(screenshare::SessionEvent::VideoFrame frame)
{
    if (frame.width > 0 && frame.height > 0) {
        frameWidth_.store(frame.width, std::memory_order_relaxed);
        frameHeight_.store(frame.height, std::memory_order_relaxed);
    }
    const auto presenter = framePresenter_;
    if (!presenter) {
        return false;
    }

    const auto hwndValue = d3dHwnd_.load(std::memory_order_acquire);
    if (hwndValue == 0) {
        return false;
    }

    presenter->enqueue(
        reinterpret_cast<HWND>(hwndValue),
        d3dWidth_.load(std::memory_order_relaxed),
        d3dHeight_.load(std::memory_order_relaxed),
        smoothScalingAtomic_.load(std::memory_order_relaxed),
        std::move(frame));
    return true;
}

void VideoFrameWidget::showVideoSurface()
{
    if (d3dSurface_ == nullptr) {
        return;
    }
    image_ = {};
    if (d3dSurface_->geometry() != rect()) {
        d3dSurface_->setGeometry(rect());
    }
    if (!d3dSurface_->isVisible()) {
        d3dSurface_->show();
    }
    d3dSurface_->raise();
    updateD3DTarget();
}

void VideoFrameWidget::setSmoothScaling(bool enabled)
{
    if (smoothScaling_ == enabled) {
        return;
    }
    smoothScaling_ = enabled;
    smoothScalingAtomic_.store(enabled, std::memory_order_release);
    if (framePresenter_) {
        framePresenter_->setSmoothScaling(enabled);
    }
    update();
}

std::uint64_t VideoFrameWidget::presentedFrameCount() const
{
    return framePresenter_ ? framePresenter_->presentedFrameCount() : 0;
}

VideoFrameWidget::PresentationStats VideoFrameWidget::presentationStats() const
{
    return framePresenter_ ? framePresenter_->stats() : PresentationStats{};
}

void VideoFrameWidget::clearFrame()
{
    if (d3dSurface_ != nullptr) {
        d3dSurface_->hide();
    }
    if (framePresenter_) {
        framePresenter_->clear();
    }
    image_ = {};
    update();
}

void VideoFrameWidget::setRemoteInputHandler(std::function<void(const screenshare::RemoteInputEvent&)> handler)
{
    inputHandler_ = std::move(handler);
}

void VideoFrameWidget::setControlCapture(bool enabled, bool mouse, bool keyboard)
{
    controlActive_ = enabled;
    controlMouse_ = enabled && mouse;
    controlKeyboard_ = enabled && keyboard;
    setMouseTracking(controlMouse_);
    if (controlActive_) {
        setFocusPolicy(Qt::StrongFocus);
        setCursor(controlMouse_ ? Qt::CrossCursor : Qt::ArrowCursor);
        setFocus(Qt::OtherFocusReason);
    } else {
        setFocusPolicy(Qt::NoFocus);
        unsetCursor();
    }
    // Mirror the capture state onto the native render surface, which is the
    // window that actually receives the events while video is on screen.
    if (d3dSurface_ != nullptr) {
        d3dSurface_->setMouseTracking(controlMouse_);
        if (controlActive_) {
            d3dSurface_->setFocusPolicy(Qt::StrongFocus);
            d3dSurface_->setCursor(controlMouse_ ? Qt::CrossCursor : Qt::ArrowCursor);
            d3dSurface_->setFocus(Qt::OtherFocusReason);
        } else {
            d3dSurface_->setFocusPolicy(Qt::NoFocus);
            d3dSurface_->unsetCursor();
        }
    }
}

bool VideoFrameWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != d3dSurface_ || !controlActive_) {
        return QWidget::eventFilter(watched, event);
    }
    switch (event->type()) {
    case QEvent::MouseMove:
        if (controlMouse_ && inputHandler_) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            float normX = 0.0f;
            float normY = 0.0f;
            if (mapToNormalized(mouseEvent->position().toPoint(), normX, normY)) {
                screenshare::RemoteInputEvent input;
                input.kind = screenshare::RemoteInputKind::MouseMove;
                input.normX = normX;
                input.normY = normY;
                inputHandler_(input);
            }
            return true;
        }
        break;
    case QEvent::MouseButtonPress:
        if (controlMouse_) {
            d3dSurface_->setFocus(Qt::MouseFocusReason);
            emitMouseButton(static_cast<QMouseEvent*>(event), true);
            return true;
        }
        break;
    case QEvent::MouseButtonRelease:
        if (controlMouse_) {
            emitMouseButton(static_cast<QMouseEvent*>(event), false);
            return true;
        }
        break;
    case QEvent::Wheel:
        if (controlMouse_ && inputHandler_) {
            auto* wheelEvent = static_cast<QWheelEvent*>(event);
            const QPoint delta = wheelEvent->angleDelta();
            if (delta.x() != 0 || delta.y() != 0) {
                screenshare::RemoteInputEvent input;
                input.kind = screenshare::RemoteInputKind::MouseScroll;
                input.scrollX = delta.x();
                input.scrollY = delta.y();
                inputHandler_(input);
            }
            return true;
        }
        break;
    case QEvent::KeyPress:
        if (controlKeyboard_) {
            emitKey(static_cast<QKeyEvent*>(event), true);
            return true;
        }
        break;
    case QEvent::KeyRelease:
        if (controlKeyboard_) {
            emitKey(static_cast<QKeyEvent*>(event), false);
            return true;
        }
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

bool VideoFrameWidget::mapToNormalized(const QPoint& pos, float& normX, float& normY) const
{
    const int frameW = frameWidth_.load(std::memory_order_relaxed);
    const int frameH = frameHeight_.load(std::memory_order_relaxed);
    if (frameW <= 0 || frameH <= 0 || width() <= 0 || height() <= 0) {
        return false;
    }
    const QSize scaled = QSize(frameW, frameH).scaled(size(), Qt::KeepAspectRatio);
    if (scaled.width() <= 0 || scaled.height() <= 0) {
        return false;
    }
    const int left = (width() - scaled.width()) / 2;
    const int top = (height() - scaled.height()) / 2;
    const double relX = static_cast<double>(pos.x() - left) / static_cast<double>(scaled.width());
    const double relY = static_cast<double>(pos.y() - top) / static_cast<double>(scaled.height());
    if (relX < 0.0 || relX > 1.0 || relY < 0.0 || relY > 1.0) {
        return false; // outside the letterboxed image
    }
    normX = static_cast<float>(relX);
    normY = static_cast<float>(relY);
    return true;
}

namespace {
int RemoteMouseButtonId(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton:
        return 0;
    case Qt::RightButton:
        return 1;
    case Qt::MiddleButton:
        return 2;
    case Qt::XButton1:
        return 3;
    case Qt::XButton2:
        return 4;
    default:
        return -1;
    }
}
} // namespace

void VideoFrameWidget::emitMouseButton(QMouseEvent* event, bool pressed)
{
    if (!controlMouse_ || !inputHandler_) {
        return;
    }
    const int button = RemoteMouseButtonId(event->button());
    if (button < 0) {
        return;
    }
    float normX = 0.0f;
    float normY = 0.0f;
    if (!mapToNormalized(event->position().toPoint(), normX, normY)) {
        return;
    }
    screenshare::RemoteInputEvent input;
    input.kind = screenshare::RemoteInputKind::MouseButton;
    input.button = button;
    input.pressed = pressed;
    input.normX = normX;
    input.normY = normY;
    inputHandler_(input);
}

void VideoFrameWidget::emitKey(QKeyEvent* event, bool pressed)
{
    if (!controlKeyboard_ || !inputHandler_) {
        return;
    }
    screenshare::RemoteInputEvent input;
    input.kind = screenshare::RemoteInputKind::Key;
    input.key = static_cast<int>(event->nativeVirtualKey());
    input.scancode = static_cast<int>(event->nativeScanCode());
    input.pressed = pressed;
    inputHandler_(input);
}

void VideoFrameWidget::mousePressEvent(QMouseEvent* event)
{
    if (controlMouse_) {
        setFocus(Qt::MouseFocusReason);
        emitMouseButton(event, true);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void VideoFrameWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (controlMouse_) {
        emitMouseButton(event, false);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void VideoFrameWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (controlMouse_ && inputHandler_) {
        float normX = 0.0f;
        float normY = 0.0f;
        if (mapToNormalized(event->position().toPoint(), normX, normY)) {
            screenshare::RemoteInputEvent input;
            input.kind = screenshare::RemoteInputKind::MouseMove;
            input.normX = normX;
            input.normY = normY;
            inputHandler_(input);
        }
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void VideoFrameWidget::wheelEvent(QWheelEvent* event)
{
    if (controlMouse_ && inputHandler_) {
        const QPoint delta = event->angleDelta();
        if (delta.x() != 0 || delta.y() != 0) {
            screenshare::RemoteInputEvent input;
            input.kind = screenshare::RemoteInputKind::MouseScroll;
            input.scrollX = delta.x();
            input.scrollY = delta.y();
            inputHandler_(input);
        }
        event->accept();
        return;
    }
    QWidget::wheelEvent(event);
}

void VideoFrameWidget::keyPressEvent(QKeyEvent* event)
{
    if (controlKeyboard_) {
        emitKey(event, true);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void VideoFrameWidget::keyReleaseEvent(QKeyEvent* event)
{
    if (controlKeyboard_) {
        emitKey(event, false);
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void VideoFrameWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.fillRect(rect(), QColor("#111514"));

    if (!image_.isNull()) {
        const QSize scaled = image_.size().scaled(size(), Qt::KeepAspectRatio);
        const QRect target(
            (width() - scaled.width()) / 2,
            (height() - scaled.height()) / 2,
            scaled.width(),
            scaled.height());
        painter.setRenderHint(QPainter::SmoothPixmapTransform, smoothScaling_);
        painter.drawImage(target, image_);
        return;
    }

    if (!statusText_.isEmpty()) {
        painter.setPen(QColor("#a9b5b1"));
        QFont font = painter.font();
        font.setPointSize(12);
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.drawText(rect(), Qt::AlignCenter, statusText_);
    }
}

void VideoFrameWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (d3dSurface_ != nullptr) {
        d3dSurface_->setGeometry(rect());
    }
    updateD3DTarget();
}

void VideoFrameWidget::updateD3DTarget()
{
    if (d3dSurface_ == nullptr || !framePresenter_) {
        d3dHwnd_.store(0, std::memory_order_release);
        return;
    }

    const HWND hwnd = reinterpret_cast<HWND>(d3dSurface_->winId());
    const auto width = static_cast<std::uint32_t>(std::max(1, d3dSurface_->width()));
    const auto height = static_cast<std::uint32_t>(std::max(1, d3dSurface_->height()));
    d3dHwnd_.store(reinterpret_cast<std::uintptr_t>(hwnd), std::memory_order_release);
    d3dWidth_.store(width, std::memory_order_release);
    d3dHeight_.store(height, std::memory_order_release);
    framePresenter_->resize(hwnd, width, height);
}
