#include "ui/VideoFrameWidget.h"

#include "render/Nv12D3D11Presenter.h"

#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QRegion>
#include <QtGui/QPaintEngine>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QResizeEvent>
#include <QtGui/QWheelEvent>
#include <QtGui/QCursor>
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
    explicit D3DFramePresenter(FramePresentationFactory factory)
        : worker_([this, factory = std::move(factory)] {
              run(factory);
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
    void setLowLatency(bool enabled) {
        { std::scoped_lock lock(mutex_); lowLatency_ = enabled; resizePending_ = true; }
        condition_.notify_one();
    }

    void clear()
    {
        {
            std::scoped_lock lock(mutex_);
            if (pendingFrame_) ++droppedFrames_;
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
        { std::lock_guard lock(diagnosticsMutex_); snapshot.renderer = rendererStatistics_; snapshot.inputMapping = inputMapping_;
          snapshot.inputViewportWidth=inputViewportWidth_;snapshot.inputViewportHeight=inputViewportHeight_; }
        snapshot.enqueuedFrames = enqueuedFrames_.load(std::memory_order_relaxed);
        snapshot.presentedFrames = presentedFrames_.load(std::memory_order_relaxed);
        snapshot.droppedFrames = droppedFrames_.load(std::memory_order_relaxed);
        snapshot.queuedFrames = queuedFrames_.load(std::memory_order_acquire);
        snapshot.presentErrors = presentErrors_.load(std::memory_order_relaxed);
        snapshot.maximumFrameLatency = maximumFrameLatency_.load(std::memory_order_relaxed);
        snapshot.recoveries = recoveries_.load(std::memory_order_relaxed);
        snapshot.terminal = terminal_.load(std::memory_order_acquire);
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
        bool lowLatency = false;
        std::optional<screenshare::SessionEvent::VideoFrame> frame;
    };

    static void recordMax(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept
    {
        std::uint64_t current = target.load(std::memory_order_relaxed);
        while (current < value &&
               !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
        }
    }

    void run(const FramePresentationFactory& factory)
    {
        FramePresentationSession presenter(factory);
        bool active = false;
        screenshare::input::FrameMapping bufferedMapping;
        for (;;) {
            Work work;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] {
                    return stopping_ || pendingFrame_ || resizePending_ || smoothPending_ || clearPending_;
                });
                if (stopping_) break;
                work.hwnd = hwnd_;
                work.width = width_;
                work.height = height_;
                work.smoothScaling = smoothScaling_;
                work.clearPending = clearPending_;
                work.lowLatency = lowLatency_;
                work.frame = std::move(pendingFrame_);
                pendingFrame_.reset();
                queuedFrames_.store(0, std::memory_order_release);
                resizePending_ = smoothPending_ = clearPending_ = false;
            }
            if (work.clearPending) { presenter.Clear(); active = false; bufferedMapping={}; }
            const auto started = std::chrono::steady_clock::now();
            bool presented = false;
            bool redrawn = false;
            if (work.frame) {
                active = true;
                const auto pixels = work.frame->native ? std::span<const uint8_t>{} : work.frame->pixels();
                presented = presenter.Present(work.hwnd, work.width, work.height, work.smoothScaling,
                    work.lowLatency, {work.frame->width, work.frame->height, pixels.data(), pixels.size(),
                        work.frame->native ? work.frame->native->texture() : nullptr});
            } else if (active) {
                redrawn = presenter.Update(work.hwnd, work.width, work.height, work.smoothScaling, work.lowLatency);
            }
            const auto status = presenter.statistics();
            const bool busy = status.outcome==screenshare::PresentationOutcome::Busy;
            if(work.frame)bufferedMapping=(presented || busy)?work.frame->inputMapping:screenshare::input::FrameMapping{};
            { std::lock_guard lock(diagnosticsMutex_); rendererStatistics_ = status;
              // A busy swap chain leaves the last presented image visible.
              // Dropping its mapping here also silently drops keyboard events
              // between that skipped frame and the next successful present.
              if(work.clearPending || (work.frame && !presented && !busy))inputMapping_={};
              if(!work.frame && redrawn && status.outcome==screenshare::PresentationOutcome::Presented && bufferedMapping.Valid()) {
                  inputMapping_=bufferedMapping;
                  inputViewportWidth_=work.width;inputViewportHeight_=work.height;
              }
              if(presented) {inputMapping_=work.frame->inputMapping;inputViewportWidth_=work.width;inputViewportHeight_=work.height;} }
            presentErrors_ = status.errors;
            recoveries_ = status.recoveries;
            maximumFrameLatency_ = status.maximumFrameLatency;
            terminal_.store(status.terminal, std::memory_order_release);
            if (!work.frame) continue;
            if (!presented) { ++droppedFrames_; continue; }
            const auto micros = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count());
            totalPresentMicros_.fetch_add(micros, std::memory_order_relaxed);
            lastPresentMicros_.store(micros, std::memory_order_relaxed);
            recordMax(maxPresentMicros_, micros);
            presentedFrames_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    mutable std::mutex mutex_;
    mutable std::mutex diagnosticsMutex_;
    FramePresentationSession::Statistics rendererStatistics_;
    screenshare::input::FrameMapping inputMapping_;
    uint32_t inputViewportWidth_=0,inputViewportHeight_=0;
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
    bool lowLatency_ = false;
    std::atomic<std::uint64_t> presentErrors_{0};
    std::atomic<std::uint64_t> recoveries_{0};
    std::atomic_bool terminal_{false};
    std::atomic<std::uint32_t> maximumFrameLatency_{0};
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
    std::function<void(int,int)> rawMotion;
    std::function<void()> positionChanged;
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
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override
    {
        const auto* msg=static_cast<MSG*>(message);
        if(msg->message==WM_WINDOWPOSCHANGED && positionChanged)positionChanged();
        if(msg->message==WM_INPUT && rawMotion) {
            RAWINPUT raw{};UINT bytes=sizeof(raw);
            if(GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam),RID_INPUT,&raw,&bytes,sizeof(RAWINPUTHEADER))!=UINT(-1) &&
                raw.header.dwType==RIM_TYPEMOUSE && !(raw.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE))
                rawMotion(raw.data.mouse.lLastX,raw.data.mouse.lLastY);
        }
        // Qt/DefWindowProc must still clean up foreground WM_INPUT.
        return QWidget::nativeEvent(type,message,result);
    }
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
    if (static_cast<qsizetype>(frame.pixels().size()) < requiredBytes) {
        return {};
    }

    QImage image(frame.width, frame.height, QImage::Format_RGB888);
    if (image.isNull()) {
        return {};
    }

    const auto* yPlane = frame.pixels().data();
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

