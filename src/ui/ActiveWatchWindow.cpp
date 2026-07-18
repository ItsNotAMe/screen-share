#include "ui/ActiveWatchWindow.h"

#include "ui/AppShellWindow.h"
#include "ui/UiStyle.h"
#include "ui/VideoFrameWidget.h"
#include "input/XInputGamepad.h"

#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QIODevice>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtCore/QSizeF>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QIcon>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtSvg/QSvgRenderer>
#include <QtWidgets/QFrame>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QSlider>
#include <QtWidgets/QStyle>
#include <QtWidgets/QApplication>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

QPixmap renderSvgResource(const QString& path, const QSize& size, const QString& color = QString())
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QByteArray svg = file.readAll();
    if (!color.isEmpty()) {
        svg.replace("currentColor", color.toUtf8());
    }

    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) {
        return {};
    }

    QPixmap pixmap(size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(size)));
    return pixmap;
}

class VolumeSlider : public QSlider {
public:
    explicit VolumeSlider(QWidget* parent = nullptr)
        : QSlider(Qt::Horizontal, parent)
    {
        setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);

        constexpr qreal handleRadius = 8.0;
        constexpr qreal trackHeight = 4.0;
        const qreal centerY = static_cast<qreal>(height()) / 2.0;
        const QRectF track(
            handleRadius,
            centerY - trackHeight / 2.0,
            std::max<qreal>(1.0, static_cast<qreal>(width()) - handleRadius * 2.0),
            trackHeight);
        const qreal ratio =
            maximum() <= minimum() ? 0.0 :
            static_cast<qreal>(value() - minimum()) / static_cast<qreal>(maximum() - minimum());
        const qreal handleX = track.left() + std::clamp<qreal>(ratio, 0.0, 1.0) * track.width();

        painter.setBrush(QColor("#53615d"));
        painter.drawRoundedRect(track, trackHeight / 2.0, trackHeight / 2.0);

        if (handleX > track.left()) {
            const QRectF filled(track.left(), track.top(), handleX - track.left(), track.height());
            painter.setBrush(QColor("#44d4c8"));
            painter.drawRoundedRect(filled, trackHeight / 2.0, trackHeight / 2.0);
        }

        painter.setBrush(QColor("#62e8dc"));
        painter.drawEllipse(QPointF(handleX, centerY), handleRadius, handleRadius);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        setValueFromPosition(event->position().x());
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if ((event->buttons() & Qt::LeftButton) == 0) {
            return;
        }
        setValueFromPosition(event->position().x());
        event->accept();
    }

private:
    void setValueFromPosition(qreal x)
    {
        constexpr qreal handleRadius = 8.0;
        const qreal left = handleRadius;
        const qreal right = std::max(left + 1.0, static_cast<qreal>(width()) - handleRadius);
        const qreal ratio = std::clamp((x - left) / (right - left), 0.0, 1.0);
        const int nextValue = minimum() + static_cast<int>(std::round(ratio * (maximum() - minimum())));
        setValue(nextValue);
    }
};

QString formatResolution(const std::optional<screenshare::SessionResolution>& resolution)
{
    if (!resolution || resolution->width <= 0 || resolution->height <= 0) {
        return QStringLiteral("-");
    }
    return QStringLiteral("%1 x %2").arg(resolution->width).arg(resolution->height);
}

QString fpsText(double fps)
{
    if (fps <= 0.0) {
        return QStringLiteral("-");
    }
    return QString::number(static_cast<int>(std::round(fps)));
}

QString bitrateText(double bitrateMbps)
{
    if (bitrateMbps <= 0.0) {
        return QStringLiteral("-");
    }
    return QStringLiteral("%1 Mbps").arg(bitrateMbps, 0, 'f', 1);
}

QString avSyncText(const screenshare::SessionAudioStatus& audio)
{
    if (!audio.hasStats) {
        return QStringLiteral("-");
    }
    if (audio.playbackMuted) {
        return QStringLiteral("Muted");
    }
    return QStringLiteral("%1 ms").arg(audio.avAudioAheadMs);
}

QString controlTypesString(uint32_t capabilities)
{
    QString types;
    if ((capabilities & screenshare::ControlCapabilityMouse) != 0) {
        types += QStringLiteral("Mouse");
    }
    if ((capabilities & screenshare::ControlCapabilityKeyboard) != 0) {
        if (!types.isEmpty()) {
            types += QStringLiteral(" + ");
        }
        types += QStringLiteral("Keyboard");
    }
    if ((capabilities & screenshare::ControlCapabilityGamepad) != 0) {
        if (!types.isEmpty()) {
            types += QStringLiteral(" + ");
        }
        types += QStringLiteral("Gamepad");
    }
    return types;
}

QString latencyText(const screenshare::SessionAudioStatus& audio)
{
    if (!audio.hasStats) {
        return QStringLiteral("-");
    }
    // Receive-side playout latency: fixed device latency plus the queued buffer.
    const qulonglong totalMs =
        static_cast<qulonglong>(std::max(0, audio.playbackLatencyMs)) + audio.playbackQueueMs;
    return QStringLiteral("%1 ms").arg(totalMs);
}

} // namespace

ActiveWatchWindow::ActiveWatchWindow(QtSessionBackend* backend, Actions actions, QWidget* parent)
    : QWidget(parent), backend_(backend), actions_(std::move(actions))
{
    setObjectName("ActiveWatchWindow");
    setStyleSheet(uiStyleSheet());

    elapsedTimer_ = new QTimer(this);
    elapsedTimer_->setInterval(1000);
    connect(elapsedTimer_, &QTimer::timeout, this, [this] {
        updateElapsed();
    });

    gamepadTimer_ = new QTimer(this);
    gamepadTimer_->setTimerType(Qt::PreciseTimer);
    gamepadTimer_->setInterval(100);
    connect(gamepadTimer_, &QTimer::timeout, this, [this] { pollGamepad(); });
    gamepadClock_.start();
    gamepadTimer_->start();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(buildShell(), 1);
}

