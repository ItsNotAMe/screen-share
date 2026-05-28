#include "ui/ActiveWatchWindow.h"

#include "ui/AppShellWindow.h"
#include "ui/UiStyle.h"
#include "ui/VideoFrameWidget.h"

#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QIODevice>
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
    layout->addStretch(1);
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
    volumeSlider_->setFixedWidth(180);
    volumeSlider_->setFixedHeight(24);
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
    volumePanel->setFixedSize(410, footerControlHeight);
    layout->addWidget(volumePanel);

    fullscreenButton_ = iconButton("Fullscreen", "ActiveSecondaryButton", "fullscreen");
    connect(fullscreenButton_, &QPushButton::clicked, this, [this] {
        toggleFullscreen();
    });
    fullscreenButton_->setFixedSize(150, footerControlHeight);
    layout->addWidget(fullscreenButton_);

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
    backend_->setVideoFrameHandler([this](const screenshare::SessionEvent::VideoFrame& frame) {
        if (hostLeft_) {
            return;
        }
        receivedVideoFrame_ = true;
        if (previewStatusText_ == QStringLiteral("Waiting for encrypted stream")) {
            setPreviewStatusText("Receiving stream");
        }
        videoFrameWidget_->setVideoFrame(frame);
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
    connectionLabel_->setText(connectionText(status));
    avSyncLabel_->setText(status.audio.hasStats ? QStringLiteral("%1 ms").arg(status.audio.avAudioAheadMs) : QStringLiteral("-"));
    qualityLabel_->setText(status.stream.hasStats ? QStringLiteral("High") : QStringLiteral("-"));
    resolutionLabel_->setText(resolutionText(status));
    fpsLabel_->setText(fpsText(status.stream.outputFps));
    bitrateLabel_->setText(bitrateText(status.stream.bitrateMbps));
    updateAudioControls(status.audio);
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
        }
    }
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
    hostLeft_ = true;
    closeStreamFullscreen();
    if (videoFrameWidget_ != nullptr) {
        videoFrameWidget_->clearFrame();
    }
    setPreviewStatusText("Host left the room");
    if (stateLabel_ != nullptr) {
        stateLabel_->setText("● Host left");
        stateLabel_->setObjectName("ActiveTopError");
        stateLabel_->style()->unpolish(stateLabel_);
        stateLabel_->style()->polish(stateLabel_);
    }
    if (connectionLabel_ != nullptr) {
        connectionLabel_->setText("Host left");
    }
    if (leaveButton_ != nullptr) {
        leaveButton_->setEnabled(true);
        leaveButton_->setText("Leave Room");
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
    if (hostLeft_ && !leaveRequested_) {
        handleHostLeft();
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