VideoFrameWidget::VideoFrameWidget(QWidget* parent, FramePresentationFactory factory) : QWidget(parent)
{
    setObjectName("VideoFrameWidget");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    framePresenter_ = std::make_shared<D3DFramePresenter>(std::move(factory));
    d3dSurface_ = new D3DVideoSurface(this);
    d3dSurface_->setGeometry(rect());
    // The D3D surface is a native child window, so it receives mouse/keyboard
    // events instead of this widget while video is rendering. Filter its events
    // so remote-control capture works over live video, not just the QImage path.
    d3dSurface_->installEventFilter(this);
    d3dSurface_->rawMotion=[this](int x,int y){emitRelativeMotion(x,y);};
    d3dSurface_->positionChanged=[this]{updateGameMouseClip();};
    updateD3DTarget();
}

VideoFrameWidget::~VideoFrameWidget() { releaseGameMouse();d3dSurface_->rawMotion={};d3dSurface_->positionChanged={}; }
void VideoFrameWidget::setLowLatency(bool enabled) { framePresenter_->setLowLatency(enabled); }

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
        pendingImageMapping_ = frame.inputMapping;
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

screenshare::input::FrameMapping VideoFrameWidget::presentedInputMapping() const
{
    if(!d3dSurface_)return paintedMapping_;
    const auto stats=framePresenter_->stats();
    if(!d3dSurface_->isVisible() || stats.inputViewportWidth!=d3dWidth_.load() || stats.inputViewportHeight!=d3dHeight_.load())return {};
    return stats.inputMapping;
}

void VideoFrameWidget::clearFrame()
{
    releaseGameMouse();
    pendingImageMapping_ = paintedMapping_ = mappedForInput_ = {};
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
    if(controlActive_==enabled && controlMouse_==(enabled&&mouse) && controlKeyboard_==(enabled&&keyboard))return;
    controlActive_ = enabled;
    controlMouse_ = enabled && mouse;
    controlKeyboard_ = enabled && keyboard;
    if(!controlMouse_)releaseGameMouse();
    setMouseTracking(controlMouse_);
    if (controlActive_) {
        setFocusPolicy(Qt::StrongFocus);
        setCursor(gameMouseCaptured_ ? Qt::BlankCursor : controlMouse_ ? Qt::CrossCursor : Qt::ArrowCursor);
        // Do not steal focus from the button that activated this window.
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
            d3dSurface_->setCursor(gameMouseCaptured_ ? Qt::BlankCursor : controlMouse_ ? Qt::CrossCursor : Qt::ArrowCursor);
            // Pointer interaction gives the video focus when requested.
        } else {
            d3dSurface_->setFocusPolicy(Qt::NoFocus);
            d3dSurface_->unsetCursor();
        }
    }
}

