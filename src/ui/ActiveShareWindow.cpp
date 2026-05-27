#include "ui/ActiveShareWindow.h"

#include "ui/UiStyle.h"

#include <QtCore/QByteArray>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QIODevice>
#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtCore/QSizeF>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
#include <QtGui/QPaintEvent>
#include <QtGui/QPainter>
#include <QtGui/QPen>
#include <QtGui/QPixmap>
#include <QtGui/QWheelEvent>
#include <QtSvg/QSvgRenderer>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLayoutItem>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace {

constexpr int kSettingsFieldMaxWidth = 286;

class SettingsComboBox final : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QComboBox::paintEvent(event);

        constexpr int arrowWidth = 36;
        const QRect arrowRect(width() - arrowWidth - 1, 1, arrowWidth, height() - 2);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(arrowRect.adjusted(1, 0, 0, 0), QColor("#252b2a"));
        painter.setPen(QPen(QColor("#313c3a"), 1));
        painter.drawLine(arrowRect.left(), arrowRect.top() + 1, arrowRect.left(), arrowRect.bottom() - 1);

        const QPointF center = arrowRect.center();
        QPen chevronPen(isEnabled() ? QColor("#d8e4e0") : QColor("#77837f"), 1.8);
        chevronPen.setCapStyle(Qt::RoundCap);
        chevronPen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(chevronPen);
        painter.drawLine(QPointF(center.x() - 4.0, center.y() - 2.0), QPointF(center.x(), center.y() + 2.0));
        painter.drawLine(QPointF(center.x(), center.y() + 2.0), QPointF(center.x() + 4.0, center.y() - 2.0));
    }
};

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

QString resolutionText(const std::optional<screenshare::SessionResolution>& resolution)
{
    if (!resolution || resolution->width <= 0 || resolution->height <= 0) {
        return QStringLiteral("-");
    }
    return QStringLiteral("%1 x %2").arg(resolution->width).arg(resolution->height);
}

QString viewerHealthText(const screenshare::SessionViewer& viewer)
{
    if (viewer.health.empty()) {
        return viewer.hasFeedback ? QStringLiteral("Good") : QStringLiteral("Connecting");
    }
    QString health = QString::fromStdString(viewer.health);
    if (health == "ok") {
        return QStringLiteral("Good");
    }
    health[0] = health[0].toUpper();
    return health;
}

int activeViewerCount(const std::vector<screenshare::SessionViewer>& viewers)
{
    return static_cast<int>(std::count_if(viewers.begin(), viewers.end(), [](const screenshare::SessionViewer& viewer) {
        return viewer.hasFeedback || viewer.activeNow;
    }));
}

std::optional<screenshare::SessionResolution> parseResolutionValue(const QString& value)
{
    const QStringList parts = value.split('x');
    if (parts.size() != 2) {
        return std::nullopt;
    }
    bool widthOk = false;
    bool heightOk = false;
    const int width = parts[0].toInt(&widthOk);
    const int height = parts[1].toInt(&heightOk);
    if (!widthOk || !heightOk || width <= 0 || height <= 0) {
        return std::nullopt;
    }
    return screenshare::SessionResolution{width, height};
}

uint32_t mbpsToBps(int mbps)
{
    return static_cast<uint32_t>(mbps) * 1'000'000U;
}

uint32_t comboBitrateBps(const QComboBox* combo)
{
    if (combo == nullptr) {
        return 0;
    }
    bool ok = false;
    const uint parsed = combo->currentData().toUInt(&ok);
    return ok ? static_cast<uint32_t>(parsed) : 0;
}

std::string toStdUtf8(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<size_t>(bytes.size()));
}

QString viewerListSignature(const screenshare::SessionStatus& status)
{
    QStringList parts;
    parts.reserve(static_cast<qsizetype>(status.viewers.size()) + 1);
    parts << QStringLiteral("%1/%2")
        .arg(activeViewerCount(status.viewers))
        .arg(static_cast<int>(status.viewers.size()));
    for (const auto& viewer : status.viewers) {
        parts << QStringLiteral("%1|%2|%3|%4|%5|%6")
            .arg(QString::fromStdString(viewer.id))
            .arg(QString::fromStdString(viewer.sessionFingerprint))
            .arg(QString::fromStdString(viewer.health))
            .arg(static_cast<int>(viewer.state))
            .arg(viewer.hasFeedback ? 1 : 0)
            .arg(viewer.activeNow ? 1 : 0);
    }
    return parts.join(';');
}

} // namespace