void ActiveWatchWindow::setSession(const WatchSessionUiState& session)
{
    closeStreamFullscreen();
    session_ = session;
    installBackendHandlers();
    elapsed_.restart();
    elapsedTimer_->start();
    updateElapsed();
    roomLabel_->setText(QStringLiteral("Room: %1").arg(session_.roomName.isEmpty() ? session_.roomId : session_.roomName));
    hostLabel_->setText(session_.roomName.isEmpty() ? QStringLiteral("-") : session_.roomName);
    audioMuted_ = false;
    audioVolumePercent_ = 100;
    audioControlsInitialized_ = false;
    audioControlsTouched_ = false;
    receivedVideoFrame_ = false;
    hostLeft_ = false;
    leaveRequested_ = false;
    // Remote-control grants never carry across sessions: a rejoin must start with
    // no control until the host explicitly re-grants it. Reset the "You control"
    // UI and stop the video widget from capturing local input; otherwise a viewer
    // that left while controlling would keep swallowing its own mouse/keyboard.
    handleControlState(0);
    firstVideoFramePosted_.store(false, std::memory_order_release);
    lastPresentedFrameCount_ = 0;
    presentedFps_ = 0.0;
    latestStreamFps_ = 0.0;
    presentedFpsTimer_.restart();
    // Re-baseline the received-bitrate sampler: elapsed_ was just restarted, so
    // a stale sample time would otherwise make the delta negative and freeze the
    // stat on rejoin.
    lastBitrateBytes_ = 0;
    lastBitrateSampleMs_ = -1;
    receiveBitrateMbps_ = 0.0;
    videoFrameWidget_->clearFrame();
    setPreviewStatusText("Connecting...");
    startWatch();
}

bool ActiveWatchWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (streamFullscreen_) {
        if (event->type() == QEvent::KeyPress) {
            const auto* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->key() == Qt::Key_Escape || keyEvent->key() == Qt::Key_F11) {
                closeStreamFullscreen();
                return true;
            }
        }
        if (watched == window() && event->type() == QEvent::WindowStateChange && !window()->isFullScreen()) {
            setStreamFullscreen(false);
        }
    }
    return QWidget::eventFilter(watched, event);
}

QWidget* ActiveWatchWindow::buildShell()
{
    auto* host = new QWidget;
    host->setObjectName("ActiveWatchContent");
    auto* root = new QVBoxLayout(host);
    rootLayout_ = root;
    root->setContentsMargins(20, 14, 20, 20);
    root->setSpacing(14);
    topStatusWidget_ = buildTopStatus();
    root->addWidget(topStatusWidget_);

    auto* body = new QHBoxLayout;
    bodyLayout_ = body;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(14);
    previewPanel_ = buildPreviewPanel();
    sideStatsPanel_ = buildSideStats();
    body->addWidget(previewPanel_, 1);
    body->addWidget(sideStatsPanel_, 0);
    root->addLayout(body, 1);
    footerWidget_ = buildFooter();
    root->addWidget(footerWidget_);
    return host;
}

QWidget* ActiveWatchWindow::buildTopStatus()
{
    auto* bar = new QWidget;
    bar->setObjectName("ActiveTransparentBlock");
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 0, 146, 0);
    layout->setSpacing(18);
    layout->addWidget(textLabel("ScreenShare", "ActiveTopTitle"));
    stateLabel_ = textLabel("● Watching", "ActiveTopLive");
    layout->addWidget(stateLabel_);
    elapsedLabel_ = textLabel("00:00:00", "ActiveTopMuted");
    layout->addWidget(elapsedLabel_);
    layout->addStretch(1);
    roomLabel_ = textLabel("Room: -", "ActiveTopMuted");
    roomLabel_->setWordWrap(false);
    roomLabel_->setMinimumWidth(300);
    roomLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(roomLabel_);
    return bar;
}

QWidget* ActiveWatchWindow::buildPreviewPanel()
{
    auto* panel = new QFrame;
    panel->setObjectName("WatchPreviewPanel");
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    videoFrameWidget_ = new VideoFrameWidget;
    videoFrameWidget_->setStatusText("Waiting for stream");
    videoFrameWidget_->setFocusPolicy(Qt::StrongFocus);
    videoFrameWidget_->setRemoteInputHandler([this](const screenshare::RemoteInputEvent& event) {
        if (backend_ != nullptr) {
            backend_->sendRemoteInput(event);
        }
    });
    layout->addWidget(videoFrameWidget_, 1);
    return panel;
}