void VideoFrameWidget::setGameMouseMode(bool enabled)
{
    if(gameMouseMode_==enabled)return;
    releaseGameMouse();gameMouseMode_=enabled;
}

bool VideoFrameWidget::captureGameMouse()
{
    if(!gameMouseMode_ || !controlMouse_ || !isVisible())return false;
    if(!presentedInputMapping().Valid()) {
        if(inputCaptureFailed)inputCaptureFailed("Waiting for the shared video. Click the video again to capture the game mouse.");
        return false;
    }
    UINT count=0;
    previousRawMouse_.reset();
    if(GetRegisteredRawInputDevices(nullptr,&count,sizeof(RAWINPUTDEVICE))==0 && count<=128) {
        std::vector<RAWINPUTDEVICE> devices(count);
        if(count && GetRegisteredRawInputDevices(devices.data(),&count,sizeof(RAWINPUTDEVICE))!=UINT(-1))
            for(const auto& existing:devices)if(existing.usUsagePage==1 && existing.usUsage==2)previousRawMouse_=existing;
    }
    const RAWINPUTDEVICE device{1,2,0,reinterpret_cast<HWND>(d3dSurface_->winId())};
    if(!RegisterRawInputDevices(&device,1,sizeof(device))) {
        if(inputCaptureFailed)inputCaptureFailed("Could not capture the game mouse. Click the video to retry.");
        return false;
    }
    gameMouseCaptured_=true;
    setCursor(Qt::BlankCursor);d3dSurface_->setCursor(Qt::BlankCursor);
    updateGameMouseClip();
    if(!gameMouseCaptured_ && inputCaptureFailed)inputCaptureFailed("Could not lock the game mouse. Click the video to retry.");
    if(gameMouseCaptured_ && gameMouseCaptureStarted)gameMouseCaptureStarted();
    return gameMouseCaptured_;
}

void VideoFrameWidget::updateGameMouseClip()
{
    if(!gameMouseCaptured_)return;
    const auto handle=reinterpret_cast<HWND>(d3dSurface_->winId());
    RECT client{};
    if(!GetClientRect(handle,&client) || client.right<=client.left || client.bottom<=client.top) {releaseGameMouse();return;}
    // Raw motion does not depend on cursor travel. Keep the pointer at the
    // native video's centre, away from window borders, controls and rounded
    // corners. Native coordinates also avoid mixed-DPI rounding errors.
    POINT native{(client.left+client.right)/2,(client.top+client.bottom)/2};
    if(!ClientToScreen(handle,&native)) {releaseGameMouse();return;}
    RECT bounds{native.x,native.y,native.x+1,native.y+1};
    if(!ClipCursor(&bounds))releaseGameMouse();
}

void VideoFrameWidget::releaseGameMouse()
{
    if(!gameMouseCaptured_)return;
    gameMouseCaptured_=false;captureClickButton_=Qt::NoButton;
    ClipCursor(nullptr);
    const RAWINPUTDEVICE device=previousRawMouse_.value_or(RAWINPUTDEVICE{1,2,RIDEV_REMOVE,nullptr});
    RegisterRawInputDevices(&device,1,sizeof(device));
    previousRawMouse_.reset();
    setCursor(controlMouse_?Qt::CrossCursor:Qt::ArrowCursor);
    d3dSurface_->setCursor(controlMouse_?Qt::CrossCursor:Qt::ArrowCursor);
}

void VideoFrameWidget::emitRelativeMotion(int x,int y)
{
    if(!gameMouseCaptured_ || !controlMouse_ || !inputHandler_ || (!x && !y))return;
    screenshare::RemoteInputEvent input;input.kind=screenshare::RemoteInputKind::MouseMove;
    input.relativeMouse=true;input.normX=float(std::clamp(x,-32767,32767));input.normY=float(std::clamp(y,-32767,32767));
    input.sourceMapping=presentedInputMapping();inputHandler_(input);
}

