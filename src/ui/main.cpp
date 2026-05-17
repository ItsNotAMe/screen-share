#include "transport/UdpCrypto.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QByteArray>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QIODevice>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QSize>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVector>
#include <QtGui/QClipboard>
#include <QtGui/QTextCursor>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace {

QString enginePath()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString besideUi = appDir.filePath("ScreenShare.exe");
    if (QFileInfo::exists(besideUi)) {
        return besideUi;
    }

    return QDir::current().filePath("ScreenShare.exe");
}

QString quoted(QString value)
{
    if (value.isEmpty()) {
        return "\"\"";
    }
    if (!value.contains(' ') && !value.contains('\t') && !value.contains('"')) {
        return value;
    }
    value.replace("\"", "\\\"");
    return "\"" + value + "\"";
}

QString formatCommand(const QString& program, const QStringList& arguments)
{
    QStringList parts;
    parts << quoted(QFileInfo(program).fileName());
    for (const QString& argument : arguments) {
        parts << quoted(argument);
    }
    return parts.join(' ');
}

bool looksLikeNatInvite(QString value)
{
    value = value.trimmed();
    return value.startsWith("nat_invite=") || value.startsWith("screenshare-invite-v1");
}

QString accessCodeFingerprintText(const QString& accessCode)
{
    const QByteArray bytes = accessCode.toUtf8();
    const uint64_t fingerprint = screenshare::UdpAccessCodeFingerprint(
        std::string_view(bytes.constData(), static_cast<size_t>(bytes.size())));

    return QStringLiteral("%1")
        .arg(static_cast<qulonglong>(fingerprint), 16, 16, QLatin1Char('0'))
        .toUpper();
}

QLabel* makeLabel(const QString& text, const QString& className = {})
{
    auto* label = new QLabel(text);
    label->setProperty("class", className);
    return label;
}

void repolish(QWidget* widget)
{
    if (widget == nullptr) {
        return;
    }
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

constexpr int kRowHeight = 34;
constexpr int kLabelWidth = 96;
constexpr int kReceiverRefreshMs = 15000;

enum class ReceiverSource {
    Lan,
    Tailscale,
};

struct DiscoveredReceiver {
    QString name;
    QString host;
    int port = 0;
    QString session;
    QString fingerprint;
    bool securityKnown = false;
    bool encrypted = false;
    QString accessFingerprint;
    ReceiverSource source = ReceiverSource::Lan;
};

struct AudioOutputDevice {
    QString name;
    QString id;
    bool isDefault = false;
};

QVector<DiscoveredReceiver> parseTailscaleStatusReceivers(const QString& output, int port)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(output.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }

    const QJsonObject peers = document.object().value("Peer").toObject();
    QVector<DiscoveredReceiver> receivers;
    for (auto iterator = peers.begin(); iterator != peers.end(); ++iterator) {
        const QJsonObject peer = iterator.value().toObject();
        if (peer.value("Online").isBool() && !peer.value("Online").toBool()) {
            continue;
        }

        QString host;
        const QJsonArray ips = peer.value("TailscaleIPs").toArray();
        for (const QJsonValue& value : ips) {
            const QString ip = value.toString();
            if (ip.startsWith("100.")) {
                host = ip;
                break;
            }
            if (host.isEmpty() && !ip.contains(':')) {
                host = ip;
            }
        }
        if (host.isEmpty() && !ips.isEmpty()) {
            host = ips.first().toString();
        }
        if (host.isEmpty()) {
            continue;
        }

        QString name = peer.value("HostName").toString().trimmed();
        if (name.isEmpty()) {
            name = peer.value("DNSName").toString().trimmed();
            if (name.endsWith('.')) {
                name.chop(1);
            }
        }
        if (name.isEmpty()) {
            name = "Tailscale peer";
        }

        DiscoveredReceiver receiver;
        receiver.name = name;
        receiver.host = host;
        receiver.port = port;
        receiver.source = ReceiverSource::Tailscale;
        receivers.push_back(std::move(receiver));
    }

    return receivers;
}

class NoWheelSpinBox final : public QSpinBox {
public:
    using QSpinBox::QSpinBox;

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        event->ignore();
    }
};

class NoWheelComboBox final : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        event->ignore();
    }
};

class CompactStackedWidget final : public QStackedWidget {
public:
    using QStackedWidget::QStackedWidget;

    QSize sizeHint() const override
    {
        if (const QWidget* current = currentWidget()) {
            return current->sizeHint();
        }
        return QStackedWidget::sizeHint();
    }

    QSize minimumSizeHint() const override
    {
        if (const QWidget* current = currentWidget()) {
            return current->minimumSizeHint();
        }
        return QStackedWidget::minimumSizeHint();
    }
};

void prepareInput(QWidget* input)
{
    input->setFixedHeight(kRowHeight);
    input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (qobject_cast<QLineEdit*>(input) != nullptr ||
        qobject_cast<QSpinBox*>(input) != nullptr ||
        qobject_cast<QComboBox*>(input) != nullptr) {
        input->setFocusPolicy(Qt::StrongFocus);
    }
}

QFrame* makePanel(const QString& title, QVBoxLayout** outContent)
{
    auto* frame = new QFrame;
    frame->setObjectName("Panel");
    frame->setFrameShape(QFrame::NoFrame);
    frame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);

    auto* outer = new QVBoxLayout(frame);
    outer->setContentsMargins(14, 10, 14, 12);
    outer->setSpacing(8);

    if (!title.isEmpty()) {
        outer->addWidget(makeLabel(title, "PanelTitle"));
    }

    auto* content = new QVBoxLayout;
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(8);
    outer->addLayout(content);

    if (outContent != nullptr) {
        *outContent = content;
    }
    return frame;
}

void addRow(QVBoxLayout* content, const QString& labelText, QWidget* field)
{
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(14);

    if (!labelText.isEmpty()) {
        auto* label = makeLabel(labelText, "FieldLabel");
        label->setFixedWidth(kLabelWidth);
        row->addWidget(label);
    } else {
        row->addSpacing(kLabelWidth);
    }
    row->addWidget(field, 1);
    content->addLayout(row);
}

void addFullRow(QVBoxLayout* content, QWidget* widget)
{
    widget->setFixedHeight(kRowHeight);
    content->addWidget(widget);
}

QString appStyleSheet(bool darkMode);

class MainWindow final : public QWidget {
public:
    MainWindow()
    {
        setWindowTitle("ScreenShare");
        resize(1080, 820);
        setMinimumSize(960, 780);

        process_ = new QProcess(this);
        process_->setProcessChannelMode(QProcess::MergedChannels);
        connect(process_, &QProcess::readyReadStandardOutput, this, [this] {
            appendOutput(QString::fromLocal8Bit(process_->readAllStandardOutput()));
        });
        connect(process_, &QProcess::started, this, [this] {
            appendOutput("Started " + QFileInfo(enginePath()).fileName() + "\n");
            setRunning(true);
        });
        connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            Q_UNUSED(error);
            appendOutput("Process error: " + process_->errorString() + "\n");
            setRunning(false);
        });
        connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus status) {
            if (stopRequested_) {
                appendOutput(forcedStop_ ?
                    "Process was forced closed after stop timed out\n" :
                    "Process stopped cleanly with exit code " + QString::number(code) + "\n");
            } else {
                const QString statusText = status == QProcess::NormalExit ? "finished" : "crashed";
                appendOutput("Process " + statusText + " with exit code " + QString::number(code) + "\n");
            }
            cleanupStopFile();
            stopRequested_ = false;
            forcedStop_ = false;
            setRunning(false);
        });

        discoveryProcess_ = new QProcess(this);
        discoveryProcess_->setProcessChannelMode(QProcess::MergedChannels);
        connect(discoveryProcess_, &QProcess::readyReadStandardOutput, this, [this] {
            const QString text = QString::fromLocal8Bit(discoveryProcess_->readAllStandardOutput());
            discoveryOutput_ += text;
            if (discoveryLogOutput_) {
                appendOutput(text);
            }
        });
        connect(discoveryProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            Q_UNUSED(error);
            if (discoveryLogOutput_) {
                appendOutput("LAN discovery error: " + discoveryProcess_->errorString() + "\n");
            }
            setDiscovering(false);
        });
        connect(discoveryProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus status) {
            finishDiscovery(code, status);
        });

        tailscaleProcess_ = new QProcess(this);
        tailscaleProcess_->setProcessChannelMode(QProcess::MergedChannels);
        connect(tailscaleProcess_, &QProcess::readyReadStandardOutput, this, [this] {
            tailscaleOutput_ += QString::fromLocal8Bit(tailscaleProcess_->readAllStandardOutput());
        });
        connect(tailscaleProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            Q_UNUSED(error);
            if (tailscaleLogOutput_) {
                appendOutput("Tailscale peer refresh unavailable; type a Tailscale IP manually if needed\n");
            }
            tailscaleReceivers_.clear();
            updateReceiverList(combinedReceivers());
            completeReceiverScanIfDone();
        });
        connect(tailscaleProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus status) {
            finishTailscaleDiscovery(code, status);
        });

        audioDeviceProcess_ = new QProcess(this);
        audioDeviceProcess_->setProcessChannelMode(QProcess::MergedChannels);
        connect(audioDeviceProcess_, &QProcess::readyReadStandardOutput, this, [this] {
            audioDeviceOutput_ += QString::fromLocal8Bit(audioDeviceProcess_->readAllStandardOutput());
        });
        connect(audioDeviceProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            Q_UNUSED(error);
            if (audioDeviceLogOutput_) {
                appendOutput("Audio device refresh error: " + audioDeviceProcess_->errorString() + "\n");
            }
            setAudioDeviceRefreshing(false);
        });
        connect(audioDeviceProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus status) {
            finishAudioDeviceRefresh(code, status);
        });

        receiverRefreshTimer_ = new QTimer(this);
        receiverRefreshTimer_->setInterval(kReceiverRefreshMs);
        connect(receiverRefreshTimer_, &QTimer::timeout, this, [this] { startDiscovery(true); });

        buildUi();
        refreshReportPath();
        refreshCommand();
        setRunning(false);
        receiverRefreshTimer_->start();
        QTimer::singleShot(400, this, [this] { startDiscovery(true); });
        QTimer::singleShot(700, this, [this] { refreshAudioDevices(true); });
    }