QWidget* ActiveWatchWindow::buildSideStats()
{
    auto* panel = new QFrame;
    panel->setObjectName("WatchStatsPanel");
    panel->setFixedWidth(164);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(0);
    layout->addWidget(buildStatRow("Host", hostLabel_));
    layout->addWidget(buildStatRow("Connection", connectionLabel_));
    layout->addWidget(buildStatRow("A/V Sync", avSyncLabel_));
    layout->addWidget(buildStatRow("Quality", qualityLabel_));
    layout->addWidget(buildStatRow("Resolution", resolutionLabel_));
    layout->addWidget(buildStatRow("FPS", fpsLabel_));
    layout->addWidget(buildStatRow("Bitrate", bitrateLabel_));
    layout->addWidget(buildStatRow("Latency", latencyLabel_));
    layout->addStretch(1);

    // Remote-control permissions: shown only while the host has granted control.
    controlStatusRow_ = new QWidget;
    auto* controlRowLayout = new QVBoxLayout(controlStatusRow_);
    controlRowLayout->setContentsMargins(0, 12, 0, 0);
    controlRowLayout->setSpacing(8);
    controlRowLayout->addWidget(textLabel("Your Control", "ActiveMuted"));
    auto* iconsRow = new QWidget;
    auto* iconsLayout = new QHBoxLayout(iconsRow);
    iconsLayout->setContentsMargins(0, 0, 0, 0);
    iconsLayout->setSpacing(10);
    auto makeControlIcon = [&](const QString& tooltip) {
        auto* iconLabel = new QLabel;
        iconLabel->setObjectName("WatchControlIcon");
        iconLabel->setFixedSize(24, 24);
        iconLabel->setAlignment(Qt::AlignCenter);
        iconLabel->setToolTip(tooltip);
        iconLabel->setVisible(false);
        return iconLabel;
    };
    controlMouseIcon_ = makeControlIcon("You can control the mouse");
    controlKeyboardIcon_ = makeControlIcon("You can control the keyboard");
    controlGamepadIcon_ = makeControlIcon("You can control a gamepad");
    iconsLayout->addWidget(controlMouseIcon_);
    iconsLayout->addWidget(controlKeyboardIcon_);
    iconsLayout->addWidget(controlGamepadIcon_);
    iconsLayout->addStretch(1);
    controlRowLayout->addWidget(iconsRow);
    gamepadSelector_ = new QComboBox;
    gamepadSelector_->setObjectName("WatchGamepadSelector");
    gamepadSelector_->setToolTip("Physical controller sent to the host");
    gamepadSelector_->addItem("No controller", -1);
    connect(gamepadSelector_, &QComboBox::currentIndexChanged, this, [this](int) {
        sendNeutralGamepad();
        lastGamepadState_.reset();
        lastGamepadSentMs_ = -1;
    });
    controlRowLayout->addWidget(gamepadSelector_);
    controlStatusRow_->setVisible(false);
    layout->addWidget(controlStatusRow_);
    return panel;
}

QWidget* ActiveWatchWindow::buildFooter()
{
    auto* footer = new QWidget;
    footer->setObjectName("ActiveFooter");
    auto* layout = new QHBoxLayout(footer);
    layout->setContentsMargins(0, 14, 0, 0);
    layout->setSpacing(12);

    auto* volumePanel = new QFrame;
    volumePanel->setObjectName("WatchVolumePanel");
    auto* volumeLayout = new QHBoxLayout(volumePanel);
    volumeLayout->setContentsMargins(14, 8, 14, 8);
    volumeLayout->setSpacing(10);

    auto* volumeIcon = new QLabel;
    volumeIcon->setFixedSize(24, 24);
    volumeIcon->setAlignment(Qt::AlignCenter);
    volumeIcon->setPixmap(renderSvgResource(
        QStringLiteral(":/screenshare/ui/icons/volume.svg"),
        QSize(24, 24),
        QStringLiteral("#eaf5f2")));
    volumeLayout->addWidget(volumeIcon, 0, Qt::AlignVCenter);

    volumeSlider_ = new VolumeSlider;
    volumeSlider_->setObjectName("WatchVolumeSlider");
    volumeSlider_->setMinimumWidth(70);
    volumeSlider_->setMaximumWidth(200);
    volumeSlider_->setFixedHeight(24);
    volumeSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    volumeSlider_->setRange(0, 200);
    volumeSlider_->setValue(100);
    connect(volumeSlider_, &QSlider::valueChanged, this, [this](int value) {
        applyVolume(value);
    });
    volumeLayout->addWidget(volumeSlider_);
    volumePercentLabel_ = textLabel("100%", "ActiveMetricValue");
    volumePercentLabel_->setFixedWidth(42);
    volumePercentLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    volumeLayout->addWidget(volumePercentLabel_);

    muteButton_ = iconButton("Mute", "WatchInlineButton", "volume");
    muteButton_->setFixedWidth(92);
    connect(muteButton_, &QPushButton::clicked, this, [this] {
        toggleMute();
    });
    volumeLayout->addWidget(muteButton_);

    constexpr int footerControlHeight = 56;
    // Let the volume panel flex (and the slider shrink) so the footer compresses
    // gracefully on small windows instead of overlapping the buttons.
    volumePanel->setFixedHeight(footerControlHeight);
    volumePanel->setMinimumWidth(210);
    volumePanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(volumePanel, 1);

    fullscreenButton_ = iconButton("Fullscreen", "ActiveSecondaryButton", "fullscreen");
    connect(fullscreenButton_, &QPushButton::clicked, this, [this] {
        toggleFullscreen();
    });
    fullscreenButton_->setFixedSize(140, footerControlHeight);
    layout->addWidget(fullscreenButton_);

    controlButton_ = iconButton("Request Control", "ActiveSecondaryButton", "");
    controlButton_->setFixedSize(190, footerControlHeight);
    connect(controlButton_, &QPushButton::clicked, this, [this] {
        toggleControlRequest();
    });
    layout->addWidget(controlButton_);

    leaveButton_ = iconButton("Leave Room", "WatchLeaveButton", "stop");
    leaveButton_->setFixedSize(220, footerControlHeight);
    connect(leaveButton_, &QPushButton::clicked, this, [this] {
        leaveRoom();
    });
    layout->addWidget(leaveButton_);
    return footer;
}

