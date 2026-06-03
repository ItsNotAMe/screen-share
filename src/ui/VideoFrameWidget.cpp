#include "ui/VideoFrameWidget.h"

#include "render/Nv12D3D11Presenter.h"

#include <QtGui/QPainter>
#include <QtGui/QPaintEngine>
#include <QtGui/QResizeEvent>
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