ActiveShareWindow::ActiveShareWindow(QtSessionBackend* backend, Actions actions, QWidget* parent)
    : QWidget(parent), backend_(backend), actions_(std::move(actions))
{
    setObjectName("ActiveShareWindow");
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

void ActiveShareWindow::setSession(const ShareSessionUiState& session)
{
    session_ = session;
    installBackendHandlers();
    elapsed_.restart();
    elapsedTimer_->start();
    lastViewerSignature_.clear();
    hostAudioMuted_ = session_.config.hostAudioMuted || !session_.config.captureSystemAudio;
    videoPaused_ = session_.config.hostVideoPaused;
    updateElapsed();
    updateShareSummary();
    updateHostControlButtons();
    updateStatus(backend_ != nullptr ? backend_->currentStatus() : screenshare::SessionStatus{});
    showSettingsPanel(false);
}

bool ActiveShareWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event != nullptr && event->type() == QEvent::Wheel) {
        if (auto* combo = qobject_cast<QComboBox*>(watched)) {
            if (combo->view() == nullptr || !combo->view()->isVisible()) {
                auto* wheel = static_cast<QWheelEvent*>(event);
                if (settingsScrollArea_ != nullptr && settingsScrollArea_->verticalScrollBar() != nullptr) {
                    QScrollBar* scrollBar = settingsScrollArea_->verticalScrollBar();
                    scrollBar->setValue(scrollBar->value() - wheel->angleDelta().y());
                }
                event->accept();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

QWidget* ActiveShareWindow::buildShell()
{
    auto* host = new QWidget;
    host->setObjectName("ActiveContent");
    auto* stack = new QGridLayout(host);
    stack->setContentsMargins(0, 0, 0, 0);
    stack->setSpacing(0);

    auto* page = new QWidget;
    page->setObjectName("ActiveContentPage");
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(20, 14, 20, 20);
    root->setSpacing(14);
    root->addWidget(buildTopStatus());

    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(14);

    auto* mainColumn = new QVBoxLayout;
    mainColumn->setContentsMargins(0, 0, 0, 0);
    mainColumn->setSpacing(14);
    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(14);
    grid->addWidget(buildShareCard(), 0, 0);
    grid->addWidget(buildHealthCard(), 1, 0);
    grid->addWidget(buildViewersCard(), 0, 1, 2, 1);
    grid->setColumnStretch(0, 3);
    grid->setColumnStretch(1, 2);
    grid->setRowStretch(0, 2);
    grid->setRowStretch(1, 3);
    mainColumn->addLayout(grid, 1);
    mainColumn->addWidget(buildFooter());
    body->addLayout(mainColumn, 1);

    root->addLayout(body, 1);
    stack->addWidget(page, 0, 0);
    stack->addWidget(buildSettingsOverlay(), 0, 0);
    return host;
}

QWidget* ActiveShareWindow::buildTopStatus()
{
    auto* bar = new QWidget;
    bar->setObjectName("ActiveTransparentBlock");
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 0, 146, 0);
    layout->setSpacing(18);
    layout->addWidget(textLabel("ScreenShare", "ActiveTopTitle"));
    stateLabel_ = textLabel("Sharing", "ActiveTopSharing");
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

QWidget* ActiveShareWindow::buildShareCard()
{
    auto* card = new QFrame;
    card->setObjectName("ActiveCard");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(16);

    auto* summary = new QHBoxLayout;
    summary->setContentsMargins(0, 0, 0, 0);
    summary->setSpacing(18);
    summary->addWidget(iconLabel("display", 54), 0, Qt::AlignTop);
    auto* text = new QVBoxLayout;
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(5);
    shareTitleLabel_ = textLabel("You are sharing", "ActiveHeroTitle");
    text->addWidget(shareTitleLabel_);
    shareDisplayLabel_ = textLabel("-", "ActiveMuted");
    text->addWidget(shareDisplayLabel_);
    shareAudioLabel_ = textLabel("-", "ActiveMuted");
    text->addWidget(shareAudioLabel_);
    summary->addLayout(text, 1);
    layout->addLayout(summary);

    auto* controls = new QHBoxLayout;
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setSpacing(12);
    muteAudioButton_ = iconButton("Mute Audio", "ActiveSecondaryButton", "mute");
    connect(muteAudioButton_, &QPushButton::clicked, this, [this] {
        toggleHostAudio();
    });
    controls->addWidget(muteAudioButton_);
    pauseVideoButton_ = iconButton("Pause Video", "ActiveSecondaryButton", "stop");
    connect(pauseVideoButton_, &QPushButton::clicked, this, [this] {
        toggleVideoPause();
    });
    controls->addWidget(pauseVideoButton_);
    layout->addLayout(controls);
    return card;
}

QWidget* ActiveShareWindow::buildViewersCard()
{
    auto* card = new QFrame;
    card->setObjectName("ActiveCard");
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* layout = new QGridLayout(card);
    layout->setContentsMargins(18, 12, 18, 12);
    layout->setHorizontalSpacing(0);
    layout->setVerticalSpacing(8);

    viewerTitleLabel_ = textLabel("Viewers (0 of 0)", "ActiveSectionTitle");
    viewerTitleLabel_->setFixedHeight(28);
    layout->addWidget(viewerTitleLabel_, 0, 0);

    auto* line = new QFrame;
    line->setObjectName("ActiveDivider");
    line->setFixedHeight(1);
    layout->addWidget(line, 1, 0);

    auto* viewerArea = new QWidget;
    viewerArea->setObjectName("ActiveViewerArea");
    viewerListLayout_ = new QVBoxLayout;
    viewerListLayout_->setContentsMargins(0, 0, 0, 0);
    viewerListLayout_->setSpacing(4);
    viewerArea->setLayout(viewerListLayout_);
    layout->addWidget(viewerArea, 2, 0);

    auto* invite = iconButton("Invite", "ActiveSecondaryButton", "viewers");
    invite->setFixedHeight(42);
    connect(invite, &QPushButton::clicked, this, [this] {
        copyInvite();
    });
    layout->addWidget(invite, 3, 0);
    layout->setRowStretch(0, 0);
    layout->setRowStretch(1, 0);
    layout->setRowStretch(2, 1);
    layout->setRowStretch(3, 0);
    return card;
}

QWidget* ActiveShareWindow::buildHealthCard()
{
    auto* card = new QFrame;
    card->setObjectName("ActiveCard");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 14, 18, 14);
    layout->setSpacing(10);
    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->addWidget(textLabel("Stream Health", "ActiveSectionTitle"));
    header->addStretch(1);
    healthStateLabel_ = textLabel("Waiting", "ActiveHealthWaiting");
    header->addWidget(healthStateLabel_);
    layout->addLayout(header);
    auto* metrics = new QGridLayout;
    metrics->setContentsMargins(0, 0, 0, 0);
    metrics->setHorizontalSpacing(0);
    metrics->setVerticalSpacing(0);
    auto* table = new QFrame;
    table->setObjectName("ActiveStatsTable");
    auto* tableLayout = new QGridLayout(table);
    tableLayout->setContentsMargins(0, 0, 0, 0);
    tableLayout->setHorizontalSpacing(0);
    tableLayout->setVerticalSpacing(0);
    auto verticalLine = [] {
        auto* line = new QFrame;
        line->setObjectName("ActiveVerticalDivider");
        line->setFixedWidth(1);
        return line;
    };
    auto horizontalLine = [] {
        auto* line = new QFrame;
        line->setObjectName("ActiveDivider");
        line->setFixedHeight(1);
        return line;
    };
    tableLayout->addWidget(buildMetricCell("Bitrate", bitrateLabel_), 0, 0);
    tableLayout->addWidget(verticalLine(), 0, 1);
    tableLayout->addWidget(buildMetricCell("Resolution", resolutionLabel_), 0, 2);
    tableLayout->addWidget(horizontalLine(), 1, 0, 1, 3);
    tableLayout->addWidget(buildMetricCell("FPS", fpsLabel_), 2, 0);
    tableLayout->addWidget(verticalLine(), 2, 1);
    tableLayout->addWidget(buildMetricCell("Packet Loss", packetLossLabel_), 2, 2);
    tableLayout->addWidget(horizontalLine(), 3, 0, 1, 3);
    tableLayout->addWidget(buildMetricCell("Latency", latencyLabel_), 4, 0);
    tableLayout->addWidget(verticalLine(), 4, 1);
    tableLayout->addWidget(buildMetricCell("Viewers", viewerMetricLabel_), 4, 2);
    tableLayout->setColumnStretch(0, 1);
    tableLayout->setColumnStretch(2, 1);
    metrics->addWidget(table, 0, 0);
    metrics->setColumnStretch(0, 1);
    layout->addLayout(metrics, 1);
    return card;
}

QWidget* ActiveShareWindow::buildFooter()
{
    auto* footer = new QWidget;
    footer->setObjectName("ActiveFooter");
    auto* layout = new QHBoxLayout(footer);
    layout->setContentsMargins(0, 14, 0, 0);
    layout->setSpacing(12);
    auto* settings = iconButton("Room Settings", "ActiveFooterButton", "settings");
    connect(settings, &QPushButton::clicked, this, [this] {
        showSettingsPanel(true);
    });
    layout->addWidget(settings);
    layout->addStretch(1);
    stopButton_ = iconButton("Stop Sharing", "ActiveStopButton", "stop");
    connect(stopButton_, &QPushButton::clicked, this, [this] {
        stopSharing();
    });
    layout->addWidget(stopButton_);
    return footer;
}

QWidget* ActiveShareWindow::buildSettingsOverlay()
{
    settingsOverlay_ = new QFrame;
    settingsOverlay_->setObjectName("ActiveSettingsOverlay");
    auto* layout = new QHBoxLayout(settingsOverlay_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addStretch(1);
    layout->addWidget(buildSettingsPanel());
    settingsOverlay_->hide();
    return settingsOverlay_;
}

QWidget* ActiveShareWindow::buildSettingsPanel()
{
    settingsPanel_ = new QFrame;
    settingsPanel_->setObjectName("ActiveSettingsPanel");
    settingsPanel_->setFixedWidth(360);
    settingsPanel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    auto* layout = new QVBoxLayout(settingsPanel_);
    layout->setContentsMargins(20, 16, 20, 20);
    layout->setSpacing(12);

    auto* header = new QHBoxLayout;
    header->addWidget(textLabel("Room Settings", "ActiveSectionTitle"));
    header->addStretch(1);
    layout->addLayout(header);

    auto* form = new QWidget;
    form->setObjectName("ActiveSettingsForm");
    auto* formLayout = new QVBoxLayout(form);
    formLayout->setContentsMargins(0, 0, 14, 0);
    formLayout->setSpacing(8);
    settingsScrollArea_ = new QScrollArea;
    settingsScrollArea_->setObjectName("ActiveSettingsScroll");
    settingsScrollArea_->setWidgetResizable(true);
    settingsScrollArea_->setFrameShape(QFrame::NoFrame);
    settingsScrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    settingsScrollArea_->setWidget(form);
    layout->addWidget(settingsScrollArea_, 1);

    formLayout->addWidget(textLabel("General", "ActiveSettingsHeading"));
    formLayout->addWidget(textLabel("Room Name", "ActiveSettingsLabel"));
    settingsRoomNameEdit_ = new QLineEdit;
    settingsRoomNameEdit_->setObjectName("RoomSettingsInput");
    settingsRoomNameEdit_->setMaxLength(80);
    settingsRoomNameEdit_->setMaximumWidth(kSettingsFieldMaxWidth);
    connect(settingsRoomNameEdit_, &QLineEdit::textChanged, this, [this] {
        if (!updatingSettingsUi_) {
            updateSettingsApplyState();
        }
    });
    formLayout->addWidget(settingsRoomNameEdit_);
    formLayout->addWidget(textLabel("Room Link", "ActiveSettingsLabel"));
    settingsRoomLinkEdit_ = new QLineEdit;
    settingsRoomLinkEdit_->setObjectName("RoomSettingsInput");
    settingsRoomLinkEdit_->setReadOnly(true);
    settingsRoomLinkEdit_->setMaximumWidth(kSettingsFieldMaxWidth);
    formLayout->addWidget(settingsRoomLinkEdit_);

    formLayout->addWidget(textLabel("Stream Controls", "ActiveSettingsHeading"));
    formLayout->addWidget(textLabel("Max Bitrate", "ActiveSettingsLabel"));
    bitrateCombo_ = new SettingsComboBox;
    configureSettingsChoice(bitrateCombo_);
    bitrateCombo_->addItem("Auto (Recommended)", QString::number(0));
    for (const int mbps : {4, 8, 12, 16, 24, 35, 50, 80}) {
        bitrateCombo_->addItem(QStringLiteral("%1 Mbps").arg(mbps), QString::number(mbpsToBps(mbps)));
    }
    connect(bitrateCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (!updatingSettingsUi_) {
            updateSettingsApplyState();
        }
    });
    formLayout->addWidget(bitrateCombo_);

    formLayout->addWidget(textLabel("Display", "ActiveSettingsLabel"));
    settingsDisplayCombo_ = new SettingsComboBox;
    configureSettingsChoice(settingsDisplayCombo_);
    settingsDisplayCombo_->setEnabled(true);
    connect(settingsDisplayCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (!updatingSettingsUi_) {
            updateSettingsApplyState();
        }
    });
    formLayout->addWidget(settingsDisplayCombo_);

    formLayout->addWidget(textLabel("Resolution", "ActiveSettingsLabel"));
    settingsResolutionCombo_ = new SettingsComboBox;
    configureSettingsChoice(settingsResolutionCombo_);
    settingsResolutionCombo_->setEnabled(true);
    connect(settingsResolutionCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (!updatingSettingsUi_) {
            updateSettingsApplyState();
        }
    });
    formLayout->addWidget(settingsResolutionCombo_);

    formLayout->addWidget(textLabel("FPS", "ActiveSettingsLabel"));
    settingsFpsCombo_ = new SettingsComboBox;
    configureSettingsChoice(settingsFpsCombo_);
    for (const int fps : {60, 30, 24, 15}) {
        settingsFpsCombo_->addItem(QStringLiteral("%1 FPS").arg(fps), fps);
    }
    settingsFpsCombo_->setEnabled(true);
    connect(settingsFpsCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (!updatingSettingsUi_) {
            updateSettingsApplyState();
        }
    });
    formLayout->addWidget(settingsFpsCombo_);

    formLayout->addWidget(textLabel("Audio", "ActiveSettingsHeading"));
    formLayout->addWidget(textLabel("Audio Output", "ActiveSettingsLabel"));
    settingsAudioCombo_ = new SettingsComboBox;
    configureSettingsChoice(settingsAudioCombo_);
    connect(settingsAudioCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (!updatingSettingsUi_) {
            updateSettingsApplyState();
        }
    });
    formLayout->addWidget(settingsAudioCombo_);

    formLayout->addStretch(1);
    auto* buttons = new QHBoxLayout;
    auto* closeButton = iconButton("Close", "ActiveSecondaryButton", nullptr);
    connect(closeButton, &QPushButton::clicked, this, [this] {
        showSettingsPanel(false);
    });
    buttons->addWidget(closeButton);
    settingsApplyButton_ = iconButton("Apply", "RoomStartButton", nullptr);
    if (settingsApplyButton_ != nullptr) {
        settingsApplyButton_->setEnabled(false);
    }
    connect(settingsApplyButton_, &QPushButton::clicked, this, [this] {
        applySettings();
    });
    buttons->addWidget(settingsApplyButton_);
    layout->addLayout(buttons);
    return settingsPanel_;
}