QWidget* ActiveWatchWindow::buildStatRow(const QString& label, QLabel*& valueLabel)
{
    auto* row = new QWidget;
    row->setObjectName("WatchStatRow");
    row->setMinimumHeight(62);
    auto* layout = new QVBoxLayout(row);
    layout->setContentsMargins(0, 10, 0, 10);
    layout->setSpacing(4);
    layout->addWidget(textLabel(label, "ActiveMuted"));
    valueLabel = textLabel("-", "WatchStatValue");
    layout->addWidget(valueLabel);
    return row;
}

QPushButton* ActiveWatchWindow::iconButton(const QString& text, const QString& objectName, const char* iconName)
{
    auto* button = new QPushButton(text);
    button->setObjectName(objectName);
    button->setCursor(Qt::PointingHandCursor);
    if (iconName == nullptr || *iconName == '\0') {
        return button;
    }
    const QPixmap pixmap = renderSvgResource(
        QStringLiteral(":/screenshare/ui/icons/%1.svg").arg(QString::fromUtf8(iconName)),
        QSize(18, 18),
        QStringLiteral("#ffffff"));
    if (!pixmap.isNull()) {
        button->setIcon(QIcon(pixmap));
        button->setIconSize(QSize(18, 18));
    }
    return button;
}

QLabel* ActiveWatchWindow::textLabel(const QString& text, const char* objectName)
{
    auto* label = new QLabel(text);
    label->setObjectName(QString::fromUtf8(objectName));
    label->setWordWrap(true);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    return label;
}

void ActiveWatchWindow::installBackendHandlers()
{
    if (backend_ == nullptr) {
        return;
    }
    backend_->setStartedHandler({});
    backend_->setErrorHandler([this](const QString& message) {
        videoFrameWidget_->setStatusText(message.isEmpty() ? QStringLiteral("Watching failed") : message);
        stateLabel_->setText("● Failed");
        stateLabel_->setObjectName("ActiveTopError");
        stateLabel_->style()->unpolish(stateLabel_);
        stateLabel_->style()->polish(stateLabel_);
    });
    backend_->setFinishedHandler([this](const QtSessionBackend::FinishInfo& info) {
        handleFinished(info);
    });
    backend_->setStatusHandler([this](const screenshare::SessionEvent& event) {
        updateStatus(event.status);
        if (event.type == screenshare::SessionEventType::ControlStateChanged) {
            handleControlState(event.controlCapabilities);
            return;
        }
        if (event.type == screenshare::SessionEventType::Issue) {
            if (event.issue == screenshare::SessionIssue::HostLeft) {
                handleHostLeft();
            } else if (
                event.issue == screenshare::SessionIssue::AccessCodeRequired ||
                event.issue == screenshare::SessionIssue::AccessCodeMismatch) {
                if (session_.config.connectionMode == screenshare::WatchConnectionMode::Room) {
                    if (!receivedVideoFrame_) {
                        setPreviewStatusText("Waiting for encrypted stream");
                        connectionLabel_->setText("Connecting");
                        stateLabel_->setText("● Connecting");
                        stateLabel_->setObjectName("ActiveTopIdle");
                        stateLabel_->style()->unpolish(stateLabel_);
                        stateLabel_->style()->polish(stateLabel_);
                    }
                    return;
                }
                setPreviewStatusText("Room password or encryption key mismatch");
                connectionLabel_->setText("Security issue");
                stateLabel_->setText("● Security issue");
                stateLabel_->setObjectName("ActiveTopError");
                stateLabel_->style()->unpolish(stateLabel_);
                stateLabel_->style()->polish(stateLabel_);
            }
        }
    });
    backend_->setVideoFrameHandler({});
    backend_->setDirectVideoFrameHandler([this](screenshare::SessionEvent::VideoFrame frame) {
        if (videoFrameWidget_ != nullptr) {
            videoFrameWidget_->presentVideoFrameAsync(std::move(frame));
        }

        bool expected = false;
        if (!firstVideoFramePosted_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }

        QMetaObject::invokeMethod(
            this,
            [this] {
                if (hostLeft_) {
                    return;
                }
                receivedVideoFrame_ = true;
                if (videoFrameWidget_ != nullptr) {
                    videoFrameWidget_->showVideoSurface();
                }
                if (previewStatusText_ == QStringLiteral("Waiting for encrypted stream")) {
                    setPreviewStatusText("Receiving stream");
                }
            },
            Qt::QueuedConnection);
    });
}

void ActiveWatchWindow::startWatch()
{
    if (backend_ == nullptr) {
        setPreviewStatusText("Session backend unavailable");
        stateLabel_->setText("● Failed");
        stateLabel_->setObjectName("ActiveTopError");
        stateLabel_->style()->unpolish(stateLabel_);
        stateLabel_->style()->polish(stateLabel_);
        return;
    }

    screenshare::WatchSessionConfig config = session_.config;
    config.emitVideoFrames = true;

    QString error;
    if (!backend_->startWatch(config, &error)) {
        setPreviewStatusText(error.isEmpty() ? QStringLiteral("Could not join room") : error);
        stateLabel_->setText("● Failed");
        stateLabel_->setObjectName("ActiveTopError");
        stateLabel_->style()->unpolish(stateLabel_);
        stateLabel_->style()->polish(stateLabel_);
        elapsedTimer_->stop();
        return;
    }

    updateStatus(backend_->currentStatus());
}

