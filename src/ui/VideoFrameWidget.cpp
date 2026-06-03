#include "ui/VideoFrameWidget.h"

#include "render/Nv12D3D11Presenter.h"

#include <QtGui/QPainter>
#include <QtGui/QPaintEngine>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QSizePolicy>

#include <algorithm>
#include <cstdint>
#include <exception>

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

    void presentFrame(const screenshare::SessionEvent::VideoFrame& frame)
    {
        presenter_.Attach(reinterpret_cast<HWND>(winId()));
        presenter_.Resize(
            static_cast<std::uint32_t>(std::max(1, width())),
            static_cast<std::uint32_t>(std::max(1, height())));
        presenter_.Present(screenshare::Nv12D3D11Presenter::FrameView{
            frame.width,
            frame.height,
            frame.nv12.data(),
            frame.nv12.size(),
        });
    }

    void clearFrame()
    {
        presenter_.Clear();
    }

    void setSmoothScaling(bool enabled)
    {
        presenter_.SetLinearSampling(enabled);
    }

    QPaintEngine* paintEngine() const override
    {
        return nullptr;
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        presenter_.Resize(
            static_cast<std::uint32_t>(std::max(1, event->size().width())),
            static_cast<std::uint32_t>(std::max(1, event->size().height())));
    }

    void paintEvent(QPaintEvent*) override
    {
    }

private:
    screenshare::Nv12D3D11Presenter presenter_;
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
    d3dSurface_ = new D3DVideoSurface(this);
    d3dSurface_->setGeometry(rect());
    d3dSurface_->setSmoothScaling(smoothScaling_);
}

void VideoFrameWidget::setStatusText(const QString& text)
{
    if (statusText_ == text) {
        return;
    }
    statusText_ = text;
    update();
}

void VideoFrameWidget::setVideoFrame(const screenshare::SessionEvent::VideoFrame& frame)
{
    if (!d3dUnavailable_ && d3dSurface_ != nullptr) {
        try {
            image_ = {};
            if (d3dSurface_->geometry() != rect()) {
                d3dSurface_->setGeometry(rect());
            }
            if (!d3dSurface_->isVisible()) {
                d3dSurface_->show();
                d3dSurface_->raise();
            }
            d3dSurface_->presentFrame(frame);
            return;
        } catch (const std::exception&) {
            d3dUnavailable_ = true;
            d3dSurface_->hide();
        }
    }

    QImage image = nv12ToRgb(frame);
    if (image.isNull()) {
        return;
    }
    image_ = std::move(image);
    update();
}

void VideoFrameWidget::setSmoothScaling(bool enabled)
{
    if (smoothScaling_ == enabled) {
        return;
    }
    smoothScaling_ = enabled;
    if (d3dSurface_ != nullptr) {
        d3dSurface_->setSmoothScaling(enabled);
    }
    update();
}

void VideoFrameWidget::clearFrame()
{
    if (d3dSurface_ != nullptr) {
        try {
            d3dSurface_->clearFrame();
        } catch (const std::exception&) {
        }
        d3dSurface_->hide();
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
}