QWidget* ActiveShareWindow::buildMetricTile(const QString& label, QLabel*& valueLabel)
{
    auto* tile = new QFrame;
    tile->setObjectName("ActiveMetricTile");
    auto* layout = new QVBoxLayout(tile);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(4);
    layout->addWidget(textLabel(label, "ActiveMuted"));
    valueLabel = textLabel("-", "ActiveMetricValue");
    valueLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(valueLabel);
    return tile;
}

QWidget* ActiveShareWindow::buildMetricCell(const QString& label, QLabel*& valueLabel)
{
    auto* cell = new QWidget;
    cell->setObjectName("ActiveMetricCell");
    auto* layout = new QVBoxLayout(cell);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(4);
    layout->addWidget(textLabel(label, "ActiveMuted"));
    valueLabel = textLabel("-", "ActiveMetricValue");
    valueLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(valueLabel);
    return cell;
}

QWidget* ActiveShareWindow::buildMetricRow(const QString& label, QLabel*& valueLabel)
{
    auto* row = new QWidget;
    row->setObjectName("ActiveMetricRow");
    row->setFixedHeight(42);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(textLabel(label, "ActiveMuted"));
    valueLabel = textLabel("-", "ActiveMetricValue");
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(valueLabel, 1);
    return row;
}

QWidget* ActiveShareWindow::buildViewerRow(const screenshare::SessionViewer& viewer, int index)
{
    auto* row = new QWidget;
    row->setObjectName("ActiveViewerRow");
    row->setFixedHeight(42);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    layout->addWidget(textLabel(viewer.hasFeedback ? "●" : "○", viewer.hasFeedback ? "ActiveViewerDot" : "ActiveMuted"));
    layout->addWidget(iconLabel("viewers", 22, QStringLiteral("#b7c5c1")));
    const QString name = viewer.sessionFingerprint.empty() ?
        (viewer.id.empty() ? QStringLiteral("Viewer %1").arg(index + 1) : QString::fromStdString(viewer.id)) :
        QStringLiteral("Viewer %1").arg(QString::fromStdString(viewer.sessionFingerprint).left(8));
    layout->addWidget(textLabel(name, "ActiveViewerName"), 1);
    layout->addWidget(textLabel(viewerHealthText(viewer), viewer.hasFeedback ? "ActiveGoodText" : "ActiveMuted"));
    return row;
}