bool VideoFrameWidget::event(QEvent* event)
{
    if(event->type()==QEvent::Hide)releaseGameMouse();
    if(controlKeyboard_ && event->type()==QEvent::ShortcutOverride) {event->accept();return true;}
    // QWidget normally interprets Tab as local focus traversal.
    if(controlKeyboard_ && event->type()==QEvent::KeyPress) {emitKey(static_cast<QKeyEvent*>(event),true);return true;}
    if(controlKeyboard_ && event->type()==QEvent::KeyRelease) {emitKey(static_cast<QKeyEvent*>(event),false);return true;}
    return QWidget::event(event);
}

bool VideoFrameWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != d3dSurface_ || !controlActive_) {
        return QWidget::eventFilter(watched, event);
    }
    switch (event->type()) {
    case QEvent::Hide: case QEvent::FocusOut: releaseGameMouse();break;
    case QEvent::ShortcutOverride:
        if(controlKeyboard_) {event->accept();return true;}
        break;
    case QEvent::MouseMove:
        if(gameMouseMode_ && controlMouse_)return true;
        if (controlMouse_ && inputHandler_) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            float normX = 0.0f;
            float normY = 0.0f;
            if (mapToNormalized(mouseEvent->position().toPoint(), normX, normY)) {
                screenshare::RemoteInputEvent input;
                input.kind = screenshare::RemoteInputKind::MouseMove;
                input.normX = normX;
                input.normY = normY;
                input.sourceMapping = mappedForInput_; inputHandler_(input);
            }
            return true;
        }
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
        // Qt delivers a double-click as Press, Release, DblClick, Release — the
        // DblClick stands in for the second press, so forward it as a button-down
        // or the host only ever sees a single click.
        if (controlActive_) {
            d3dSurface_->setFocus(Qt::MouseFocusReason);
            if(gameMouseMode_ && controlMouse_ && !gameMouseCaptured_) {
                if(captureGameMouse())captureClickButton_=static_cast<QMouseEvent*>(event)->button();
                return true;
            }
            if (controlMouse_) emitMouseButton(static_cast<QMouseEvent*>(event), true);
            return true;
        }
        break;
    case QEvent::MouseButtonRelease:
        if(captureClickButton_==static_cast<QMouseEvent*>(event)->button()) {captureClickButton_=Qt::NoButton;return true;}
        if (controlMouse_) {
            emitMouseButton(static_cast<QMouseEvent*>(event), false);
            return true;
        }
        break;
    case QEvent::Wheel:
        if (controlMouse_ && inputHandler_) {
            emitWheel(static_cast<QWheelEvent*>(event));
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
    mappedForInput_ = presentedInputMapping();
    const int frameW = mappedForInput_.Valid()?mappedForInput_.width:frameWidth_.load(std::memory_order_relaxed);
    const int frameH = mappedForInput_.Valid()?mappedForInput_.height:frameHeight_.load(std::memory_order_relaxed);
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
    if(gameMouseMode_) {
        if(!gameMouseCaptured_)return;
        screenshare::RemoteInputEvent input;input.kind=screenshare::RemoteInputKind::MouseButton;
        input.relativeMouse=true;input.button=button;input.pressed=pressed;
        input.sourceMapping=presentedInputMapping();inputHandler_(input);return;
    }
    float normX = 0.0f;
    float normY = 0.0f;
    const bool mapped = mapToNormalized(event->position().toPoint(), normX, normY);
    if (!mapped && pressed) return;
    screenshare::RemoteInputEvent input;
    input.kind = screenshare::RemoteInputKind::MouseButton;
    input.button = button;
    input.pressed = pressed;
    input.normX = normX;
    input.normY = normY;
    // An outside release still releases the held button. The receiver uses its
    // original mapping rather than withdrawing the host's permission.
    if (mapped) input.sourceMapping = mappedForInput_;
    inputHandler_(input);
}

void VideoFrameWidget::emitKey(QKeyEvent* event, bool pressed)
{
    if (!controlKeyboard_ || !inputHandler_) {
        return;
    }
    // Qt synthesizes auto-repeat as alternating repeat press/release events.
    // Forward repeat presses so the host repeats at the viewer's configured
    // rate, but suppress repeat releases so the host retains one tracked key
    // until the real physical release arrives.
    if (!pressed && event->isAutoRepeat()) {
        return;
    }
    screenshare::RemoteInputEvent input;
    input.kind = screenshare::RemoteInputKind::Key;
    input.key = static_cast<int>(event->nativeVirtualKey());
    input.scancode = static_cast<int>(event->nativeScanCode());
    // Qt's Windows mapper encodes extended scans as E0xx. The input protocol
    // uses the low scan byte plus bit 8, matching the Win32 preview path.
    // Convert before validation, otherwise navigation keys are dropped locally.
    if ((input.scancode & 0xff00) == 0xe000)
        input.scancode = (input.scancode & 0xff) | 0x100;
    input.pressed = pressed;
    // Keys need the displayed source identity, not a pixel-to-viewport mapping.
    // Resizing the viewport must not suppress typing while its redraw completes.
    input.sourceMapping = d3dSurface_ && d3dSurface_->isVisible()
        ? framePresenter_->stats().inputMapping : presentedInputMapping();
    inputHandler_(input);
}