void ActiveWatchWindow::updateElapsed()
{
    const qint64 seconds = elapsed_.isValid() ? elapsed_.elapsed() / 1000 : 0;
    elapsedLabel_->setText(QStringLiteral("%1:%2:%3")
        .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0')));
}

void ActiveWatchWindow::updateStatus(const screenshare::SessionStatus& status)
{
    if (hostLeft_ && status.state != screenshare::SessionState::Stopping && status.state != screenshare::SessionState::Failed) {
        stateLabel_->setText("● Host left");
        stateLabel_->setObjectName("ActiveTopError");
        stateLabel_->style()->unpolish(stateLabel_);
        stateLabel_->style()->polish(stateLabel_);
        setPreviewStatusText("Host left the room");
        connectionLabel_->setText("Host left");
        return;
    }

    stateLabel_->setText(stateText(status));
    stateLabel_->setObjectName(
        (status.state == screenshare::SessionState::Failed ||
         (status.state == screenshare::SessionState::Disconnected && status.summary == "Host left the room"))
            ? "ActiveTopError"
            : "ActiveTopLive");
    stateLabel_->style()->unpolish(stateLabel_);
    stateLabel_->style()->polish(stateLabel_);
    if (status.state == screenshare::SessionState::Disconnected && !status.summary.empty()) {
        setPreviewStatusText(QString::fromStdString(status.summary));
    } else {
        setPreviewStatusText(status.state == screenshare::SessionState::Live ? "Receiving stream" : "Waiting for stream");
    }
    // While a control grant is active, keep the "You control" indicator; the
    // ordinary ~1/sec status tick would otherwise clobber it back to the plain
    // connection text.
    if (controlCapabilities_ != 0) {
        connectionLabel_->setText(QStringLiteral("You control: %1").arg(controlTypesString(controlCapabilities_)));
    } else {
        connectionLabel_->setText(connectionText(status));
    }
    avSyncLabel_->setText(avSyncText(status.audio));
    qualityLabel_->setText(status.stream.hasStats ? QStringLiteral("High") : QStringLiteral("-"));
    resolutionLabel_->setText(resolutionText(status));
    refreshFpsLabel(status.stream.outputFps);
    updateReceiveBitrate(status.payloadBytes);
    bitrateLabel_->setText(bitrateText(receiveBitrateMbps_));
    if (latencyLabel_ != nullptr) {
        latencyLabel_->setText(latencyText(status.audio));
    }
    updateAudioControls(status.audio);
}

void ActiveWatchWindow::updateReceiveBitrate(uint64_t payloadBytes)
{
    // The stream's bitrate field is sender-side and always 0 on the viewer, so
    // derive the received bitrate from the cumulative payload-byte counter.
    const qint64 nowMs = elapsed_.isValid() ? elapsed_.elapsed() : 0;
    if (lastBitrateSampleMs_ < 0 || payloadBytes < lastBitrateBytes_) {
        // First sample, or the counter rewound (stream restart): re-baseline.
        lastBitrateSampleMs_ = nowMs;
        lastBitrateBytes_ = payloadBytes;
        receiveBitrateMbps_ = 0.0;
        return;
    }
    const qint64 deltaMs = nowMs - lastBitrateSampleMs_;
    if (deltaMs < 500) {
        return; // keep the previous value until a meaningful interval elapses
    }
    const uint64_t deltaBytes = payloadBytes - lastBitrateBytes_;
    receiveBitrateMbps_ =
        static_cast<double>(deltaBytes) * 8.0 / (static_cast<double>(deltaMs) / 1000.0) / 1'000'000.0;
    lastBitrateBytes_ = payloadBytes;
    lastBitrateSampleMs_ = nowMs;
}

void ActiveWatchWindow::updateAudioControls(const screenshare::SessionAudioStatus& audio)
{
    if (volumeSlider_ == nullptr || volumePercentLabel_ == nullptr || muteButton_ == nullptr) {
        return;
    }
    if (audioControlsTouched_ || !audio.hasStats) {
        return;
    }

    audioMuted_ = audio.playbackMuted;
    audioVolumePercent_ = std::clamp(audio.playbackVolumePercent, 0, 200);
    audioControlsInitialized_ = true;
    {
        const QSignalBlocker blocker(volumeSlider_);
        volumeSlider_->setValue(audioVolumePercent_);
    }
    volumePercentLabel_->setText(QStringLiteral("%1%").arg(audioVolumePercent_));
    refreshMuteButton();
}

void ActiveWatchWindow::applyVolume(int volumePercent)
{
    volumePercent = std::clamp(volumePercent, 0, 200);
    audioControlsInitialized_ = true;
    audioControlsTouched_ = true;
    audioVolumePercent_ = volumePercent;
    if (volumePercentLabel_ != nullptr) {
        volumePercentLabel_->setText(QStringLiteral("%1%").arg(volumePercent));
    }
    if (backend_ == nullptr || !backend_->isRunning()) {
        return;
    }

    screenshare::AudioPlaybackSettings settings;
    settings.volumePercent = volumePercent;
    if (volumePercent > 0 && audioMuted_) {
        audioMuted_ = false;
        settings.muted = false;
        refreshMuteButton();
    }
    backend_->applyAudioPlaybackSettings(settings);
}

void ActiveWatchWindow::toggleMute()
{
    audioControlsInitialized_ = true;
    audioControlsTouched_ = true;
    audioMuted_ = !audioMuted_;
    refreshMuteButton();
    if (backend_ == nullptr || !backend_->isRunning()) {
        return;
    }

    screenshare::AudioPlaybackSettings settings;
    settings.muted = audioMuted_;
    backend_->applyAudioPlaybackSettings(settings);
}

void ActiveWatchWindow::refreshMuteButton()
{
    if (muteButton_ == nullptr) {
        return;
    }
    muteButton_->setText(audioMuted_ ? QStringLiteral("Unmute") : QStringLiteral("Mute"));
    const QPixmap pixmap = renderSvgResource(
        QStringLiteral(":/screenshare/ui/icons/%1.svg").arg(audioMuted_ ? QStringLiteral("mute") : QStringLiteral("volume")),
        QSize(18, 18),
        QStringLiteral("#ffffff"));
    if (!pixmap.isNull()) {
        muteButton_->setIcon(QIcon(pixmap));
    }
}

void ActiveWatchWindow::toggleFullscreen()
{
    if (streamFullscreen_) {
        closeStreamFullscreen();
        return;
    }

    setStreamFullscreen(true);
}

void ActiveWatchWindow::closeStreamFullscreen()
{
    if (!streamFullscreen_) {
        return;
    }
    setStreamFullscreen(false);
}

void ActiveWatchWindow::setStreamFullscreen(bool enabled)
{
    if (streamFullscreen_ == enabled) {
        return;
    }

    QWidget* topLevel = window();
    auto* shell = dynamic_cast<AppShellWindow*>(topLevel);
    if (enabled) {
        preFullscreenGeometry_ = topLevel != nullptr ? topLevel->geometry() : QRect();
        preFullscreenState_ = topLevel != nullptr ? topLevel->windowState() : Qt::WindowNoState;
    }

    streamFullscreen_ = enabled;
    installFullscreenEventFilter(enabled);

    if (topStatusWidget_ != nullptr) {
        topStatusWidget_->setVisible(!enabled);
    }
    if (sideStatsPanel_ != nullptr) {
        sideStatsPanel_->setVisible(!enabled);
    }
    if (footerWidget_ != nullptr) {
        footerWidget_->setVisible(!enabled);
    }
    if (rootLayout_ != nullptr) {
        rootLayout_->setContentsMargins(enabled ? 0 : 20, enabled ? 0 : 14, enabled ? 0 : 20, enabled ? 0 : 20);
        rootLayout_->setSpacing(enabled ? 0 : 14);
    }
    if (bodyLayout_ != nullptr) {
        bodyLayout_->setSpacing(enabled ? 0 : 14);
    }
    if (previewPanel_ != nullptr) {
        previewPanel_->setProperty("streamFullscreen", enabled);
        previewPanel_->style()->unpolish(previewPanel_);
        previewPanel_->style()->polish(previewPanel_);
    }
    if (shell != nullptr) {
        shell->setChromeVisible(!enabled);
    }
    if (fullscreenButton_ != nullptr) {
        fullscreenButton_->setText(enabled ? QStringLiteral("Exit Fullscreen") : QStringLiteral("Fullscreen"));
    }

    if (topLevel != nullptr) {
        if (enabled) {
            topLevel->showFullScreen();
            setFocus(Qt::OtherFocusReason);
            if (videoFrameWidget_ != nullptr) {
                videoFrameWidget_->setFocus(Qt::OtherFocusReason);
            }
        } else {
            const bool wasMaximized = (preFullscreenState_ & Qt::WindowMaximized) != 0;
            if (wasMaximized) {
                topLevel->showMaximized();
            } else {
                topLevel->showNormal();
                if (!preFullscreenGeometry_.isNull()) {
                    topLevel->setGeometry(preFullscreenGeometry_);
                }
            }
            // The fullscreen->windowed transition on this frameless window can
            // leave a stale copy of the footer painted. Force a full relayout and
            // repaint once the window has settled into its new state.
            QPointer<QWidget> repaintTarget(topLevel);
            QTimer::singleShot(0, this, [this, repaintTarget] {
                if (layout() != nullptr) {
                    layout()->invalidate();
                    layout()->activate();
                }
                if (repaintTarget != nullptr) {
                    repaintTarget->update();
                }
            });
        }
    }
}

void ActiveWatchWindow::updatePresentedFps()
{
    if (videoFrameWidget_ == nullptr) {
        return;
    }
    const std::uint64_t currentCount = videoFrameWidget_->presentedFrameCount();
    if (!presentedFpsTimer_.isValid() || lastPresentedFrameCount_ == 0) {
        lastPresentedFrameCount_ = currentCount;
        presentedFpsTimer_.restart();
        return;
    }

    const qint64 elapsedMs = presentedFpsTimer_.elapsed();
    if (elapsedMs < 1000) {
        return;
    }

    const std::uint64_t presentedDelta = currentCount - lastPresentedFrameCount_;
    presentedFps_ = static_cast<double>(presentedDelta) * 1000.0 / static_cast<double>(elapsedMs);
    lastPresentedFrameCount_ = currentCount;
    presentedFpsTimer_.restart();
}

void ActiveWatchWindow::refreshFpsLabel(double streamFps)
{
    latestStreamFps_ = streamFps;
    updatePresentedFps();
    if (fpsLabel_ == nullptr) {
        return;
    }
    if (presentedFps_ > 0.0) {
        fpsLabel_->setText(fpsText(presentedFps_));
        const auto stats = videoFrameWidget_ != nullptr
            ? videoFrameWidget_->presentationStats()
            : VideoFrameWidget::PresentationStats{};
        fpsLabel_->setToolTip(QStringLiteral(
            "Presented FPS\n"
            "Queued: %1\n"
            "Dropped: %2\n"
            "Present: %3 ms avg / %4 ms max / %5 ms last")
            .arg(stats.queuedFrames)
            .arg(stats.droppedFrames)
            .arg(stats.averagePresentMs, 0, 'f', 2)
            .arg(stats.maxPresentMs, 0, 'f', 2)
            .arg(stats.lastPresentMs, 0, 'f', 2));
        return;
    }
    fpsLabel_->setText(fpsText(streamFps));
    fpsLabel_->setToolTip(QStringLiteral("Stream FPS"));
}

void ActiveWatchWindow::installFullscreenEventFilter(bool installed)
{
    if (fullscreenEventFilterInstalled_ == installed) {
        return;
    }
    if (QApplication* app = qobject_cast<QApplication*>(QApplication::instance())) {
        if (installed) {
            app->installEventFilter(this);
        } else {
            app->removeEventFilter(this);
        }
    }
    if (window() != nullptr) {
        if (installed) {
            window()->installEventFilter(this);
        } else {
            window()->removeEventFilter(this);
        }
    }
    fullscreenEventFilterInstalled_ = installed;
}

void ActiveWatchWindow::setPreviewStatusText(const QString& text)
{
    previewStatusText_ = text;
    if (videoFrameWidget_ != nullptr) {
        videoFrameWidget_->setStatusText(text);
    }
}

void ActiveWatchWindow::handleHostLeft()
{
    if (hostLeft_) {
        return;
    }
    hostLeft_ = true;
    closeStreamFullscreen();
    if (videoFrameWidget_ != nullptr) {
        videoFrameWidget_->clearFrame();
    }
    setPreviewStatusText("Host ended the session");
    if (stateLabel_ != nullptr) {
        stateLabel_->setText("● Host left");
        stateLabel_->setObjectName("ActiveTopError");
        stateLabel_->style()->unpolish(stateLabel_);
        stateLabel_->style()->polish(stateLabel_);
    }
    if (connectionLabel_ != nullptr) {
        connectionLabel_->setText("Host left");
    }

    // Leave the room right away. This stops the live signaling refresh loop, so
    // the watcher stops re-joining and re-creating the now host-less room in the
    // directory (otherwise it lingers there with a random-looking name). The
    // popup + navigation home then runs from handleFinished once stop completes.
    if (backend_ != nullptr && backend_->isRunning()) {
        leaveRequested_ = true;
        if (leaveButton_ != nullptr) {
            leaveButton_->setEnabled(false);
            leaveButton_->setText("Leaving...");
        }
        backend_->stop();
    } else {
        QTimer::singleShot(0, this, [this] { exitToHomeAfterHostLeft(); });
    }
}

void ActiveWatchWindow::exitToHomeAfterHostLeft()
{
    if (hostLeftHandled_) {
        return;
    }
    hostLeftHandled_ = true;

    // Return straight to the home screen and let the shell show a brief,
    // non-blocking notice there explaining why. No modal to dismiss.
    if (actions_.sessionEndedByHost) {
        actions_.sessionEndedByHost();
    } else if (actions_.sessionStopped) {
        actions_.sessionStopped();
    }
}

void ActiveWatchWindow::handleControlState(uint32_t capabilities)
{
    const bool hadGamepad = (controlCapabilities_ & screenshare::ControlCapabilityGamepad) != 0;
    const bool hasGamepad = (capabilities & screenshare::ControlCapabilityGamepad) != 0;
    if (hadGamepad && !hasGamepad) {
        sendNeutralGamepad();
    }
    controlCapabilities_ = capabilities;
    const bool mouse = (capabilities & screenshare::ControlCapabilityMouse) != 0;
    const bool keyboard = (capabilities & screenshare::ControlCapabilityKeyboard) != 0;
    const bool gamepad = (capabilities & screenshare::ControlCapabilityGamepad) != 0;
    if (videoFrameWidget_ != nullptr) {
        videoFrameWidget_->setControlCapture(mouse || keyboard, mouse, keyboard);
    }
    gamepadTimer_->setInterval(gamepad ? 8 : 100);
    if (gamepad) {
        refreshGamepadSlots();
    }
    const QString types = controlTypesString(capabilities);
    if (controlButton_ != nullptr) {
        controlButton_->setText(capabilities != 0
            ? QStringLiteral("Release Control")
            : QStringLiteral("Request Control"));
    }
    const auto applyControlIcon = [](QLabel* iconLabel, const char* name, bool active) {
        if (iconLabel == nullptr) {
            return;
        }
        iconLabel->setVisible(active);
        if (active) {
            iconLabel->setPixmap(renderSvgResource(
                QStringLiteral(":/screenshare/ui/icons/%1.svg").arg(QString::fromUtf8(name)),
                QSize(20, 20),
                QStringLiteral("#38b27e")));
        }
    };
    applyControlIcon(controlMouseIcon_, "mouse", mouse);
    applyControlIcon(controlKeyboardIcon_, "keyboard", keyboard);
    applyControlIcon(controlGamepadIcon_, "gamepad", gamepad);
    if (gamepadSelector_ != nullptr) {
        gamepadSelector_->setVisible(gamepad);
    }
    if (controlStatusRow_ != nullptr) {
        controlStatusRow_->setVisible(capabilities != 0);
    }
    if (connectionLabel_ != nullptr && capabilities != 0) {
        connectionLabel_->setText(QStringLiteral("You control: %1").arg(types));
    }
}

void ActiveWatchWindow::refreshGamepadSlots()
{
    if (gamepadSelector_ == nullptr) {
        return;
    }
    const int previousSlot = gamepadSelector_->currentData().toInt();
    const auto connectedSlots = screenshare::XInputGamepad::ConnectedSlots();
    QSignalBlocker blocker(gamepadSelector_);
    gamepadSelector_->clear();
    if (connectedSlots.empty()) {
        gamepadSelector_->addItem("No controller", -1);
        return;
    }
    int selectedIndex = 0;
    for (int slot : connectedSlots) {
        gamepadSelector_->addItem(QStringLiteral("Controller %1").arg(slot + 1), slot);
        if (slot == previousSlot) {
            selectedIndex = gamepadSelector_->count() - 1;
        }
    }
    gamepadSelector_->setCurrentIndex(selectedIndex);
}

void ActiveWatchWindow::sendNeutralGamepad()
{
    if (backend_ == nullptr || (controlCapabilities_ & screenshare::ControlCapabilityGamepad) == 0) {
        return;
    }
    screenshare::RemoteInputEvent input;
    input.kind = screenshare::RemoteInputKind::GamepadState;
    input.gamepad.controllerSlot = lastGamepadState_.has_value() ?
        lastGamepadState_->controllerSlot :
        static_cast<uint8_t>(std::max(0, gamepadSelector_ != nullptr ? gamepadSelector_->currentData().toInt() : 0));
    backend_->sendRemoteInput(input);
}

void ActiveWatchWindow::pollGamepad()
{
    if ((controlCapabilities_ & screenshare::ControlCapabilityGamepad) == 0) {
        if (++gamepadScanTicks_ >= 5) {
            gamepadScanTicks_ = 0;
            refreshGamepadSlots();
        }
        return;
    }
    if (++gamepadScanTicks_ >= 63) {
        gamepadScanTicks_ = 0;
        refreshGamepadSlots();
    }
    const int slot = gamepadSelector_ != nullptr ? gamepadSelector_->currentData().toInt() : -1;
    const auto state = screenshare::XInputGamepad::ReadState(slot);
    if (!state.has_value()) {
        if (lastGamepadState_.has_value()) {
            sendNeutralGamepad();
            lastGamepadState_.reset();
            lastGamepadSentMs_ = gamepadClock_.elapsed();
        }
        return;
    }

    const qint64 now = gamepadClock_.elapsed();
    if (state == lastGamepadState_ && lastGamepadSentMs_ >= 0 && now - lastGamepadSentMs_ < 100) {
        return;
    }
    screenshare::RemoteInputEvent input;
    input.kind = screenshare::RemoteInputKind::GamepadState;
    input.gamepad = *state;
    if (backend_ != nullptr) {
        backend_->sendRemoteInput(input);
    }
    lastGamepadState_ = state;
    lastGamepadSentMs_ = now;
}

void ActiveWatchWindow::toggleControlRequest()
{
    if (backend_ == nullptr) {
        return;
    }
    if (controlCapabilities_ != 0) {
        backend_->releaseControl();
        handleControlState(0); // optimistic: the host does not ack our own release
        return;
    }
    backend_->requestControl(
        screenshare::ControlCapabilityMouse |
        screenshare::ControlCapabilityKeyboard |
        screenshare::ControlCapabilityGamepad);
    if (controlButton_ != nullptr) {
        controlButton_->setText(QStringLiteral("Requesting..."));
    }
}

void ActiveWatchWindow::leaveRoom()
{
    leaveRequested_ = true;
    if (backend_ == nullptr || !backend_->isRunning()) {
        if (actions_.sessionStopped) {
            actions_.sessionStopped();
        }
        return;
    }
    leaveButton_->setEnabled(false);
    leaveButton_->setText("Leaving...");
    backend_->stop();
}

void ActiveWatchWindow::handleFinished(const QtSessionBackend::FinishInfo& info)
{
    closeStreamFullscreen();
    elapsedTimer_->stop();
    leaveButton_->setEnabled(true);
    leaveButton_->setText("Leave Room");
    if (hostLeft_) {
        // The session stopped because the host left. Return home (with a toast on
        // the home screen), deferred so we do not destroy this window from inside
        // the backend's finished callback.
        QTimer::singleShot(0, this, [this] { exitToHomeAfterHostLeft(); });
        return;
    }
    setPreviewStatusText(info.failed ? "Watching failed" : "Room left");
    if (actions_.sessionStopped) {
        QTimer::singleShot(info.failed ? 900 : 300, this, [this] {
            actions_.sessionStopped();
        });
    }
}

QString ActiveWatchWindow::stateText(const screenshare::SessionStatus& status) const
{
    switch (status.state) {
    case screenshare::SessionState::Live:
        return QStringLiteral("● Watching");
    case screenshare::SessionState::Disconnected:
        if (status.summary == "Host left the room") {
            return QStringLiteral("● Host left");
        }
        return QStringLiteral("● Disconnected");
    case screenshare::SessionState::Stopping:
        return QStringLiteral("● Leaving");
    case screenshare::SessionState::Failed:
        return QStringLiteral("● Failed");
    default:
        return backend_ != nullptr && backend_->isRunning() ? QStringLiteral("● Watching") : QStringLiteral("● Idle");
    }
}

QString ActiveWatchWindow::connectionText(const screenshare::SessionStatus& status) const
{
    if (status.health == "ok" || status.state == screenshare::SessionState::Live) {
        return QStringLiteral("Good");
    }
    if (!status.health.empty()) {
        QString health = QString::fromStdString(status.health);
        health[0] = health[0].toUpper();
        return health;
    }
    return QStringLiteral("Waiting");
}

QString ActiveWatchWindow::resolutionText(const screenshare::SessionStatus& status) const
{
    const QString streamResolution = formatResolution(status.stream.outputResolution);
    if (streamResolution != "-") {
        return streamResolution;
    }
    return formatResolution(status.videoResolution);
}