QPushButton* ActiveShareWindow::iconButton(const QString& text, const QString& objectName, const char* iconName)
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

QLabel* ActiveShareWindow::textLabel(const QString& text, const char* objectName)
{
    auto* label = new QLabel(text);
    label->setObjectName(QString::fromUtf8(objectName));
    label->setWordWrap(true);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    return label;
}

QLabel* ActiveShareWindow::iconLabel(const char* iconName, int size, const QString& color)
{
    auto* label = new QLabel;
    label->setObjectName("RoomIconLabel");
    label->setFixedSize(size, size);
    label->setAlignment(Qt::AlignCenter);
    label->setPixmap(renderSvgResource(
        QStringLiteral(":/screenshare/ui/icons/%1.svg").arg(QString::fromUtf8(iconName)),
        QSize(size, size),
        color));
    return label;
}

void ActiveShareWindow::configureSettingsChoice(QComboBox* combo)
{
    if (combo == nullptr) {
        return;
    }
    combo->setObjectName("RoomSettingsInput");
    combo->setFocusPolicy(Qt::ClickFocus);
    combo->setMaximumWidth(kSettingsFieldMaxWidth);
    combo->installEventFilter(this);
}

void ActiveShareWindow::installBackendHandlers()
{
    if (backend_ == nullptr) {
        return;
    }
    backend_->setStartedHandler({});
    backend_->setErrorHandler([this](const QString& message) {
        healthStateLabel_->setText(message.isEmpty() ? QStringLiteral("Error") : message);
        healthStateLabel_->setObjectName("ActiveHealthError");
        healthStateLabel_->style()->unpolish(healthStateLabel_);
        healthStateLabel_->style()->polish(healthStateLabel_);
    });
    backend_->setFinishedHandler([this](const QtSessionBackend::FinishInfo& info) {
        handleFinished(info);
    });
    backend_->setStatusHandler([this](const screenshare::SessionEvent& event) {
        updateStatus(event.status);
    });
}