void VideoFrameWidget::emitWheel(QWheelEvent* event)
{
    if (!controlMouse_ || !inputHandler_) {
        return;
    }
    QPoint delta = event->angleDelta();
    if (delta.isNull() && !event->pixelDelta().isNull()) {
        // Precision touchpads may provide pixelDelta only. Convert the common
        // Qt ratio of 15 pixels per wheel step into Win32 WHEEL_DELTA units
        // (120/15 = 8) while preserving sub-step/high-resolution movement.
        constexpr int kWheelUnitsPerPixel = 8;
        delta = event->pixelDelta() * kWheelUnitsPerPixel;
    }
    if (delta.isNull()) {
        return;
    }
    screenshare::RemoteInputEvent input;
    input.kind = screenshare::RemoteInputKind::MouseScroll;
    if(gameMouseMode_) {
        if(!gameMouseCaptured_)return;
        input.relativeMouse=true;input.sourceMapping=presentedInputMapping();
    } else {
        if(!mapToNormalized(event->position().toPoint(),input.normX,input.normY))return;
        input.sourceMapping=mappedForInput_;
    }
    input.scrollX = delta.x();
    input.scrollY = delta.y();
    inputHandler_(input);
}

void VideoFrameWidget::mousePressEvent(QMouseEvent* event)
{
    if (controlActive_) {
        setFocus(Qt::MouseFocusReason);
        if(gameMouseMode_ && controlMouse_ && !gameMouseCaptured_) {
            if(captureGameMouse())captureClickButton_=event->button();
            event->accept();return;
        }
        if (controlMouse_) emitMouseButton(event, true);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void VideoFrameWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if(captureClickButton_==event->button()) {captureClickButton_=Qt::NoButton;event->accept();return;}
    if (controlMouse_) {
        emitMouseButton(event, false);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void VideoFrameWidget::mouseMoveEvent(QMouseEvent* event)
{
    if(gameMouseMode_ && controlMouse_) {event->accept();return;}
    if (controlMouse_ && inputHandler_) {
        float normX = 0.0f;
        float normY = 0.0f;
        if (mapToNormalized(event->position().toPoint(), normX, normY)) {
            screenshare::RemoteInputEvent input;
            input.kind = screenshare::RemoteInputKind::MouseMove;
            input.normX = normX;
            input.normY = normY;
            input.sourceMapping = mappedForInput_; inputHandler_(input);
        }
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void VideoFrameWidget::wheelEvent(QWheelEvent* event)
{
    if (controlMouse_ && inputHandler_) {
        emitWheel(event);
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
        paintedMapping_ = pendingImageMapping_;
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

void VideoFrameWidget::setCornerRadius(int radius)
{
    cornerRadius_ = std::max(0, radius);
    updateCornerMask();
}

void VideoFrameWidget::setOverlayExclusion(const QRect& rect)
{
    overlayExclusion_ = rect;
    updateCornerMask();
}

void VideoFrameWidget::updateCornerMask()
{
    QRegion region(rect());
    if (cornerRadius_) {
        QPainterPath outline;
        outline.addRoundedRect(QRectF(rect()), cornerRadius_, cornerRadius_);
        region = QRegion(outline.toFillPolygon().toPolygon());
        setMask(region);
    } else clearMask();
    // Clip the native swap chain under in-app overlays without changing its
    // viewport, scaling, frame presentation or input coordinates.
    if (d3dSurface_) {
        if (!cornerRadius_ && overlayExclusion_.isEmpty())d3dSurface_->clearMask();
        else {
            region -= QRegion(overlayExclusion_);
            // An empty QWidget mask removes clipping instead of hiding it.
            d3dSurface_->setMask(region.isEmpty()?QRegion(-1,-1,1,1):region);
        }
    }
}

void VideoFrameWidget::resizeEvent(QResizeEvent* event)
{
    paintedMapping_={};
    QWidget::resizeEvent(event);
    if (d3dSurface_ != nullptr) {
        d3dSurface_->setGeometry(rect());
    }
    updateCornerMask();
    updateD3DTarget();
    updateGameMouseClip();
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