private:
    void buildUi()
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(22, 20, 22, 20);
        root->setSpacing(14);

        root->addLayout(buildHeader());
        root->addWidget(buildModeSelector());

        auto* body = new QHBoxLayout;
        body->setSpacing(16);
        root->addLayout(body, 1);

        auto* leftContent = new QWidget;
        leftContent->setObjectName("LeftHost");
        auto* leftColumn = new QVBoxLayout(leftContent);
        leftColumn->setContentsMargins(0, 0, 6, 0);
        leftColumn->setSpacing(8);
        leftColumn->addWidget(buildOptionStack());
        leftColumn->addWidget(buildSecurityPanel());
        leftColumn->addStretch(1);

        auto* leftScroll = new QScrollArea;
        leftScroll->setObjectName("LeftScroll");
        leftScroll->setWidget(leftContent);
        leftScroll->setWidgetResizable(true);
        leftScroll->setFrameShape(QFrame::NoFrame);
        leftScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        leftScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        leftScroll->setFixedWidth(450);
        body->addWidget(leftScroll);

        auto* rightColumn = new QVBoxLayout;
        rightColumn->setSpacing(12);
        rightColumn->addWidget(buildCommandPanel());
        rightColumn->addWidget(buildOutputPanel(), 1);
        body->addLayout(rightColumn, 1);

        bindCommandRefresh();
        applyTheme(darkModeCheck_->isChecked());
    }

    QHBoxLayout* buildHeader()
    {
        auto* header = new QHBoxLayout;
        header->setSpacing(12);

        auto* titleBlock = new QVBoxLayout;
        titleBlock->setSpacing(2);
        titleBlock->addWidget(makeLabel("ScreenShare", "HeroTitle"));
        titleBlock->addWidget(makeLabel("Fast local screen sharing", "Subtle"));
        header->addLayout(titleBlock, 1);

        statusBadge_ = makeLabel("Idle", "StatusIdle");
        statusBadge_->setAlignment(Qt::AlignCenter);
        statusBadge_->setMinimumHeight(34);
        header->addWidget(statusBadge_, 0, Qt::AlignVCenter);

        darkModeCheck_ = new QCheckBox("Dark");
        darkModeCheck_->setObjectName("ThemeSwitch");
        darkModeCheck_->setChecked(true);
        connect(darkModeCheck_, &QCheckBox::toggled, this, [this](bool checked) { applyTheme(checked); });
        header->addWidget(darkModeCheck_, 0, Qt::AlignVCenter);

        actionButton_ = new QPushButton("Start");
        actionButton_->setObjectName("PrimaryButton");
        actionButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        actionButton_->setIconSize(QSize(16, 16));
        actionButton_->setCursor(Qt::PointingHandCursor);
        actionButton_->setMinimumHeight(40);
        actionButton_->setMinimumWidth(140);
        connect(actionButton_, &QPushButton::clicked, this, [this] { toggleProcess(); });
        header->addWidget(actionButton_, 0, Qt::AlignVCenter);

        return header;
    }

    QWidget* buildModeSelector()
    {
        auto* bar = new QFrame;
        bar->setObjectName("ModeBar");
        bar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

        auto* layout = new QHBoxLayout(bar);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(6);

        shareModeButton_ = new QPushButton("Share");
        watchModeButton_ = new QPushButton("Watch");
        shareModeButton_->setCheckable(true);
        watchModeButton_->setCheckable(true);
        shareModeButton_->setObjectName("ModeButton");
        watchModeButton_->setObjectName("ModeButton");
        shareModeButton_->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
        watchModeButton_->setIcon(style()->standardIcon(QStyle::SP_DesktopIcon));
        shareModeButton_->setIconSize(QSize(16, 16));
        watchModeButton_->setIconSize(QSize(16, 16));
        shareModeButton_->setCursor(Qt::PointingHandCursor);
        watchModeButton_->setCursor(Qt::PointingHandCursor);
        shareModeButton_->setChecked(true);

        layout->addWidget(shareModeButton_);
        layout->addWidget(watchModeButton_);
        layout->addStretch(1);

        connect(shareModeButton_, &QPushButton::clicked, this, [this] { setMode(0); });
        connect(watchModeButton_, &QPushButton::clicked, this, [this] { setMode(1); });
        return bar;
    }

    QWidget* buildOptionStack()
    {
        optionStack_ = new CompactStackedWidget;
        optionStack_->setObjectName("OptionStack");
        optionStack_->addWidget(buildSharePage());
        optionStack_->addWidget(buildWatchPage());
        connect(optionStack_, &QStackedWidget::currentChanged, this, [this](int) {
            optionStack_->updateGeometry();
        });
        return optionStack_;
    }

    QWidget* buildSharePage()
    {
        auto* page = new QWidget;
        page->setObjectName("OptionPage");
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);

        QVBoxLayout* receiversContent = nullptr;
        layout->addWidget(makePanel("Targets", &receiversContent));
        receiverList_ = new QListWidget;
        receiverList_->setObjectName("ReceiverList");
        receiverList_->setMinimumHeight(120);
        receiverList_->setUniformItemSizes(true);
        receiverList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        receiversContent->addWidget(receiverList_);

        auto* receiversFooter = new QWidget;
        receiversFooter->setObjectName("FormRow");
        auto* receiversFooterLayout = new QHBoxLayout(receiversFooter);
        receiversFooterLayout->setContentsMargins(0, 0, 0, 0);
        receiversFooterLayout->setSpacing(8);
        receiverStatusLabel_ = makeLabel("Scanning", "Subtle");
        receiversFooterLayout->addWidget(receiverStatusLabel_, 1);
        findLanButton_ = new QPushButton("Refresh");
        findLanButton_->setObjectName("SecondaryButton");
        findLanButton_->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
        findLanButton_->setIconSize(QSize(14, 14));
        findLanButton_->setCursor(Qt::PointingHandCursor);
        findLanButton_->setFixedHeight(kRowHeight);
        receiversFooterLayout->addWidget(findLanButton_);
        receiversContent->addWidget(receiversFooter);
        connect(findLanButton_, &QPushButton::clicked, this, [this] { startDiscovery(false); });
        connect(receiverList_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
            selectReceiverItem(item, true);
        });

        QVBoxLayout* connectionContent = nullptr;
        layout->addWidget(makePanel("Connection", &connectionContent));
        shareHostEdit_ = new QLineEdit("127.0.0.1");
        shareHostEdit_->setPlaceholderText("Receiver IP or Tailscale IP");
        sharePortSpin_ = new NoWheelSpinBox;
        sharePortSpin_->setRange(1, 65535);
        sharePortSpin_->setValue(5000);
        prepareInput(shareHostEdit_);
        prepareInput(sharePortSpin_);
        addRow(connectionContent, "Address", shareHostEdit_);
        addRow(connectionContent, "Port", sharePortSpin_);

        QVBoxLayout* internetContent = nullptr;
        layout->addWidget(makePanel("Internet", &internetContent));
        sharePeerInviteEdit_ = new QLineEdit;
        sharePeerInviteEdit_->setPlaceholderText("Receiver invite");
        shareLocalInviteEdit_ = new QLineEdit;
        shareLocalInviteEdit_->setPlaceholderText("Your invite");
        prepareInput(sharePeerInviteEdit_);
        prepareInput(shareLocalInviteEdit_);
        addRow(internetContent, "Peer invite", sharePeerInviteEdit_);
        addRow(internetContent, "Local invite", shareLocalInviteEdit_);
        internetContent->addWidget(makeLabel("Use invites for direct UDP hole punching across the internet.", "Subtle"));

        QVBoxLayout* videoContent = nullptr;
        layout->addWidget(makePanel("Video", &videoContent));
        displaySpin_ = new NoWheelSpinBox;
        displaySpin_->setRange(0, 16);
        displaySpin_->setValue(0);
        fpsSpin_ = new NoWheelSpinBox;
        fpsSpin_->setRange(15, 240);
        fpsSpin_->setValue(60);
        resolutionCombo_ = new NoWheelComboBox;
        resolutionCombo_->addItem("Native", QSize(0, 0));
        resolutionCombo_->addItem("1920 × 1080", QSize(1920, 1080));
        resolutionCombo_->addItem("1600 × 900", QSize(1600, 900));
        resolutionCombo_->addItem("1280 × 720", QSize(1280, 720));
        prepareInput(displaySpin_);
        prepareInput(fpsSpin_);
        prepareInput(resolutionCombo_);
        addRow(videoContent, "Display", displaySpin_);
        addRow(videoContent, "FPS", fpsSpin_);
        addRow(videoContent, "Resolution", resolutionCombo_);

        QVBoxLayout* audioContent = nullptr;
        layout->addWidget(makePanel("Audio", &audioContent));
        auto* audioDeviceRow = new QWidget;
        audioDeviceRow->setObjectName("FormRow");
        prepareInput(audioDeviceRow);
        auto* audioDeviceLayout = new QHBoxLayout(audioDeviceRow);
        audioDeviceLayout->setContentsMargins(0, 0, 0, 0);
        audioDeviceLayout->setSpacing(8);
        audioDeviceCombo_ = new NoWheelComboBox;
        audioDeviceCombo_->addItem("Default output", QString());
        prepareInput(audioDeviceCombo_);
        refreshAudioDevicesButton_ = new QPushButton("Refresh");
        refreshAudioDevicesButton_->setObjectName("SecondaryButton");
        refreshAudioDevicesButton_->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
        refreshAudioDevicesButton_->setIconSize(QSize(14, 14));
        refreshAudioDevicesButton_->setCursor(Qt::PointingHandCursor);
        refreshAudioDevicesButton_->setFixedHeight(kRowHeight);
        audioDeviceLayout->addWidget(audioDeviceCombo_, 1);
        audioDeviceLayout->addWidget(refreshAudioDevicesButton_);
        addRow(audioContent, "Output", audioDeviceRow);
        audioDeviceStatusLabel_ = makeLabel("Default output", "Subtle");
        audioContent->addWidget(audioDeviceStatusLabel_);
        connect(refreshAudioDevicesButton_, &QPushButton::clicked, this, [this] { refreshAudioDevices(false); });

        layout->addStretch(1);
        return page;
    }

    QWidget* buildWatchPage()
    {
        auto* page = new QWidget;
        page->setObjectName("OptionPage");
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);

        QVBoxLayout* listenContent = nullptr;
        layout->addWidget(makePanel("Listen", &listenContent));
        watchPortSpin_ = new NoWheelSpinBox;
        watchPortSpin_->setRange(1, 65535);
        watchPortSpin_->setValue(5000);
        prepareInput(watchPortSpin_);
        addRow(listenContent, "Port", watchPortSpin_);
        lanDiscoverableCheck_ = new QCheckBox("LAN discoverable");
        lanDiscoverableCheck_->setChecked(true);
        addFullRow(listenContent, lanDiscoverableCheck_);

        QVBoxLayout* internetContent = nullptr;
        layout->addWidget(makePanel("Internet", &internetContent));
        watchPeerInviteEdit_ = new QLineEdit;
        watchPeerInviteEdit_->setPlaceholderText("Sender invite");
        prepareInput(watchPeerInviteEdit_);
        addRow(internetContent, "Peer invite", watchPeerInviteEdit_);
        internetContent->addWidget(makeLabel("Paste the sharer's invite to send punch probes while waiting.", "Subtle"));

        QVBoxLayout* audioContent = nullptr;
        layout->addWidget(makePanel("Audio", &audioContent));
        mutedCheck_ = new QCheckBox("Mute playback");
        volumeSpin_ = new NoWheelSpinBox;
        volumeSpin_->setRange(0, 200);
        volumeSpin_->setSuffix("%");
        volumeSpin_->setValue(100);
        prepareInput(volumeSpin_);
        addFullRow(audioContent, mutedCheck_);
        addRow(audioContent, "Volume", volumeSpin_);

        QVBoxLayout* timingContent = nullptr;
        layout->addWidget(makePanel("Timing", &timingContent));
        previewLatencySpin_ = new NoWheelSpinBox;
        previewLatencySpin_->setRange(0, 2000);
        previewLatencySpin_->setSuffix(" ms");
        previewLatencySpin_->setValue(100);
        prepareInput(previewLatencySpin_);
        addRow(timingContent, "Preview latency", previewLatencySpin_);

        layout->addStretch(1);
        return page;
    }

    QWidget* buildSecurityPanel()
    {
        QVBoxLayout* content = nullptr;
        auto* panel = makePanel("Security", &content);

        auto* accessRow = new QWidget;
        accessRow->setObjectName("FormRow");
        prepareInput(accessRow);
        auto* accessLayout = new QHBoxLayout(accessRow);
        accessLayout->setContentsMargins(0, 0, 0, 0);
        accessLayout->setSpacing(8);
        accessCodeEdit_ = new QLineEdit;
        accessCodeEdit_->setPlaceholderText("Generate or paste");
        accessCodeEdit_->setEchoMode(QLineEdit::Password);
        prepareInput(accessCodeEdit_);
        generateAccessCodeButton_ = new QPushButton("Generate");
        generateAccessCodeButton_->setObjectName("SecondaryButton");
        generateAccessCodeButton_->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
        generateAccessCodeButton_->setIconSize(QSize(14, 14));
        generateAccessCodeButton_->setCursor(Qt::PointingHandCursor);
        generateAccessCodeButton_->setFixedHeight(kRowHeight);
        copyAccessCodeButton_ = new QPushButton("Copy");
        copyAccessCodeButton_->setObjectName("SecondaryButton");
        copyAccessCodeButton_->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
        copyAccessCodeButton_->setIconSize(QSize(14, 14));
        copyAccessCodeButton_->setCursor(Qt::PointingHandCursor);
        copyAccessCodeButton_->setFixedHeight(kRowHeight);
        accessLayout->addWidget(accessCodeEdit_, 1);
        accessLayout->addWidget(generateAccessCodeButton_);
        accessLayout->addWidget(copyAccessCodeButton_);
        addRow(content, "Access code", accessRow);

        allowPlaintextCheck_ = new QCheckBox("Allow plaintext");
        addFullRow(content, allowPlaintextCheck_);

        reportCheck_ = new QCheckBox("Save report");
        reportCheck_->setChecked(true);
        addFullRow(content, reportCheck_);

        auto* reportRow = new QWidget;
        reportRow->setObjectName("FormRow");
        auto* reportLayout = new QHBoxLayout(reportRow);
        reportLayout->setContentsMargins(0, 0, 0, 0);
        reportLayout->setSpacing(8);
        reportPathEdit_ = new QLineEdit;
        browseReportButton_ = new QPushButton("Browse");
        browseReportButton_->setObjectName("SecondaryButton");
        browseReportButton_->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
        browseReportButton_->setIconSize(QSize(14, 14));
        browseReportButton_->setCursor(Qt::PointingHandCursor);
        prepareInput(reportPathEdit_);
        browseReportButton_->setFixedHeight(kRowHeight);
        reportLayout->addWidget(reportPathEdit_, 1);
        reportLayout->addWidget(browseReportButton_);
        addRow(content, "File", reportRow);

        connect(reportPathEdit_, &QLineEdit::textEdited, this, [this] {
            reportPathEdited_ = true;
            refreshCommand();
        });
        connect(accessCodeEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
            if (!text.isEmpty()) {
                allowPlaintextCheck_->setChecked(false);
            }
        });
        connect(generateAccessCodeButton_, &QPushButton::clicked, this, [this] { generateAccessCode(); });
        connect(copyAccessCodeButton_, &QPushButton::clicked, this, [this] { copyAccessCode(); });
        connect(browseReportButton_, &QPushButton::clicked, this, [this] {
            const QString selected = QFileDialog::getSaveFileName(
                this,
                "Save report",
                reportPathEdit_->text(),
                "Zip reports (*.zip)");
            if (!selected.isEmpty()) {
                reportPathEdited_ = true;
                reportPathEdit_->setText(selected);
                refreshCommand();
            }
        });
        return panel;
    }

    QWidget* buildCommandPanel()
    {
        auto* panel = new QFrame;
        panel->setObjectName("Panel");
        panel->setFrameShape(QFrame::NoFrame);
        panel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

        auto* layout = new QVBoxLayout(panel);
        layout->setContentsMargins(18, 14, 18, 16);
        layout->setSpacing(10);
        layout->addWidget(makeLabel("Command", "PanelTitle"));
        commandPreview_ = makeLabel("", "CommandPreview");
        commandPreview_->setWordWrap(true);
        commandPreview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(commandPreview_);
        return panel;
    }

    QWidget* buildOutputPanel()
    {
        auto* panel = new QFrame;
        panel->setObjectName("Panel");
        panel->setFrameShape(QFrame::NoFrame);

        auto* layout = new QVBoxLayout(panel);
        layout->setContentsMargins(18, 14, 18, 16);
        layout->setSpacing(10);

        auto* header = new QHBoxLayout;
        header->addWidget(makeLabel("Output", "PanelTitle"));
        header->addStretch(1);
        auto* clearButton = new QPushButton("Clear");
        clearButton->setObjectName("GhostButton");
        clearButton->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
        clearButton->setIconSize(QSize(14, 14));
        clearButton->setCursor(Qt::PointingHandCursor);
        header->addWidget(clearButton);
        layout->addLayout(header);

        outputEdit_ = new QPlainTextEdit;
        outputEdit_->setReadOnly(true);
        outputEdit_->setLineWrapMode(QPlainTextEdit::NoWrap);
        layout->addWidget(outputEdit_, 1);
        connect(clearButton, &QPushButton::clicked, outputEdit_, &QPlainTextEdit::clear);
        return panel;
    }

    void setMode(int index)
    {
        optionStack_->setCurrentIndex(index);
        shareModeButton_->setChecked(index == 0);
        watchModeButton_->setChecked(index == 1);
        refreshReportPath();
        refreshCommand();
    }

    void bindCommandRefresh()
    {
        const auto bindLineEdit = [this](QLineEdit* edit) {
            connect(edit, &QLineEdit::textChanged, this, [this] { refreshCommand(); });
        };
        const auto bindSpinBox = [this](QSpinBox* spin) {
            connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this] { refreshCommand(); });
        };
        const auto bindCheckBox = [this](QCheckBox* check) {
            connect(check, &QCheckBox::toggled, this, [this] { refreshCommand(); });
        };

        bindLineEdit(shareHostEdit_);
        bindLineEdit(sharePeerInviteEdit_);
        bindLineEdit(shareLocalInviteEdit_);
        bindSpinBox(sharePortSpin_);
        connect(sharePortSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int port) {
            updateTailscaleReceiverPorts(port);
        });
        bindCheckBox(lanDiscoverableCheck_);
        bindSpinBox(displaySpin_);
        bindSpinBox(fpsSpin_);
        connect(resolutionCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { refreshCommand(); });
        connect(audioDeviceCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { refreshCommand(); });
        bindSpinBox(watchPortSpin_);
        bindLineEdit(watchPeerInviteEdit_);
        bindCheckBox(mutedCheck_);
        bindSpinBox(volumeSpin_);
        bindSpinBox(previewLatencySpin_);
        bindLineEdit(accessCodeEdit_);
        bindCheckBox(allowPlaintextCheck_);
        bindCheckBox(reportCheck_);
    }

    bool shareMode() const
    {
        return optionStack_->currentIndex() == 0;
    }

    QString defaultReportName() const
    {
        return shareMode() ? "sender-report.zip" : "receiver-report.zip";
    }

    void refreshReportPath()
    {
        if (!reportPathEdited_) {
            reportPathEdit_->setText(defaultReportName());
        }
    }

    void generateAccessCode()
    {
        try {
            accessCodeEdit_->setText(QString::fromStdString(screenshare::GenerateUdpAccessCode()));
            allowPlaintextCheck_->setChecked(false);
            copyAccessCode();
        } catch (const std::exception& error) {
            QMessageBox::critical(this, "Access code", QString::fromLocal8Bit(error.what()));
        }
    }

    void copyAccessCode()
    {
        const QString accessCode = accessCodeEdit_->text();
        if (accessCode.isEmpty()) {
            return;
        }
        QApplication::clipboard()->setText(accessCode);
        appendOutput("Access code copied to clipboard\n");
    }

    QStringList currentArguments() const
    {
        QStringList args;
        if (shareMode()) {
            const QString peerInvite = sharePeerInviteEdit_->text().trimmed();
            if (!peerInvite.isEmpty()) {
                args << "--share" << peerInvite;
                const QString localInvite = shareLocalInviteEdit_->text().trimmed();
                if (!localInvite.isEmpty()) {
                    args << "--local-invite" << localInvite;
                }
            } else {
                args << "--share" << (shareHostEdit_->text().trimmed() + ":" + QString::number(sharePortSpin_->value()));
            }
            args << "--display" << QString::number(displaySpin_->value());
            args << "--fps" << QString::number(fpsSpin_->value());

            const QSize resolution = resolutionCombo_->currentData().toSize();
            if (resolution.width() > 0 && resolution.height() > 0) {
                args << "--width" << QString::number(resolution.width());
                args << "--height" << QString::number(resolution.height());
            }

            const QString audioDeviceId = audioDeviceCombo_->currentData().toString();
            if (!audioDeviceId.isEmpty()) {
                args << "--audio-device-id" << audioDeviceId;
            }
        } else {
            args << "--watch" << QString::number(watchPortSpin_->value());
            if (lanDiscoverableCheck_->isChecked()) {
                args << "--lan-advertise";
            }
            const QString peerInvite = watchPeerInviteEdit_->text().trimmed();
            if (!peerInvite.isEmpty()) {
                args << "--peer-invite" << peerInvite;
            }
            args << "--preview-latency-ms" << QString::number(previewLatencySpin_->value());
            args << "--audio-playback-volume" << QString::number(volumeSpin_->value());
            if (mutedCheck_->isChecked()) {
                args << "--audio-playback-muted";
            }
        }

        const QString accessCode = accessCodeEdit_->text();
        if (!accessCode.isEmpty()) {
            args << "--access-code" << accessCode;
        } else if (allowPlaintextCheck_->isChecked()) {
            args << "--allow-plaintext";
        }

        if (reportCheck_->isChecked() && !reportPathEdit_->text().trimmed().isEmpty()) {
            args << "--save-report" << reportPathEdit_->text().trimmed();
        }

        return args;
    }

    QStringList displayArguments() const
    {
        QStringList args = currentArguments();
        for (int index = 0; index + 1 < args.size(); ++index) {
            if (args[index] == "--access-code") {
                args[index + 1] = "<redacted>";
                ++index;
            } else if (args[index] == "--audio-device-id") {
                args[index + 1] = "<selected audio device>";
                ++index;
            } else if (args[index] == "--peer-invite") {
                args[index + 1] = "<peer invite>";
                ++index;
            } else if (args[index] == "--local-invite") {
                args[index + 1] = "<local invite>";
                ++index;
            } else if (args[index] == "--share" && looksLikeNatInvite(args[index + 1])) {
                args[index + 1] = "<peer invite>";
                ++index;
            }
        }
        return args;
    }

    void refreshCommand()
    {
        if (commandPreview_ == nullptr) {
            return;
        }
        commandPreview_->setText(formatCommand(enginePath(), displayArguments()));
    }

    void applyTheme(bool darkMode)
    {
        qApp->setStyleSheet(appStyleSheet(darkMode));
        repolish(this);
    }

    void toggleProcess()
    {
        if (process_->state() == QProcess::NotRunning) {
            startProcess();
        } else {
            stopProcess();
        }
    }

    void startProcess()
    {
        if (process_->state() != QProcess::NotRunning) {
            return;
        }
        const bool shareNatInvite = shareMode() && !sharePeerInviteEdit_->text().trimmed().isEmpty();
        if (shareMode() && !shareNatInvite && shareHostEdit_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, "Missing address", "Enter a target address before sharing.");
            return;
        }
        if (shareMode() && !shareNatInvite && !shareLocalInviteEdit_->text().trimmed().isEmpty()) {
            QMessageBox::warning(
                this,
                "Missing peer invite",
                "Paste the receiver invite before using a local invite.");
            sharePeerInviteEdit_->setFocus();
            return;
        }
        if (!confirmShareTarget()) {
            return;
        }
        if (!validateSelectedReceiverSecurity()) {
            return;
        }
        if (discoveryProcess_->state() != QProcess::NotRunning) {
            discoveryProcess_->kill();
            discoveryProcess_->waitForFinished(500);
        }
        if (tailscaleProcess_->state() != QProcess::NotRunning) {
            tailscaleProcess_->kill();
            tailscaleProcess_->waitForFinished(500);
        }
        setDiscovering(false);
        if (accessCodeEdit_->text().isEmpty() && !allowPlaintextCheck_->isChecked()) {
            QMessageBox::warning(
                this,
                "Security choice",
                "Generate or paste an access code, or allow plaintext for this run.");
            return;
        }
        const QString program = enginePath();
        if (!QFileInfo::exists(program)) {
            QMessageBox::critical(this, "Missing engine", "ScreenShare.exe was not found beside the UI executable.");
            return;
        }

        stopRequested_ = false;
        forcedStop_ = false;
        cleanupStopFile();
        stopFilePath_ = QDir::temp().filePath(
            "ScreenShare-stop-" +
            QString::number(QCoreApplication::applicationPid()) +
            "-" +
            QString::number(++runSerial_) +
            ".signal");
        QFile::remove(stopFilePath_);

        const QStringList displayArgs = displayArguments();
        QStringList args = currentArguments();
        args << "--stop-file" << stopFilePath_;
        appendOutput("\n" + formatCommand(program, displayArgs) + "\n");
        process_->setProgram(program);
        process_->setArguments(args);
        process_->setWorkingDirectory(QFileInfo(program).absolutePath());
        process_->start();
    }

    void stopProcess()
    {
        if (process_->state() == QProcess::NotRunning) {
            return;
        }
        appendOutput("Stopping...\n");
        stopRequested_ = true;

        QFile stopFile(stopFilePath_);
        if (stopFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            stopFile.write("stop\n");
            stopFile.close();
        } else {
            appendOutput("Could not write graceful stop signal; forcing close if needed\n");
        }

        if (!process_->waitForFinished(7000)) {
            appendOutput("Graceful stop timed out; forcing process closed...\n");
            forcedStop_ = true;
            process_->kill();
            process_->waitForFinished(1000);
        }
    }

    void cleanupStopFile()
    {
        if (!stopFilePath_.isEmpty()) {
            QFile::remove(stopFilePath_);
            stopFilePath_.clear();
        }
    }

    QVector<DiscoveredReceiver> parseDiscoveredReceivers(const QString& output) const
    {
        QVector<DiscoveredReceiver> receivers;
        const QRegularExpression receiverPattern(QStringLiteral(
            R"room(receiver name="([^"]*)" address=([^\s]+) port=(\d+) session=([^\s]+) fingerprint=([^\s]+) security=(encrypted|plaintext) access_fingerprint=([0-9A-Fa-f]{16}|none))room"));

        auto matches = receiverPattern.globalMatch(output);
        while (matches.hasNext()) {
            const auto match = matches.next();
            bool portOk = false;
            const int port = match.captured(3).toInt(&portOk);
            if (!portOk || port < sharePortSpin_->minimum() || port > sharePortSpin_->maximum()) {
                continue;
            }

            DiscoveredReceiver receiver;
            receiver.name = match.captured(1);
            receiver.host = match.captured(2);
            receiver.port = port;
            receiver.session = match.captured(4) == "unknown" ? QString() : match.captured(4);
            receiver.fingerprint = match.captured(5).toUpper();
            receiver.securityKnown = true;
            receiver.encrypted = match.captured(6) == "encrypted";
            receiver.accessFingerprint = match.captured(7).toUpper();
            receivers.push_back(std::move(receiver));
        }

        return receivers;
    }

    QVector<AudioOutputDevice> parseAudioOutputDevices(const QString& output) const
    {
        const QString header = "system audio devices";
        const int start = output.indexOf(header, 0, Qt::CaseInsensitive);
        if (start < 0) {
            return {};
        }

        int end = output.indexOf("\nmicrophone audio devices", start, Qt::CaseInsensitive);
        if (end < 0) {
            end = output.size();
        }
        const QString section = output.mid(start, end - start);

        QVector<AudioOutputDevice> devices;
        const QRegularExpression devicePattern(QStringLiteral(
            R"device(\[\d+\]\s+(default\s+)?"([^"]*)"\s*\r?\n\s*id=([^\r\n]+))device"));
        auto matches = devicePattern.globalMatch(section);
        while (matches.hasNext()) {
            const auto match = matches.next();
            AudioOutputDevice device;
            device.isDefault = !match.captured(1).isEmpty();
            device.name = match.captured(2).trimmed();
            device.id = match.captured(3).trimmed();
            if (!device.id.isEmpty()) {
                devices.push_back(std::move(device));
            }
        }
        return devices;
    }

    QVector<DiscoveredReceiver> combinedReceivers() const
    {
        QVector<DiscoveredReceiver> receivers = lanReceivers_;
        receivers += tailscaleReceivers_;
        return receivers;
    }

    QString receiverDisplayText(const DiscoveredReceiver& receiver) const
    {
        const QString name = receiver.name.trimmed().isEmpty() ? QStringLiteral("ScreenShare") : receiver.name.trimmed();
        if (receiver.source == ReceiverSource::Tailscale) {
            return QStringLiteral("%1  %2:%3  Tailscale peer")
                .arg(name, receiver.host)
                .arg(receiver.port);
        }
        const QString security = receiver.securityKnown ? (receiver.encrypted ? QStringLiteral("Encrypted") : QStringLiteral("Plaintext")) : QStringLiteral("Unknown");
        return QStringLiteral("%1  %2:%3  %4")
            .arg(name, receiver.host)
            .arg(receiver.port)
            .arg(security);
    }

    bool isLoopbackHost(const QString& host) const
    {
        const QString normalized = host.trimmed().toLower();
        return normalized == "localhost" ||
               normalized == "::1" ||
               normalized == "[::1]" ||
               normalized == "127.0.0.1" ||
               normalized.startsWith("127.");
    }

    bool confirmShareTarget()
    {
        if (shareMode() && sharePeerInviteEdit_ != nullptr && !sharePeerInviteEdit_->text().trimmed().isEmpty()) {
            return true;
        }
        if (!shareMode() || !isLoopbackHost(shareHostEdit_->text())) {
            return true;
        }

        const auto result = QMessageBox::question(
            this,
            "Share to this computer?",
            "The target address is localhost, so Share will send to this same computer. "
            "Use the remote computer's LAN or Tailscale IP if you want a friend to watch.\n\n"
            "Continue with localhost?",
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (result == QMessageBox::Yes) {
            return true;
        }

        shareHostEdit_->setFocus();
        return false;
    }

    bool sameDiscoveredReceiver(const DiscoveredReceiver& lhs, const DiscoveredReceiver& rhs) const
    {
        if (lhs.host == rhs.host && lhs.port == rhs.port) {
            return true;
        }
        if (lhs.port != rhs.port || lhs.accessFingerprint != rhs.accessFingerprint) {
            return false;
        }
        if (!lhs.fingerprint.isEmpty() &&
            lhs.fingerprint != "UNKNOWN" &&
            lhs.fingerprint == rhs.fingerprint) {
            return true;
        }
        return !lhs.session.isEmpty() &&
               lhs.session == rhs.session &&
               lhs.name == rhs.name;
    }

    QVector<DiscoveredReceiver> deduplicateReceivers(QVector<DiscoveredReceiver> receivers) const
    {
        QVector<DiscoveredReceiver> uniqueReceivers;
        for (const auto& receiver : receivers) {
            bool merged = false;
            for (auto& existing : uniqueReceivers) {
                if (!sameDiscoveredReceiver(existing, receiver)) {
                    continue;
                }
                if (isLoopbackHost(existing.host) && !isLoopbackHost(receiver.host)) {
                    existing = receiver;
                }
                merged = true;
                break;
            }
            if (!merged) {
                uniqueReceivers.push_back(receiver);
            }
        }
        return uniqueReceivers;
    }

    void updateReceiverList(const QVector<DiscoveredReceiver>& receivers)
    {
        discoveredReceivers_ = deduplicateReceivers(receivers);
        if (receiverList_ == nullptr) {
            return;
        }

        const QString selectedHost = selectedReceiverHost_;
        const int selectedPort = selectedReceiverPort_;

        receiverList_->clear();
        int selectedRow = -1;
        for (int index = 0; index < discoveredReceivers_.size(); ++index) {
            const auto& receiver = discoveredReceivers_[index];
            auto* item = new QListWidgetItem(receiverDisplayText(receiver));
            item->setData(Qt::UserRole, index);
            item->setToolTip(receiver.source == ReceiverSource::Tailscale ?
                QStringLiteral("Tailscale peer only. Watch may not be running on that computer.\n%1:%2")
                    .arg(receiver.host)
                    .arg(receiver.port) :
                QStringLiteral("%1:%2").arg(receiver.host).arg(receiver.port));
            receiverList_->addItem(item);
            if (receiver.host == selectedHost && receiver.port == selectedPort) {
                selectedRow = index;
            }
        }

        if (selectedRow >= 0) {
            receiverList_->setCurrentRow(selectedRow);
        }
        updateReceiverStatus(false);
    }

    void updateReceiverStatus(bool scanning)
    {
        if (receiverStatusLabel_ == nullptr) {
            return;
        }
        if (scanning) {
            receiverStatusLabel_->setText("Scanning");
            return;
        }
        const int count = discoveredReceivers_.size();
        if (count == 0) {
            receiverStatusLabel_->setText("No targets");
        } else if (count == 1) {
            receiverStatusLabel_->setText("1 target");
        } else {
            receiverStatusLabel_->setText(QString::number(count) + " targets");
        }
    }

    void selectReceiverItem(QListWidgetItem* item, bool promptForAccessCode)
    {
        if (item == nullptr) {
            return;
        }
        bool ok = false;
        const int index = item->data(Qt::UserRole).toInt(&ok);
        if (!ok || index < 0 || index >= discoveredReceivers_.size()) {
            return;
        }
        selectDiscoveredReceiver(discoveredReceivers_[index], promptForAccessCode);
    }

    void selectDiscoveredReceiver(const DiscoveredReceiver& receiver, bool promptForAccessCode)
    {
        shareHostEdit_->setText(receiver.host);
        sharePortSpin_->setValue(receiver.port);

        selectedReceiverKnown_ = true;
        selectedReceiverHost_ = receiver.host;
        selectedReceiverPort_ = receiver.port;
        selectedReceiverSecurityKnown_ = receiver.securityKnown;
        selectedReceiverEncrypted_ = receiver.encrypted;
        selectedReceiverAccessFingerprint_ = receiver.accessFingerprint;

        const QString receiverName = receiver.name.trimmed().isEmpty() ? QStringLiteral("ScreenShare") : receiver.name.trimmed();
        if (receiver.securityKnown && !receiver.encrypted) {
            if (accessCodeEdit_->text().isEmpty()) {
                allowPlaintextCheck_->setChecked(true);
                appendOutput("Selected plaintext receiver " + receiverName + "\n");
            } else {
                appendOutput("Selected receiver is plaintext; clear the access code or start Watch with the same access code\n");
            }
        } else if (receiver.securityKnown && receiver.encrypted) {
            allowPlaintextCheck_->setChecked(false);

            const QString accessCode = accessCodeEdit_->text();
            if (accessCode.isEmpty()) {
                if (promptForAccessCode) {
                    accessCodeEdit_->setFocus();
                }
                appendOutput("Selected encrypted receiver " + receiverName + "; enter the matching access code before Start\n");
            } else if (!receiver.accessFingerprint.isEmpty() && receiver.accessFingerprint != "NONE") {
                const QString typedFingerprint = accessCodeFingerprintText(accessCode);
                if (typedFingerprint == receiver.accessFingerprint) {
                    appendOutput("Access code matches the selected receiver fingerprint\n");
                } else {
                    clearAccessCodeForRetry("Access code does not match the selected receiver. Try again.");
                }
            }
        } else if (receiver.source == ReceiverSource::Tailscale) {
            appendOutput("Selected Tailscale peer " + receiverName + "; make sure Watch is running on that computer\n");
        }

        appendOutput("Selected receiver " + receiverName + " at " + receiver.host + ":" + QString::number(receiver.port) + "\n");
        refreshCommand();
    }

    void startDiscovery(bool automatic)
    {
        if (receiverScanRunning()) {
            return;
        }
        if (automatic && !shareMode()) {
            return;
        }
        if (process_->state() != QProcess::NotRunning) {
            if (!automatic) {
                QMessageBox::information(this, "Already running", "Stop the current session before refreshing targets.");
            }
            return;
        }

        const QString program = enginePath();
        if (!QFileInfo::exists(program)) {
            if (!automatic) {
                QMessageBox::critical(this, "Missing engine", "ScreenShare.exe was not found beside the UI executable.");
            }
            return;
        }

        discoverySelectFirst_ = !automatic;
        discoveryLogOutput_ = !automatic;
        tailscaleLogOutput_ = !automatic;
        receiverScanNotificationShown_ = false;
        discoveryOutput_.clear();
        tailscaleOutput_.clear();
        if (!automatic) {
            appendOutput("\nRefreshing targets...\n");
        }
        discoveryProcess_->setProgram(program);
        discoveryProcess_->setArguments({"--lan-discover", "--lan-discover-seconds", "2"});
        discoveryProcess_->setWorkingDirectory(QFileInfo(program).absolutePath());
        setDiscovering(true);
        discoveryProcess_->start();
        startTailscaleDiscovery();
    }

    void finishDiscovery(int code, QProcess::ExitStatus status)
    {
        if (status != QProcess::NormalExit || code != 0) {
            if (discoveryLogOutput_) {
                appendOutput("LAN discovery finished with exit code " + QString::number(code) + "\n");
            }
            lanReceivers_.clear();
            updateReceiverList(combinedReceivers());
            completeReceiverScanIfDone();
            return;
        }

        lanReceivers_ = parseDiscoveredReceivers(discoveryOutput_);
        updateReceiverList(combinedReceivers());
        completeReceiverScanIfDone();
    }

    void startTailscaleDiscovery()
    {
        tailscaleProcess_->setProgram("tailscale");
        tailscaleProcess_->setArguments({"status", "--json"});
        tailscaleProcess_->setWorkingDirectory(QDir::currentPath());
        tailscaleProcess_->start();
    }

    void finishTailscaleDiscovery(int code, QProcess::ExitStatus status)
    {
        if (status != QProcess::NormalExit || code != 0) {
            if (tailscaleLogOutput_) {
                appendOutput("Tailscale peer refresh finished with exit code " + QString::number(code) + "\n");
            }
            tailscaleReceivers_.clear();
            updateReceiverList(combinedReceivers());
            completeReceiverScanIfDone();
            return;
        }

        tailscaleReceivers_ = parseTailscaleStatusReceivers(tailscaleOutput_, sharePortSpin_->value());
        updateReceiverList(combinedReceivers());
        if (tailscaleLogOutput_ && !tailscaleReceivers_.isEmpty()) {
            appendOutput("Found " + QString::number(tailscaleReceivers_.size()) + " Tailscale peers\n");
        }
        completeReceiverScanIfDone();
    }

    void refreshAudioDevices(bool automatic)
    {
        if (audioDeviceProcess_->state() != QProcess::NotRunning) {
            return;
        }

        const QString program = enginePath();
        if (!QFileInfo::exists(program)) {
            if (!automatic) {
                QMessageBox::critical(this, "Missing engine", "ScreenShare.exe was not found beside the UI executable.");
            }
            return;
        }

        audioDeviceLogOutput_ = !automatic;
        audioDeviceOutput_.clear();
        if (!automatic) {
            appendOutput("\nRefreshing audio devices...\n");
        }
        audioDeviceProcess_->setProgram(program);
        audioDeviceProcess_->setArguments({"--list-audio-devices"});
        audioDeviceProcess_->setWorkingDirectory(QFileInfo(program).absolutePath());
        setAudioDeviceRefreshing(true);
        audioDeviceProcess_->start();
    }

    void finishAudioDeviceRefresh(int code, QProcess::ExitStatus status)
    {
        setAudioDeviceRefreshing(false);
        if (status != QProcess::NormalExit || code != 0) {
            if (audioDeviceLogOutput_) {
                appendOutput("Audio device refresh finished with exit code " + QString::number(code) + "\n");
            }
            return;
        }

        const QVector<AudioOutputDevice> devices = parseAudioOutputDevices(audioDeviceOutput_);
        updateAudioDeviceList(devices);
        if (audioDeviceLogOutput_) {
            appendOutput("Found " + QString::number(devices.size()) + " output audio devices\n");
        }
    }

    void updateAudioDeviceList(const QVector<AudioOutputDevice>& devices)
    {
        if (audioDeviceCombo_ == nullptr) {
            return;
        }

        const QString selectedId = audioDeviceCombo_->currentData().toString();
        audioDeviceCombo_->blockSignals(true);
        audioDeviceCombo_->clear();
        audioDeviceCombo_->addItem("Default output", QString());
        audioDeviceCombo_->setItemData(0, "Use the current Windows default output device", Qt::ToolTipRole);

        int selectedIndex = 0;
        for (const auto& device : devices) {
            QString label = device.name.isEmpty() ? "Unnamed output" : device.name;
            if (device.isDefault) {
                label += " (default)";
            }
            audioDeviceCombo_->addItem(label, device.id);
            const int itemIndex = audioDeviceCombo_->count() - 1;
            audioDeviceCombo_->setItemData(itemIndex, device.id, Qt::ToolTipRole);
            if (!selectedId.isEmpty() && device.id == selectedId) {
                selectedIndex = itemIndex;
            }
        }

        audioDeviceCombo_->setCurrentIndex(selectedIndex);
        audioDeviceCombo_->blockSignals(false);
        updateAudioDeviceStatus(devices.size());
        refreshCommand();
    }

    void updateAudioDeviceStatus(int deviceCount)
    {
        if (audioDeviceStatusLabel_ == nullptr) {
            return;
        }
        if (deviceCount <= 0) {
            audioDeviceStatusLabel_->setText("Default output");
            return;
        }
        audioDeviceStatusLabel_->setText(QString::number(deviceCount) + " output devices available");
    }

    bool selectedReceiverApplies() const
    {
        if (!selectedReceiverKnown_ || !shareMode()) {
            return false;
        }
        if (sharePeerInviteEdit_ != nullptr && !sharePeerInviteEdit_->text().trimmed().isEmpty()) {
            return false;
        }
        if (shareHostEdit_->text().trimmed() != selectedReceiverHost_ ||
            sharePortSpin_->value() != selectedReceiverPort_) {
            return false;
        }
        return true;
    }

    bool selectedReceiverAccessCodeMatches(const QString& accessCode) const
    {
        return selectedReceiverAccessFingerprint_.isEmpty() ||
               selectedReceiverAccessFingerprint_ == "NONE" ||
               accessCodeFingerprintText(accessCode) == selectedReceiverAccessFingerprint_;
    }

    void clearAccessCodeForRetry(const QString& message)
    {
        accessCodeEdit_->clear();
        allowPlaintextCheck_->setChecked(false);
        accessCodeEdit_->setFocus();
        QMessageBox::warning(this, "Access code mismatch", message);
    }

    bool validateSelectedReceiverSecurity()
    {
        if (!selectedReceiverApplies()) {
            return true;
        }
        if (!selectedReceiverSecurityKnown_) {
            return true;
        }

        const QString accessCode = accessCodeEdit_->text();
        if (selectedReceiverEncrypted_) {
            if (accessCode.isEmpty()) {
                accessCodeEdit_->setFocus();
                QMessageBox::warning(
                    this,
                    "Access code required",
                    "Enter the access code for the selected receiver.");
                return false;
            }
            if (!selectedReceiverAccessCodeMatches(accessCode)) {
                clearAccessCodeForRetry("That access code does not match. Try again.");
                return false;
            }
            return true;
        }

        if (!accessCode.isEmpty()) {
            QMessageBox::warning(
                this,
                "Security mismatch",
                "The selected receiver is plaintext. Clear the access code or restart Watch with the same access code.");
            return false;
        }
        return true;
    }

    bool receiverScanRunning() const
    {
        return discoveryProcess_->state() != QProcess::NotRunning ||
               tailscaleProcess_->state() != QProcess::NotRunning;
    }

    void completeReceiverScanIfDone()
    {
        if (receiverScanRunning()) {
            setDiscovering(true);
            return;
        }

        setDiscovering(false);
        if (!discoverySelectFirst_ || receiverScanNotificationShown_) {
            return;
        }

        receiverScanNotificationShown_ = true;
        if (discoveredReceivers_.isEmpty()) {
            QMessageBox::information(
                this,
                "No targets found",
                "No LAN receivers or Tailscale peers were found. You can still type the receiver IP manually.");
        } else if (discoveredReceivers_.size() == 1) {
            selectDiscoveredReceiver(discoveredReceivers_.front(), true);
        } else {
            appendOutput("Found " + QString::number(discoveredReceivers_.size()) + " targets\n");
        }
    }

    void updateTailscaleReceiverPorts(int port)
    {
        bool changed = false;
        for (auto& receiver : tailscaleReceivers_) {
            if (receiver.port != port) {
                receiver.port = port;
                changed = true;
            }
        }
        if (changed) {
            updateReceiverList(combinedReceivers());
        }
    }

    void setDiscovering(bool discovering)
    {
        if (findLanButton_ != nullptr) {
            findLanButton_->setEnabled(!discovering && process_->state() == QProcess::NotRunning);
            findLanButton_->setText(discovering ? "Scanning..." : "Refresh");
        }
        updateReceiverStatus(discovering);
        if (receiverList_ != nullptr) {
            receiverList_->setEnabled(!discovering);
        }
        if (process_->state() == QProcess::NotRunning) {
            statusBadge_->setText(discovering ? "Finding" : "Idle");
            statusBadge_->setProperty("class", discovering ? "StatusRunning" : "StatusIdle");
            repolish(statusBadge_);
        }
    }

    void setAudioDeviceRefreshing(bool refreshing)
    {
        if (refreshAudioDevicesButton_ != nullptr) {
            refreshAudioDevicesButton_->setEnabled(!refreshing && process_->state() == QProcess::NotRunning);
            refreshAudioDevicesButton_->setText(refreshing ? "Scanning..." : "Refresh");
        }
        if (audioDeviceStatusLabel_ != nullptr) {
            if (refreshing) {
                audioDeviceStatusLabel_->setText("Scanning output devices");
            } else if (audioDeviceCombo_ != nullptr) {
                updateAudioDeviceStatus(std::max(0, audioDeviceCombo_->count() - 1));
            }
        }
    }

    void setRunning(bool running)
    {
        if (running) {
            actionButton_->setText("Stop");
            actionButton_->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
            actionButton_->setProperty("class", "Danger");
            statusBadge_->setText("Running");
            statusBadge_->setProperty("class", "StatusRunning");
        } else {
            actionButton_->setText("Start");
            actionButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
            actionButton_->setProperty("class", "");
            statusBadge_->setText("Idle");
            statusBadge_->setProperty("class", "StatusIdle");
        }
        repolish(actionButton_);
        repolish(statusBadge_);

        shareModeButton_->setEnabled(!running);
        watchModeButton_->setEnabled(!running);
        if (findLanButton_ != nullptr) {
            findLanButton_->setEnabled(!running && !receiverScanRunning());
        }
        if (receiverList_ != nullptr) {
            receiverList_->setEnabled(!running && !receiverScanRunning());
        }
        if (audioDeviceCombo_ != nullptr) {
            audioDeviceCombo_->setEnabled(!running);
        }
        if (refreshAudioDevicesButton_ != nullptr) {
            refreshAudioDevicesButton_->setEnabled(!running && audioDeviceProcess_->state() == QProcess::NotRunning);
        }
    }

    void appendOutput(const QString& text)
    {
        outputEdit_->moveCursor(QTextCursor::End);
        outputEdit_->insertPlainText(text);
        outputEdit_->moveCursor(QTextCursor::End);
    }

    CompactStackedWidget* optionStack_ = nullptr;
    QPushButton* shareModeButton_ = nullptr;
    QPushButton* watchModeButton_ = nullptr;
    QLabel* statusBadge_ = nullptr;
    QCheckBox* darkModeCheck_ = nullptr;
    QLabel* commandPreview_ = nullptr;
    QPlainTextEdit* outputEdit_ = nullptr;
    QPushButton* actionButton_ = nullptr;
    QProcess* process_ = nullptr;
    QProcess* discoveryProcess_ = nullptr;
    QProcess* tailscaleProcess_ = nullptr;
    QProcess* audioDeviceProcess_ = nullptr;
    QTimer* receiverRefreshTimer_ = nullptr;

    QListWidget* receiverList_ = nullptr;
    QLabel* receiverStatusLabel_ = nullptr;
    QLineEdit* shareHostEdit_ = nullptr;
    QLineEdit* sharePeerInviteEdit_ = nullptr;
    QLineEdit* shareLocalInviteEdit_ = nullptr;
    QSpinBox* sharePortSpin_ = nullptr;
    QPushButton* findLanButton_ = nullptr;
    QSpinBox* displaySpin_ = nullptr;
    QSpinBox* fpsSpin_ = nullptr;
    QComboBox* resolutionCombo_ = nullptr;
    QComboBox* audioDeviceCombo_ = nullptr;
    QPushButton* refreshAudioDevicesButton_ = nullptr;
    QLabel* audioDeviceStatusLabel_ = nullptr;
    QSpinBox* watchPortSpin_ = nullptr;
    QLineEdit* watchPeerInviteEdit_ = nullptr;
    QCheckBox* lanDiscoverableCheck_ = nullptr;
    QCheckBox* mutedCheck_ = nullptr;
    QSpinBox* volumeSpin_ = nullptr;
    QSpinBox* previewLatencySpin_ = nullptr;

    QLineEdit* accessCodeEdit_ = nullptr;
    QPushButton* generateAccessCodeButton_ = nullptr;
    QPushButton* copyAccessCodeButton_ = nullptr;
    QCheckBox* allowPlaintextCheck_ = nullptr;
    QCheckBox* reportCheck_ = nullptr;
    QLineEdit* reportPathEdit_ = nullptr;
    QPushButton* browseReportButton_ = nullptr;
    bool reportPathEdited_ = false;
    bool stopRequested_ = false;
    bool forcedStop_ = false;
    quint64 runSerial_ = 0;
    QString stopFilePath_;
    QString discoveryOutput_;
    QString tailscaleOutput_;
    QString audioDeviceOutput_;
    QVector<DiscoveredReceiver> discoveredReceivers_;
    QVector<DiscoveredReceiver> lanReceivers_;
    QVector<DiscoveredReceiver> tailscaleReceivers_;
    bool discoverySelectFirst_ = false;
    bool discoveryLogOutput_ = false;
    bool tailscaleLogOutput_ = false;
    bool receiverScanNotificationShown_ = false;
    bool audioDeviceLogOutput_ = false;
    bool selectedReceiverKnown_ = false;
    bool selectedReceiverSecurityKnown_ = false;
    bool selectedReceiverEncrypted_ = false;
    QString selectedReceiverHost_;
    int selectedReceiverPort_ = 0;
    QString selectedReceiverAccessFingerprint_;
};

QString appStyleSheet(bool darkMode)
{
    if (darkMode) {
        return QString::fromUtf8(R"(
* {
    font-family: "Segoe UI", "Inter", "Arial";
    font-size: 10.5pt;
}
QWidget {
    background: #0e1216;
    color: #e6ecf2;
}
QWidget#LeftHost, QWidget#OptionPage, QWidget#FormRow,
QStackedWidget#OptionStack {
    background: transparent;
}
QScrollArea#LeftScroll, QScrollArea#LeftScroll > QWidget > QWidget {
    background: transparent;
    border: 0;
}
QLabel {
    background: transparent;
}
QLabel[class="HeroTitle"] {
    font-size: 18pt;
    font-weight: 700;
    color: #f4f7fb;
    letter-spacing: 0.3px;
}
QLabel[class="Subtle"] {
    color: #8a96a5;
    font-size: 9.5pt;
}
QLabel[class="PanelTitle"] {
    font-size: 10.5pt;
    font-weight: 700;
    color: #cdd6e1;
    text-transform: uppercase;
    letter-spacing: 1.2px;
}
QLabel[class="FieldLabel"] {
    color: #9aa6b5;
    font-weight: 500;
}
QLabel[class="CommandPreview"] {
    background: #050709;
    color: #d8f3ec;
    border: 1px solid #1a2530;
    border-radius: 8px;
    padding: 12px 14px;
    font-family: "Cascadia Mono", "Consolas";
    font-size: 9.5pt;
}
QLabel[class="StatusIdle"], QLabel[class="StatusRunning"] {
    border-radius: 17px;
    padding: 0 18px;
    font-weight: 650;
    min-width: 96px;
}
QLabel[class="StatusIdle"] {
    background: #1a212a;
    color: #93a0b0;
    border: 1px solid #232c37;
}
QLabel[class="StatusRunning"] {
    background: #0f2e2a;
    color: #6cdcc6;
    border: 1px solid #1f4a44;
}
QFrame#Panel {
    background: #161b22;
    border: 1px solid #232c37;
    border-radius: 10px;
}
QFrame#ModeBar {
    background: #161b22;
    border: 1px solid #232c37;
    border-radius: 12px;
}
QPushButton#ModeButton {
    background: transparent;
    color: #a5b1c0;
    border: 0;
    border-radius: 8px;
    padding: 9px 22px;
    font-weight: 650;
}
QPushButton#ModeButton:hover {
    background: #1c232c;
    color: #e6ecf2;
}
QPushButton#ModeButton:checked {
    background: #1a9b89;
    color: #ffffff;
}
QPushButton#ModeButton:disabled {
    color: #4d5663;
}
QLineEdit, QSpinBox, QComboBox {
    background: #0b0f14;
    border: 1px solid #2a3340;
    border-radius: 6px;
    color: #e6ecf2;
    padding: 6px 9px;
    selection-background-color: #1a9b89;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus {
    border: 1px solid #22b8a5;
}
QSpinBox::up-button, QSpinBox::down-button {
    width: 16px;
    background: transparent;
    border: 0;
}
QComboBox::drop-down {
    border: 0;
    width: 22px;
}
QComboBox QAbstractItemView {
    background: #161b22;
    color: #e6ecf2;
    border: 1px solid #2a3340;
    selection-background-color: #1a9b89;
    padding: 4px;
}
QCheckBox {
    spacing: 8px;
    color: #c9d2dd;
    background: transparent;
}
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border-radius: 4px;
    border: 1px solid #3a4655;
    background: #0b0f14;
}
QCheckBox::indicator:checked {
    background: #1a9b89;
    border: 1px solid #1a9b89;
    image: none;
}
QCheckBox#ThemeSwitch {
    color: #aab5c3;
    font-weight: 600;
    background: transparent;
}
QPushButton {
    border: 0;
    border-radius: 8px;
    padding: 9px 18px;
    font-weight: 650;
    color: #ffffff;
}
QPushButton#PrimaryButton {
    background: #1a9b89;
    color: #ffffff;
    padding: 0 24px;
}
QPushButton#PrimaryButton:hover {
    background: #21b8a3;
}
QPushButton#PrimaryButton:pressed {
    background: #148876;
}
QPushButton#PrimaryButton[class="Danger"] {
    background: #c64a4a;
}
QPushButton#PrimaryButton[class="Danger"]:hover {
    background: #d96060;
}
QPushButton#PrimaryButton[class="Danger"]:pressed {
    background: #a83b3b;
}
QPushButton#SecondaryButton {
    background: #1f2731;
    color: #dde4ee;
}
QPushButton#SecondaryButton:hover {
    background: #28323e;
}
QPushButton#GhostButton {
    background: transparent;
    color: #93a0b0;
    padding: 6px 12px;
}
QPushButton#GhostButton:hover {
    background: #1c232c;
    color: #e6ecf2;
}
QPushButton:disabled {
    background: #1a2027;
    color: #5d6776;
}
QPlainTextEdit {
    background: #050709;
    color: #d8f3ec;
    border: 1px solid #1a2530;
    border-radius: 8px;
    padding: 12px;
    font-family: "Cascadia Mono", "Consolas";
    font-size: 9.5pt;
}
QListWidget#ReceiverList {
    background: #0b0f14;
    border: 1px solid #2a3340;
    border-radius: 8px;
    padding: 4px;
    color: #e6ecf2;
}
QListWidget#ReceiverList::item {
    border-radius: 6px;
    padding: 8px 10px;
}
QListWidget#ReceiverList::item:selected {
    background: #1a9b89;
    color: #ffffff;
}
QListWidget#ReceiverList::item:hover {
    background: #1c232c;
}
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 4px;
}
QScrollBar::handle:vertical {
    background: #3a4655;
    border-radius: 4px;
    min-height: 28px;
}
QScrollBar::handle:vertical:hover {
    background: #4a5667;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0;
    background: transparent;
}
)");
    }

    return QString::fromUtf8(R"(
* {
    font-family: "Segoe UI", "Inter", "Arial";
    font-size: 10.5pt;
}
QWidget {
    background: #f4f6fa;
    color: #15202b;
}
QWidget#LeftHost, QWidget#OptionPage, QWidget#FormRow,
QStackedWidget#OptionStack {
    background: transparent;
}
QScrollArea#LeftScroll, QScrollArea#LeftScroll > QWidget > QWidget {
    background: transparent;
    border: 0;
}
QLabel {
    background: transparent;
}
QLabel[class="HeroTitle"] {
    font-size: 18pt;
    font-weight: 700;
    color: #0f1820;
    letter-spacing: 0.3px;
}
QLabel[class="Subtle"] {
    color: #5e6b7a;
    font-size: 9.5pt;
}
QLabel[class="PanelTitle"] {
    font-size: 10.5pt;
    font-weight: 700;
    color: #3a4756;
    text-transform: uppercase;
    letter-spacing: 1.2px;
}
QLabel[class="FieldLabel"] {
    color: #5e6b7a;
    font-weight: 500;
}
QLabel[class="CommandPreview"] {
    background: #0f1820;
    color: #e6f3ef;
    border-radius: 8px;
    padding: 12px 14px;
    font-family: "Cascadia Mono", "Consolas";
    font-size: 9.5pt;
}
QLabel[class="StatusIdle"], QLabel[class="StatusRunning"] {
    border-radius: 17px;
    padding: 0 18px;
    font-weight: 650;
    min-width: 96px;
}
QLabel[class="StatusIdle"] {
    background: #e6eaf1;
    color: #4f5d6e;
    border: 1px solid #dadfe7;
}
QLabel[class="StatusRunning"] {
    background: #d9f1ea;
    color: #0f6b5d;
    border: 1px solid #b6e2d5;
}
QFrame#Panel {
    background: #ffffff;
    border: 1px solid #e0e5ec;
    border-radius: 10px;
}
QFrame#ModeBar {
    background: #ffffff;
    border: 1px solid #e0e5ec;
    border-radius: 12px;
}
QPushButton#ModeButton {
    background: transparent;
    color: #5e6b7a;
    border: 0;
    border-radius: 8px;
    padding: 9px 22px;
    font-weight: 650;
}
QPushButton#ModeButton:hover {
    background: #eef1f6;
    color: #15202b;
}
QPushButton#ModeButton:checked {
    background: #157a6e;
    color: #ffffff;
}
QPushButton#ModeButton:disabled {
    color: #aab3bf;
}
QLineEdit, QSpinBox, QComboBox {
    background: #ffffff;
    border: 1px solid #cfd6df;
    border-radius: 6px;
    color: #15202b;
    padding: 6px 9px;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus {
    border: 1px solid #157a6e;
}
QSpinBox::up-button, QSpinBox::down-button {
    width: 16px;
    background: transparent;
    border: 0;
}
QComboBox::drop-down {
    border: 0;
    width: 22px;
}
QCheckBox {
    spacing: 8px;
    background: transparent;
}
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border-radius: 4px;
    border: 1px solid #b8c1ce;
    background: #ffffff;
}
QCheckBox::indicator:checked {
    background: #157a6e;
    border: 1px solid #157a6e;
}
QCheckBox#ThemeSwitch {
    color: #4f5d6e;
    font-weight: 600;
    background: transparent;
}
QPushButton {
    border: 0;
    border-radius: 8px;
    padding: 9px 18px;
    font-weight: 650;
}
QPushButton#PrimaryButton {
    background: #157a6e;
    color: #ffffff;
    padding: 0 24px;
}
QPushButton#PrimaryButton:hover {
    background: #1a9385;
}
QPushButton#PrimaryButton:pressed {
    background: #0f6b5d;
}
QPushButton#PrimaryButton[class="Danger"] {
    background: #c84545;
    color: #ffffff;
}
QPushButton#PrimaryButton[class="Danger"]:hover {
    background: #d65b5b;
}
QPushButton#PrimaryButton[class="Danger"]:pressed {
    background: #ad3838;
}
QPushButton#SecondaryButton {
    background: #eef1f6;
    color: #243140;
}
QPushButton#SecondaryButton:hover {
    background: #e2e7ee;
}
QPushButton#GhostButton {
    background: transparent;
    color: #5e6b7a;
    padding: 6px 12px;
}
QPushButton#GhostButton:hover {
    background: #eef1f6;
    color: #15202b;
}
QPushButton:disabled {
    background: #dee3ec;
    color: #8a95a3;
}
QPlainTextEdit {
    background: #0f1820;
    color: #e9f3f1;
    border: 0;
    border-radius: 8px;
    padding: 12px;
    font-family: "Cascadia Mono", "Consolas";
    font-size: 9.5pt;
}
QListWidget#ReceiverList {
    background: #ffffff;
    border: 1px solid #cfd6df;
    border-radius: 8px;
    padding: 4px;
    color: #15202b;
}
QListWidget#ReceiverList::item {
    border-radius: 6px;
    padding: 8px 10px;
}
QListWidget#ReceiverList::item:selected {
    background: #157a6e;
    color: #ffffff;
}
QListWidget#ReceiverList::item:hover {
    background: #eef1f6;
}
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 4px;
}
QScrollBar::handle:vertical {
    background: #c4ccd7;
    border-radius: 4px;
    min-height: 28px;
}
QScrollBar::handle:vertical:hover {
    background: #aab5c3;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0;
    background: transparent;
}
)");
}

} // namespace

int main(int argc, char** argv)
{
    for (int index = 1; index < argc; ++index) {
        const QString arg = QString::fromLocal8Bit(argv[index]);
        if (arg == "--self-test") {
            if (!QFileInfo::exists(enginePath())) {
                return 1;
            }
            const auto peers = parseTailscaleStatusReceivers(QString::fromUtf8(R"json({
                "Peer": {
                    "node-a": {
                        "HostName": "friend-pc",
                        "TailscaleIPs": ["100.64.0.2", "fd7a:115c:a1e0::2"],
                        "Online": true
                    },
                    "node-b": {
                        "HostName": "offline-pc",
                        "TailscaleIPs": ["100.64.0.3"],
                        "Online": false
                    }
                }
            })json"), 5000);
            return peers.size() == 1 && peers.front().host == "100.64.0.2" ? 0 : 2;
        }
    }

    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    app.setStyleSheet(appStyleSheet(true));

    MainWindow window;
    window.show();
    return app.exec();
}