void ActiveShareWindow::updateElapsed()
{
    const qint64 seconds = elapsed_.isValid() ? elapsed_.elapsed() / 1000 : 0;
    elapsedLabel_->setText(QStringLiteral("%1:%2:%3")
        .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0')));
}

void ActiveShareWindow::updateStatus(const screenshare::SessionStatus& status)
{
    stateLabel_->setText(stateText(status));
    stateLabel_->setObjectName(
        status.state == screenshare::SessionState::Failed ? "ActiveTopError" :
        (status.state == screenshare::SessionState::Live ? "ActiveTopLive" : "ActiveTopIdle"));
    stateLabel_->style()->unpolish(stateLabel_);
    stateLabel_->style()->polish(stateLabel_);
    updateViewerList(status);
    updateHealth(status);
}

void ActiveShareWindow::updateViewerList(const screenshare::SessionStatus& status)
{
    if (viewerListLayout_ == nullptr) {
        return;
    }
    const int active = activeViewerCount(status.viewers);
    const int total = static_cast<int>(status.viewers.size());
    if (viewerMetricLabel_ != nullptr) {
        viewerMetricLabel_->setText(QStringLiteral("%1 / %2").arg(active).arg(total));
    }
    const QString signature = viewerListSignature(status);
    if (signature == lastViewerSignature_) {
        return;
    }
    lastViewerSignature_ = signature;

    while (QLayoutItem* item = viewerListLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    viewerTitleLabel_->setText(QStringLiteral("Viewers (%1 of %2)")
        .arg(active)
        .arg(total));
    if (status.viewers.empty()) {
        auto* empty = textLabel("Waiting for viewers", "ActiveMuted");
        empty->setAlignment(Qt::AlignCenter);
        viewerListLayout_->addWidget(empty, 1);
        return;
    }
    int index = 0;
    for (const auto& viewer : status.viewers) {
        viewerListLayout_->addWidget(buildViewerRow(viewer, index++));
    }
    viewerListLayout_->addStretch(1);
}

void ActiveShareWindow::updateHealth(const screenshare::SessionStatus& status)
{
    healthStateLabel_->setText(healthText(status));
    healthStateLabel_->setObjectName(
        healthText(status) == "Good" ? "ActiveHealthGood" :
        (status.state == screenshare::SessionState::Failed ? "ActiveHealthError" : "ActiveHealthWaiting"));
    healthStateLabel_->style()->unpolish(healthStateLabel_);
    healthStateLabel_->style()->polish(healthStateLabel_);

    bitrateLabel_->setText(status.stream.bitrateMbps > 0.0 ?
        QStringLiteral("%1 Mbps").arg(status.stream.bitrateMbps, 0, 'f', 1) :
        QStringLiteral("-"));
    const QString streamResolution = resolutionText(status.stream.outputResolution);
    resolutionLabel_->setText(streamResolution != "-" ? streamResolution : session_.resolutionText);
    fpsLabel_->setText(status.stream.outputFps > 0.0 ?
        QString::number(static_cast<int>(std::round(status.stream.outputFps))) :
        QString(session_.fpsText).remove(" FPS"));
    packetLossLabel_->setText(status.viewers.empty() ? QStringLiteral("-") : QStringLiteral("0%"));
    const auto maxQueue = std::max_element(status.viewers.begin(), status.viewers.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.queueDelayMs < rhs.queueDelayMs;
    });
    latencyLabel_->setText(maxQueue == status.viewers.end() ? QStringLiteral("-") : QStringLiteral("%1 ms").arg(maxQueue->queueDelayMs));
    if (adaptiveLabel_ != nullptr) {
        adaptiveLabel_->setText(QStringLiteral("Adaptive: %1").arg(
            session_.config.stream.adaptBitrate || session_.config.stream.adaptResolution ? "On" : "Off"));
    }
}

void ActiveShareWindow::updateShareSummary()
{
    roomLabel_->setText(QStringLiteral("Room: %1").arg(session_.roomName.isEmpty() ? session_.roomId : session_.roomName));
    shareDisplayLabel_->setText(QStringLiteral("%1 @ %2").arg(session_.displayText, session_.fpsText));
    shareAudioLabel_->setText(session_.audioText);
    if (settingsRoomNameEdit_ != nullptr) {
        updatingSettingsUi_ = true;
        settingsRoomNameEdit_->setText(session_.roomName);
        appliedRoomName_ = settingsRoomNameEdit_->text().trimmed();
        updatingSettingsUi_ = false;
    }
    if (settingsRoomLinkEdit_ != nullptr) {
        settingsRoomLinkEdit_->setText(session_.roomLink);
    }
    if (settingsDisplayCombo_ != nullptr) {
        updatingSettingsUi_ = true;
        const bool blocked = settingsDisplayCombo_->blockSignals(true);
        settingsDisplayCombo_->clear();
        if (session_.displayChoices.isEmpty()) {
            settingsDisplayCombo_->addItem(
                session_.displayText.isEmpty() ? QStringLiteral("Display 0") : session_.displayText,
                session_.displayValue);
        } else {
            for (const ShareDisplayChoice& choice : session_.displayChoices) {
                settingsDisplayCombo_->addItem(choice.text, choice.value);
            }
        }
        const int index = settingsDisplayCombo_->findData(session_.displayValue);
        settingsDisplayCombo_->setCurrentIndex(index >= 0 ? index : 0);
        settingsDisplayCombo_->blockSignals(blocked);
        appliedDisplayValue_ = settingsDisplayCombo_->currentData().toInt();
        updatingSettingsUi_ = false;
    }
    if (settingsResolutionCombo_ != nullptr) {
        updatingSettingsUi_ = true;
        const bool blocked = settingsResolutionCombo_->blockSignals(true);
        settingsResolutionCombo_->clear();
        if (session_.resolutionChoices.isEmpty()) {
            settingsResolutionCombo_->addItem(
                session_.resolutionText.isEmpty() ? QStringLiteral("Auto") : session_.resolutionText,
                session_.resolutionValue.isEmpty() ? QStringLiteral("auto") : session_.resolutionValue);
        } else {
            for (const ShareResolutionChoice& choice : session_.resolutionChoices) {
                settingsResolutionCombo_->addItem(choice.text, choice.value);
            }
            const int index = settingsResolutionCombo_->findData(session_.resolutionValue);
            settingsResolutionCombo_->setCurrentIndex(index >= 0 ? index : 0);
        }
        settingsResolutionCombo_->blockSignals(blocked);
        appliedResolutionValue_ = settingsResolutionCombo_->currentData().toString();
        updatingSettingsUi_ = false;
    }
    if (settingsFpsCombo_ != nullptr) {
        updatingSettingsUi_ = true;
        const bool blocked = settingsFpsCombo_->blockSignals(true);
        const int index = settingsFpsCombo_->findData(session_.fpsValue);
        if (index < 0) {
            settingsFpsCombo_->addItem(QStringLiteral("%1 FPS").arg(session_.fpsValue), session_.fpsValue);
        }
        settingsFpsCombo_->setCurrentIndex(settingsFpsCombo_->findData(session_.fpsValue));
        settingsFpsCombo_->blockSignals(blocked);
        appliedFpsValue_ = settingsFpsCombo_->currentData().toInt();
        updatingSettingsUi_ = false;
    }
    if (bitrateCombo_ != nullptr) {
        updatingSettingsUi_ = true;
        const bool bitrateBlocked = bitrateCombo_->blockSignals(true);
        uint32_t bitrate = session_.config.stream.bitrateBps;
        int bitrateIndex = bitrateCombo_->findData(QString::number(bitrate));
        if (bitrateIndex < 0) {
            bitrateCombo_->addItem(QStringLiteral("%1 Mbps").arg(static_cast<double>(bitrate) / 1'000'000.0, 0, 'f', 1),
                QString::number(bitrate));
            bitrateIndex = bitrateCombo_->count() - 1;
        }
        bitrateCombo_->setCurrentIndex(bitrateIndex);
        bitrateCombo_->blockSignals(bitrateBlocked);
        appliedBitrateBps_ = comboBitrateBps(bitrateCombo_);
        updatingSettingsUi_ = false;
    }
    if (settingsAudioCombo_ != nullptr) {
        updatingSettingsUi_ = true;
        const bool blocked = settingsAudioCombo_->blockSignals(true);
        settingsAudioCombo_->clear();
        if (session_.audioChoices.isEmpty()) {
            settingsAudioCombo_->addItem(
                session_.audioText.isEmpty() ? QStringLiteral("System Audio (default)") : session_.audioText,
                session_.audioDeviceValue);
        } else {
            for (const ShareAudioChoice& choice : session_.audioChoices) {
                settingsAudioCombo_->addItem(choice.text, choice.value);
            }
        }
        const int index = settingsAudioCombo_->findData(session_.audioDeviceValue);
        settingsAudioCombo_->setCurrentIndex(index >= 0 ? index : 0);
        settingsAudioCombo_->blockSignals(blocked);
        appliedAudioDeviceValue_ = settingsAudioCombo_->currentData().toString();
        updatingSettingsUi_ = false;
    }
    updateHostControlButtons();
    updateSettingsApplyState();
}

void ActiveShareWindow::stopSharing()
{
    if (backend_ == nullptr || !backend_->isRunning()) {
        return;
    }
    stopButton_->setEnabled(false);
    stopButton_->setText("Stopping...");
    backend_->stop();
}

void ActiveShareWindow::toggleHostAudio()
{
    hostAudioMuted_ = !hostAudioMuted_;
    session_.config.hostAudioMuted = hostAudioMuted_;
    session_.config.captureSystemAudio = !hostAudioMuted_;
    session_.captureSystemAudio = !hostAudioMuted_;
    session_.audioText = hostAudioMuted_ ?
        QStringLiteral("System Audio muted") :
        (settingsAudioCombo_ != nullptr ? settingsAudioCombo_->currentText() : QStringLiteral("System Audio (default)"));
    updateShareSummary();
    updateHostControlButtons();

    if (backend_ != nullptr && backend_->isRunning()) {
        screenshare::ShareSessionSettings settings;
        settings.captureSystemAudio = !hostAudioMuted_;
        settings.hostAudioMuted = hostAudioMuted_;
        if (!session_.audioDeviceValue.isEmpty()) {
            settings.audioDeviceId = toStdUtf8(session_.audioDeviceValue);
        }
        settings.stream = session_.config.stream;
        backend_->applyShareSettings(settings);
    }
}

void ActiveShareWindow::toggleVideoPause()
{
    videoPaused_ = !videoPaused_;
    session_.config.hostVideoPaused = videoPaused_;
    updateHostControlButtons();

    if (backend_ != nullptr && backend_->isRunning()) {
        screenshare::ShareSessionSettings settings;
        settings.hostVideoPaused = videoPaused_;
        settings.stream = session_.config.stream;
        backend_->applyShareSettings(settings);
    }
}

void ActiveShareWindow::updateHostControlButtons()
{
    const auto setButtonIcon = [](QPushButton* button, const QString& iconName) {
        if (button == nullptr) {
            return;
        }
        const QPixmap pixmap = renderSvgResource(
            QStringLiteral(":/screenshare/ui/icons/%1.svg").arg(iconName),
            QSize(18, 18),
            QStringLiteral("#ffffff"));
        if (!pixmap.isNull()) {
            button->setIcon(QIcon(pixmap));
        }
    };
    if (muteAudioButton_ != nullptr) {
        muteAudioButton_->setText(hostAudioMuted_ ? QStringLiteral("Unmute Audio") : QStringLiteral("Mute Audio"));
        setButtonIcon(muteAudioButton_, hostAudioMuted_ ? QStringLiteral("mute") : QStringLiteral("volume"));
    }
    if (pauseVideoButton_ != nullptr) {
        pauseVideoButton_->setText(videoPaused_ ? QStringLiteral("Resume Video") : QStringLiteral("Pause Video"));
        setButtonIcon(pauseVideoButton_, videoPaused_ ? QStringLiteral("play") : QStringLiteral("pause"));
    }
    if (shareTitleLabel_ != nullptr) {
        shareTitleLabel_->setText(videoPaused_ ? QStringLiteral("Video paused") : QStringLiteral("You are sharing"));
    }
}

void ActiveShareWindow::copyInvite()
{
    QGuiApplication::clipboard()->setText(session_.roomLink);
}

screenshare::ShareSessionSettings ActiveShareWindow::selectedShareSettings() const
{
    screenshare::ShareSessionSettings settings;
    const QString roomName = settingsRoomNameEdit_ == nullptr ?
        session_.roomName :
        settingsRoomNameEdit_->text().trimmed();
    if (roomName != appliedRoomName_) {
        settings.roomName = toStdUtf8(roomName);
    }
    settings.displayIndex = settingsDisplayCombo_ == nullptr ?
        session_.displayValue :
        settingsDisplayCombo_->currentData().toInt();
    settings.audioDeviceId = settingsAudioCombo_ == nullptr ?
        toStdUtf8(session_.audioDeviceValue) :
        toStdUtf8(settingsAudioCombo_->currentData().toString());
    settings.stream = session_.config.stream;
    const QString value = settingsResolutionCombo_ == nullptr ?
        session_.resolutionValue :
        settingsResolutionCombo_->currentData().toString();
    settings.stream.fps = settingsFpsCombo_ == nullptr ?
        session_.fpsValue :
        settingsFpsCombo_->currentData().toInt();
    settings.stream.bitrateBps = comboBitrateBps(bitrateCombo_);
    settings.stream.adaptBitrate = settings.stream.bitrateBps == 0;
    settings.stream.adaptFps = false;
    if (value == "auto" || value.isEmpty()) {
        settings.stream.outputResolution.reset();
        settings.stream.adaptResolution = true;
        return settings;
    }

    const auto resolution = parseResolutionValue(value);
    if (resolution) {
        settings.stream.outputResolution = *resolution;
        settings.stream.adaptResolution = false;
    }
    return settings;
}

void ActiveShareWindow::applySettings()
{
    if (backend_ == nullptr || settingsResolutionCombo_ == nullptr) {
        return;
    }

    screenshare::ShareSessionSettings settings = selectedShareSettings();
    if (settings.roomName) {
        session_.roomName = QString::fromStdString(*settings.roomName);
        session_.config.roomName = *settings.roomName;
    }
    session_.config.displayIndex = settings.displayIndex.value_or(session_.config.displayIndex);
    session_.config.audioDeviceId = settings.audioDeviceId.value_or(session_.config.audioDeviceId);
    session_.config.stream = settings.stream;
    session_.displayValue = session_.config.displayIndex;
    if (settingsDisplayCombo_ != nullptr) {
        session_.displayText = settingsDisplayCombo_->currentText();
    }
    session_.resolutionValue = settingsResolutionCombo_->currentData().toString();
    session_.resolutionText = settingsResolutionCombo_->currentText();
    session_.fpsValue = settings.stream.fps;
    session_.fpsText = QStringLiteral("%1 FPS").arg(settings.stream.fps);
    session_.audioDeviceValue = QString::fromStdString(session_.config.audioDeviceId);
    session_.audioText = hostAudioMuted_ ?
        QStringLiteral("System Audio muted") :
        (settingsAudioCombo_ != nullptr ? settingsAudioCombo_->currentText() : QStringLiteral("System Audio (default)"));
    appliedDisplayValue_ = session_.displayValue;
    appliedRoomName_ = session_.roomName;
    appliedResolutionValue_ = session_.resolutionValue;
    appliedFpsValue_ = settings.stream.fps;
    appliedBitrateBps_ = settings.stream.bitrateBps;
    appliedAudioDeviceValue_ = session_.audioDeviceValue;
    settingsApplyButton_->setEnabled(false);
    resolutionLabel_->setText(session_.resolutionText);
    updateShareSummary();
    if (adaptiveLabel_ != nullptr) {
        adaptiveLabel_->setText(QStringLiteral("Adaptive: %1").arg(
            settings.stream.adaptBitrate || settings.stream.adaptResolution || settings.stream.adaptFps ? "On" : "Off"));
    }
    if (backend_->isRunning()) {
        backend_->applyShareSettings(settings);
    }
}

void ActiveShareWindow::updateSettingsApplyState()
{
    if (settingsApplyButton_ == nullptr || updatingSettingsUi_) {
        return;
    }
    const QString resolutionValue = settingsResolutionCombo_ == nullptr ?
        appliedResolutionValue_ :
        settingsResolutionCombo_->currentData().toString();
    const int displayValue = settingsDisplayCombo_ == nullptr ?
        appliedDisplayValue_ :
        settingsDisplayCombo_->currentData().toInt();
    const int fpsValue = settingsFpsCombo_ == nullptr ?
        appliedFpsValue_ :
        settingsFpsCombo_->currentData().toInt();
    const uint32_t bitrateBps = comboBitrateBps(bitrateCombo_);
    const QString roomName = settingsRoomNameEdit_ == nullptr ?
        appliedRoomName_ :
        settingsRoomNameEdit_->text().trimmed();
    const QString audioDevice = settingsAudioCombo_ == nullptr ?
        appliedAudioDeviceValue_ :
        settingsAudioCombo_->currentData().toString();
    const bool changed =
        roomName != appliedRoomName_ ||
        displayValue != appliedDisplayValue_ ||
        resolutionValue != appliedResolutionValue_ ||
        fpsValue != appliedFpsValue_ ||
        bitrateBps != appliedBitrateBps_ ||
        audioDevice != appliedAudioDeviceValue_;
    settingsApplyButton_->setEnabled(changed);
}

void ActiveShareWindow::showSettingsPanel(bool visible)
{
    if (settingsOverlay_ != nullptr) {
        settingsOverlay_->setVisible(visible);
        if (visible) {
            settingsOverlay_->raise();
        }
    }
}

void ActiveShareWindow::handleFinished(const QtSessionBackend::FinishInfo& info)
{
    elapsedTimer_->stop();
    stopButton_->setEnabled(true);
    stopButton_->setText("Stop Sharing");
    healthStateLabel_->setText(info.failed ? "Failed" : "Stopped");
    healthStateLabel_->setObjectName(info.failed ? "ActiveHealthError" : "ActiveHealthWaiting");
    healthStateLabel_->style()->unpolish(healthStateLabel_);
    healthStateLabel_->style()->polish(healthStateLabel_);
    if (actions_.sessionStopped) {
        QTimer::singleShot(500, this, [this] {
            actions_.sessionStopped();
        });
    }
}

QString ActiveShareWindow::stateText(const screenshare::SessionStatus& status) const
{
    switch (status.state) {
    case screenshare::SessionState::Live:
        return QStringLiteral("● Sharing");
    case screenshare::SessionState::Stopping:
        return QStringLiteral("● Stopping");
    case screenshare::SessionState::Failed:
        return QStringLiteral("● Failed");
    case screenshare::SessionState::Connecting:
    case screenshare::SessionState::Disconnected:
        return QStringLiteral("● Idle");
    default:
        return backend_ != nullptr && backend_->isRunning() ? QStringLiteral("● Sharing") : QStringLiteral("● Idle");
    }
}

QString ActiveShareWindow::healthText(const screenshare::SessionStatus& status) const
{
    if (status.state == screenshare::SessionState::Live || activeViewerCount(status.viewers) > 0) {
        return QStringLiteral("Good");
    }
    if (status.state == screenshare::SessionState::Failed) {
        return QStringLiteral("Error");
    }
    return QStringLiteral("Waiting");
}
