#include "transport/UdpCrypto.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QByteArray>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QAbstractAnimation>
#include <QtCore/QPropertyAnimation>
#include <QtCore/QEasingCurve>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QIODevice>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QPoint>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QSize>
#include <QtCore/QStringList>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtCore/QVector>
#include <QtGui/QClipboard>
#include <QtGui/QScreen>
#include <QtGui/QTextCursor>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGraphicsOpacityEffect>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

constexpr const char* kRoomLinkPrefix = "screenshare-room-v1;";
constexpr const char* kDefaultSignalServer = "https://screenshare-signaling.bit-yeet.workers.dev";

QStringList startupArguments(int argc, char** argv)
{
#ifdef _WIN32
    int wideArgc = 0;
    LPWSTR* wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
    if (wideArgv != nullptr) {
        QStringList arguments;
        arguments.reserve(wideArgc);
        for (int index = 0; index < wideArgc; ++index) {
            arguments.push_back(QString::fromWCharArray(wideArgv[index]));
        }
        LocalFree(wideArgv);
        return arguments;
    }
#endif

    QStringList arguments;
    arguments.reserve(argc);
    for (int index = 0; index < argc; ++index) {
        arguments.push_back(QString::fromLocal8Bit(argv[index]));
    }
    return arguments;
}

QString enginePath(const QString& uiExecutablePath = QString())
{
    if (!uiExecutablePath.isEmpty()) {
        const QDir appDir(QFileInfo(uiExecutablePath).absoluteDir());
        const QString besideUi = appDir.filePath("ScreenShare.exe");
        if (QFileInfo::exists(besideUi)) {
            return besideUi;
        }
    }

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
    return value.startsWith("nat_invite=") ||
           value.startsWith("screenshare-invite-v1") ||
           value.startsWith("ss1p:") ||
           value.startsWith("ss1e:");
}

QStringList parseExtraShareTargets(QString value)
{
    value.replace(',', ' ');
    value.replace(';', ' ');
    value.replace('\r', ' ');
    value.replace('\n', ' ');
    return value.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

QString directShareTargetError(const QString& value)
{
    const QString target = value.trimmed();
    if (target.isEmpty()) {
        return {};
    }
    if (looksLikeNatInvite(target)) {
        return "Manual targets use HOST:PORT. Use the Internet tab's shared room invite for NAT watchers.";
    }

    const int separator = target.lastIndexOf(':');
    if (separator <= 0 || separator == target.size() - 1) {
        return "Extra viewers must be written as HOST:PORT.";
    }

    bool ok = false;
    const int port = target.mid(separator + 1).toInt(&ok);
    if (!ok || port < 1 || port > 65535) {
        return "Extra viewer ports must be between 1 and 65535.";
    }

    return {};
}

QString normalizeInviteText(QString invite)
{
    invite = invite.trimmed();
    if (invite.startsWith("screenshare-invite-v1")) {
        invite.prepend("nat_invite=");
    }
    return invite;
}

QStringList extractInviteLines(const QString& output)
{
    QStringList invites;
    const auto addInvite = [&invites](const QString& rawInvite) {
        const QString invite = normalizeInviteText(rawInvite);
        if (looksLikeNatInvite(invite) && !invites.contains(invite)) {
            invites.push_back(invite);
        }
    };

    const QString trimmed = normalizeInviteText(output);
    if (looksLikeNatInvite(trimmed)) {
        invites.push_back(trimmed);
        return invites;
    }

    const QStringList lines = output.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        if (line.startsWith("send_this_invite_to_peer=")) {
            addInvite(line.mid(QStringLiteral("send_this_invite_to_peer=").size()));
        }
    }
    for (const QString& line : lines) {
        addInvite(line);
    }

    const QRegularExpression invitePattern(
        QStringLiteral(R"(((?:nat_invite=)?(?:screenshare-invite-v1;|ss1[pe]:)[^\s'"`]+))"));
    QRegularExpressionMatchIterator matches = invitePattern.globalMatch(output);
    while (matches.hasNext()) {
        addInvite(matches.next().captured(1));
    }
    return invites;
}

QString extractInviteLine(const QString& output)
{
    const QStringList invites = extractInviteLines(output);
    if (!invites.isEmpty()) {
        return invites.front();
    }
    return {};
}

QString generatedRoomId()
{
    const QString id = QUuid::createUuid().toString(QUuid::Id128).left(10).toLower();
    return "room-" + id;
}

bool validRoomId(const QString& roomId)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{3,96}$"));
    return pattern.match(roomId).hasMatch();
}

QString roomLink(const QString& room, int port)
{
    return QStringLiteral("%1room=%2;port=%3")
        .arg(QString::fromUtf8(kRoomLinkPrefix), room.trimmed(), QString::number(port));
}

QString defaultSignalServer()
{
    return QString::fromUtf8(kDefaultSignalServer);
}

bool parseRoomLink(const QString& text, QString* server, QString* room, int* port)
{
    const QString trimmed = text.trimmed();
    const int prefixIndex = trimmed.indexOf(QString::fromUtf8(kRoomLinkPrefix));
    if (prefixIndex < 0) {
        return false;
    }

    const QString link = trimmed.mid(prefixIndex + QString::fromUtf8(kRoomLinkPrefix).size());
    const QStringList fields = link.split(';', Qt::SkipEmptyParts);
    QString parsedServer;
    QString parsedRoom;
    int parsedPort = 0;
    for (const QString& field : fields) {
        const int separator = field.indexOf('=');
        if (separator <= 0) {
            continue;
        }
        const QString key = field.left(separator).trimmed();
        const QString value = field.mid(separator + 1).trimmed();
        if (key == "server") {
            parsedServer = value;
        } else if (key == "room") {
            parsedRoom = value;
        } else if (key == "port") {
            bool ok = false;
            const int valuePort = value.toInt(&ok);
            if (ok && valuePort >= 1 && valuePort <= 65535) {
                parsedPort = valuePort;
            }
        }
    }

    if (!validRoomId(parsedRoom) || parsedPort == 0) {
        return false;
    }
    if (server != nullptr) {
        *server = parsedServer.isEmpty() ? defaultSignalServer() : parsedServer;
    }
    if (room != nullptr) {
        *room = parsedRoom;
    }
    if (port != nullptr) {
        *port = parsedPort;
    }
    return true;
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

struct DisplayChoice {
    int index = 0;
    QString outputName;
    int width = 0;
    int height = 0;
    int left = 0;
    int top = 0;
    QString adapterName;
    bool attached = true;
};

QString displayChoiceText(const DisplayChoice& display, QScreen* primaryScreen)
{
    QStringList parts;
    parts << "Display " + QString::number(display.index);
    if (!display.outputName.trimmed().isEmpty()) {
        parts << display.outputName.trimmed();
    }
    if (display.width > 0 && display.height > 0) {
        parts << QString("%1x%2").arg(display.width).arg(display.height);
    }
    parts << QString("at %1,%2").arg(display.left).arg(display.top);
    if (primaryScreen != nullptr && primaryScreen->geometry().topLeft() == QPoint(display.left, display.top)) {
        parts << "Primary";
    }
    if (!display.attached) {
        parts << "Detached";
    }
    return parts.join(" - ");
}

QVector<DisplayChoice> parseDisplayChoices(const QString& output)
{
    QVector<DisplayChoice> displays;
    const QRegularExpression displayPattern(QStringLiteral(
        R"display(\[(\d+)\]\s+([^\s]+)\s+(\d+)x(\d+)\s+at\s+\((-?\d+),(-?\d+)\)\s+adapter="([^"]*)"\s+attached=(yes|no))display"));
    auto matches = displayPattern.globalMatch(output);
    while (matches.hasNext()) {
        const auto match = matches.next();
        DisplayChoice display;
        display.index = match.captured(1).toInt();
        display.outputName = match.captured(2).trimmed();
        display.width = match.captured(3).toInt();
        display.height = match.captured(4).toInt();
        display.left = match.captured(5).toInt();
        display.top = match.captured(6).toInt();
        display.adapterName = match.captured(7).trimmed();
        display.attached = match.captured(8) == "yes";
        displays.push_back(std::move(display));
    }
    return displays;
}

QString lastLogFieldValue(const QString& output, const QString& field)
{
    const QRegularExpression pattern(
        QStringLiteral(R"((?:^|\s)%1=([^\s,]+))").arg(QRegularExpression::escape(field)));
    QRegularExpressionMatchIterator matches = pattern.globalMatch(output);
    QString value;
    while (matches.hasNext()) {
        value = matches.next().captured(1).trimmed();
    }
    return value;
}

enum class AccessCodeProblem {
    None,
    Required,
    Mismatch,
};

bool logFieldPositive(const QString& output, const QString& field)
{
    const QString value = lastLogFieldValue(output, field);
    if (value.isEmpty()) {
        return false;
    }
    bool ok = false;
    return value.toULongLong(&ok) > 0 && ok;
}

AccessCodeProblem detectAccessCodeProblem(const QString& output)
{
    if (output.contains("requires --access-code CODE", Qt::CaseInsensitive) ||
        output.contains("requires an access code", Qt::CaseInsensitive) ||
        output.contains("--access-code CODE or --allow-plaintext", Qt::CaseInsensitive) ||
        lastLogFieldValue(output, "access_code") == "required") {
        return AccessCodeProblem::Required;
    }

    if (output.contains("could not be decrypted", Qt::CaseInsensitive) ||
        output.contains("access code does not match", Qt::CaseInsensitive) ||
        output.contains("does not match the peer invite fingerprint", Qt::CaseInsensitive) ||
        output.contains("does not match the local invite fingerprint", Qt::CaseInsensitive) ||
        logFieldPositive(output, "access_rejected_datagrams") ||
        logFieldPositive(output, "crypto_rejected_datagrams") ||
        logFieldPositive(output, "udp_feedback_access_rejected") ||
        logFieldPositive(output, "udp_feedback_crypto_rejected")) {
        return AccessCodeProblem::Mismatch;
    }

    return AccessCodeProblem::None;
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
constexpr int kPeerStatusPollMs = 500;
constexpr int kPeerActivityTimeoutMs = 3000;
constexpr const char* kDefaultStunServer = "stun.l.google.com:19302";

enum class ReceiverSource {
    Lan,
    Tailscale,
};

enum class InviteTarget {
    None,
    Share,
    Watch,
};

enum class ShareConnectionMethod {
    Nearby = 0,
    InternetInvite = 1,
    ManualAddress = 2,
};

enum class WatchConnectionMethod {
    Nearby = 0,
    InternetInvite = 1,
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

// PageStack is a drop-in replacement for QStackedWidget that uses show/hide
// instead of QStackedLayout. QStackedLayout::minimumSize() returns the max
// minimum across *all* pages, which leaked past our sizeHint() override and
// forced the column layout to allocate the largest page's height to every
// page. With show/hide, hidden pages contribute zero to the QVBoxLayout, so
// the container's geometry tracks the visible page exactly and switching
// pages cannot leave behind a vertical gap.
class PageStack final : public QWidget {
public:
    explicit PageStack(QWidget* parent = nullptr) : QWidget(parent)
    {
        layout_ = new QVBoxLayout(this);
        layout_->setContentsMargins(0, 0, 0, 0);
        layout_->setSpacing(0);
    }

    void addPage(QWidget* page)
    {
        page->setVisible(pages_.isEmpty());
        layout_->addWidget(page);
        pages_.append(page);
    }

    void setCurrentIndex(int index)
    {
        if (index < 0 || index >= pages_.size() || index == currentIndex_) {
            return;
        }
        for (int i = 0; i < pages_.size(); ++i) {
            pages_[i]->setVisible(i == index);
        }
        currentIndex_ = index;
    }

    int currentIndex() const { return currentIndex_; }

    QWidget* currentWidget() const
    {
        return (currentIndex_ >= 0 && currentIndex_ < pages_.size()) ? pages_[currentIndex_] : nullptr;
    }

private:
    QVBoxLayout* layout_ = nullptr;
    QList<QWidget*> pages_;
    int currentIndex_ = 0;
};

void prepareInput(QWidget* input)
{
    input->setFixedHeight(kRowHeight);
    input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (auto* combo = qobject_cast<QComboBox*>(input)) {
        combo->setMinimumWidth(0);
        combo->setMinimumContentsLength(12);
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        combo->view()->setTextElideMode(Qt::ElideMiddle);
    }
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
        resize(640, 820);
        setMinimumSize(560, 720);

        process_ = new QProcess(this);
        process_->setProcessChannelMode(QProcess::MergedChannels);
        connect(process_, &QProcess::readyReadStandardOutput, this, [this] {
            handleProcessOutput(QString::fromLocal8Bit(process_->readAllStandardOutput()));
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
            const QString remaining = QString::fromLocal8Bit(process_->readAllStandardOutput());
            if (!remaining.isEmpty()) {
                handleProcessOutput(remaining);
            }
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

        displayProcess_ = new QProcess(this);
        displayProcess_->setProcessChannelMode(QProcess::MergedChannels);
        connect(displayProcess_, &QProcess::readyReadStandardOutput, this, [this] {
            displayOutput_ += QString::fromLocal8Bit(displayProcess_->readAllStandardOutput());
        });
        connect(displayProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            Q_UNUSED(error);
            setDisplayRefreshing(false);
        });
        connect(displayProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus status) {
            finishDisplayRefresh(code, status);
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

        inviteProcess_ = new QProcess(this);
        inviteProcess_->setProcessChannelMode(QProcess::MergedChannels);
        connect(inviteProcess_, &QProcess::readyReadStandardOutput, this, [this] {
            const QString text = QString::fromLocal8Bit(inviteProcess_->readAllStandardOutput());
            inviteOutput_ += text;
            appendOutput(text);
        });
        connect(inviteProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            Q_UNUSED(error);
            appendOutput("Invite creation error: " + inviteProcess_->errorString() + "\n");
            inviteTarget_ = InviteTarget::None;
            updateInviteButtons();
        });
        connect(inviteProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus status) {
            finishInviteGeneration(code, status);
        });

        receiverRefreshTimer_ = new QTimer(this);
        receiverRefreshTimer_->setInterval(kReceiverRefreshMs);
        connect(receiverRefreshTimer_, &QTimer::timeout, this, [this] { startDiscovery(true); });

        peerStatusTimer_ = new QTimer(this);
        peerStatusTimer_->setInterval(kPeerStatusPollMs);
        connect(peerStatusTimer_, &QTimer::timeout, this, [this] { checkPeerActivityTimeout(); });

        buildUi();
        updateInternetAdvancedVisibility();
        refreshReportPath();
        refreshCommand();
        setRunning(false);
        receiverRefreshTimer_->start();
        peerStatusTimer_->start();
        QTimer::singleShot(400, this, [this] { startDiscovery(true); });
        QTimer::singleShot(550, this, [this] { refreshDisplays(true); });
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

        auto* settingsContent = new QWidget;
        settingsContent->setObjectName("LeftHost");
        auto* settingsColumn = new QVBoxLayout(settingsContent);
        settingsColumn->setContentsMargins(0, 0, 6, 0);
        settingsColumn->setSpacing(8);
        settingsColumn->addWidget(buildOptionStack());
        settingsColumn->addWidget(buildSecurityPanel());
        settingsColumn->addStretch(1);

        auto* settingsScroll = new QScrollArea;
        settingsScroll->setObjectName("LeftScroll");
        settingsScroll->setWidget(settingsContent);
        settingsScroll->setWidgetResizable(true);
        settingsScroll->setFrameShape(QFrame::NoFrame);
        settingsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        settingsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        root->addWidget(settingsScroll, 1);

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
        titleBlock->addWidget(makeLabel("Direct screen sharing", "Subtle"));
        header->addLayout(titleBlock, 1);

        statusBadge_ = makeLabel("○  Idle", "StatusIdle");
        statusBadge_->setAlignment(Qt::AlignCenter);
        statusBadge_->setMinimumHeight(40);
        statusBadge_->setMinimumWidth(140);
        statusOpacityEffect_ = new QGraphicsOpacityEffect(statusBadge_);
        statusOpacityEffect_->setOpacity(1.0);
        statusBadge_->setGraphicsEffect(statusOpacityEffect_);
        statusPulseAnimation_ = new QPropertyAnimation(statusOpacityEffect_, "opacity", this);
        statusPulseAnimation_->setDuration(1300);
        statusPulseAnimation_->setEasingCurve(QEasingCurve::InOutSine);
        statusPulseAnimation_->setKeyValueAt(0.0, 1.0);
        statusPulseAnimation_->setKeyValueAt(0.5, 0.45);
        statusPulseAnimation_->setKeyValueAt(1.0, 1.0);
        statusPulseAnimation_->setLoopCount(-1);
        header->addWidget(statusBadge_, 0, Qt::AlignVCenter);

        darkModeCheck_ = new QCheckBox("Dark");
        darkModeCheck_->setObjectName("ThemeSwitch");
        darkModeCheck_->setChecked(true);
        connect(darkModeCheck_, &QCheckBox::toggled, this, [this](bool checked) { applyTheme(checked); });
        header->addWidget(darkModeCheck_, 0, Qt::AlignVCenter);

        actionButton_ = new QPushButton("Share");
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

        shareModeButton_ = new QPushButton("Create room");
        watchModeButton_ = new QPushButton("Join room");
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
        optionStack_ = new PageStack;
        optionStack_->setObjectName("OptionStack");
        optionStack_->addPage(buildSharePage());
        optionStack_->addPage(buildWatchPage());
        return optionStack_;
    }

    QWidget* buildSharePage()
    {
        auto* page = new QWidget;
        page->setObjectName("OptionPage");
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);

        QVBoxLayout* connectionContent = nullptr;
        layout->addWidget(makePanel("Room", &connectionContent));
        auto* methodTabs = new QFrame;
        methodTabs->setObjectName("ModeBar");
        methodTabs->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        auto* methodTabsLayout = new QHBoxLayout(methodTabs);
        methodTabsLayout->setContentsMargins(6, 6, 6, 6);
        methodTabsLayout->setSpacing(6);
        nearbyConnectionButton_ = new QPushButton("Nearby");
        internetConnectionButton_ = new QPushButton("Internet");
        manualConnectionButton_ = new QPushButton("Manual");
        auto* connectionGroup = new QButtonGroup(this);
        connectionGroup->setExclusive(true);
        const auto prepareConnectionButton = [connectionGroup, methodTabsLayout](
                                                QPushButton* button,
                                                ShareConnectionMethod method,
                                                const QString& tooltip) {
            button->setCheckable(true);
            button->setObjectName("ModeButton");
            button->setCursor(Qt::PointingHandCursor);
            button->setToolTip(tooltip);
            methodTabsLayout->addWidget(button);
            connectionGroup->addButton(button, static_cast<int>(method));
        };
        prepareConnectionButton(
            nearbyConnectionButton_,
            ShareConnectionMethod::Nearby,
            "Use LAN discovery or a Tailscale peer.");
        prepareConnectionButton(
            internetConnectionButton_,
            ShareConnectionMethod::InternetInvite,
            "Create a room invite for reachable direct paths.");
        prepareConnectionButton(
            manualConnectionButton_,
            ShareConnectionMethod::ManualAddress,
            "Type a receiver IP address and port.");
        connectionContent->addWidget(methodTabs);

        shareConnectionStack_ = new PageStack;
        shareConnectionStack_->setObjectName("OptionStack");

        auto* nearbyPage = new QWidget;
        nearbyPage->setObjectName("OptionPage");
        auto* receiversContent = new QVBoxLayout(nearbyPage);
        receiversContent->setContentsMargins(0, 0, 0, 0);
        receiversContent->setSpacing(8);
        receiverList_ = new QListWidget;
        receiverList_->setObjectName("ReceiverList");
        receiverList_->setMinimumHeight(120);
        receiverList_->setUniformItemSizes(true);
        receiverList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
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
        connect(receiverList_, &QListWidget::itemSelectionChanged, this, [this] {
            updateReceiverStatus(false);
            updateInternetStatus();
            refreshCommand();
        });

        auto* internetPage = new QWidget;
        internetPage->setObjectName("OptionPage");
        auto* internetContent = new QVBoxLayout(internetPage);
        internetContent->setContentsMargins(0, 0, 0, 0);
        internetContent->setSpacing(8);
        shareSignalRoomEdit_ = new QLineEdit(generatedRoomId());
        shareSignalRoomEdit_->setPlaceholderText("room-name");
        auto* shareRoomRow = new QWidget;
        shareRoomRow->setObjectName("FormRow");
        auto* shareRoomLayout = new QHBoxLayout(shareRoomRow);
        shareRoomLayout->setContentsMargins(0, 0, 0, 0);
        shareRoomLayout->setSpacing(8);
        newShareRoomButton_ = new QPushButton("New");
        newShareRoomButton_->setObjectName("SecondaryButton");
        newShareRoomButton_->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
        newShareRoomButton_->setIconSize(QSize(14, 14));
        newShareRoomButton_->setCursor(Qt::PointingHandCursor);
        newShareRoomButton_->setFixedHeight(kRowHeight);
        copyShareRoomButton_ = new QPushButton("Copy");
        copyShareRoomButton_->setObjectName("SecondaryButton");
        copyShareRoomButton_->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
        copyShareRoomButton_->setIconSize(QSize(14, 14));
        copyShareRoomButton_->setCursor(Qt::PointingHandCursor);
        copyShareRoomButton_->setFixedHeight(kRowHeight);
        prepareInput(shareSignalRoomEdit_);
        prepareInput(shareRoomRow);
        shareRoomLayout->addWidget(shareSignalRoomEdit_, 1);
        shareRoomLayout->addWidget(newShareRoomButton_);
        shareRoomLayout->addWidget(copyShareRoomButton_);
        addRow(internetContent, "Room", shareRoomRow);
        shareInvitePortSpin_ = new NoWheelSpinBox;
        shareInvitePortSpin_->setRange(1, 65535);
        shareInvitePortSpin_->setValue(5001);
        prepareInput(shareInvitePortSpin_);
        addRow(internetContent, "Port", shareInvitePortSpin_);
        shareLegacyInviteCheck_ = new QCheckBox("Manual invite fallback");
        shareLegacyInviteCheck_->setToolTip("Only use this if the Worker room path is unavailable.");
        addFullRow(internetContent, shareLegacyInviteCheck_);
        shareLegacyInvitePanel_ = new QWidget;
        shareLegacyInvitePanel_->setObjectName("OptionPage");
        auto* shareLegacyContent = new QVBoxLayout(shareLegacyInvitePanel_);
        shareLegacyContent->setContentsMargins(0, 0, 0, 0);
        shareLegacyContent->setSpacing(8);
        shareLocalInviteEdit_ = new QLineEdit;
        shareLocalInviteEdit_->setPlaceholderText("Create a room invite");
        auto* shareLocalInviteRow = new QWidget;
        shareLocalInviteRow->setObjectName("FormRow");
        auto* shareLocalInviteLayout = new QHBoxLayout(shareLocalInviteRow);
        shareLocalInviteLayout->setContentsMargins(0, 0, 0, 0);
        shareLocalInviteLayout->setSpacing(8);
        createShareInviteButton_ = new QPushButton("Create");
        createShareInviteButton_->setObjectName("SecondaryButton");
        createShareInviteButton_->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
        createShareInviteButton_->setIconSize(QSize(14, 14));
        createShareInviteButton_->setCursor(Qt::PointingHandCursor);
        createShareInviteButton_->setFixedHeight(kRowHeight);
        copyShareInviteButton_ = new QPushButton("Copy");
        copyShareInviteButton_->setObjectName("SecondaryButton");
        copyShareInviteButton_->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
        copyShareInviteButton_->setIconSize(QSize(14, 14));
        copyShareInviteButton_->setCursor(Qt::PointingHandCursor);
        copyShareInviteButton_->setFixedHeight(kRowHeight);
        prepareInput(shareLocalInviteEdit_);
        prepareInput(shareLocalInviteRow);
        shareLocalInviteLayout->addWidget(shareLocalInviteEdit_, 1);
        shareLocalInviteLayout->addWidget(createShareInviteButton_);
        shareLocalInviteLayout->addWidget(copyShareInviteButton_);
        addRow(shareLegacyContent, "Room invite", shareLocalInviteRow);
        auto* sharePeerInvitePanel = new QWidget;
        sharePeerInvitePanel->setObjectName("OptionPage");
        auto* sharePeerInviteLayout = new QVBoxLayout(sharePeerInvitePanel);
        sharePeerInviteLayout->setContentsMargins(0, 0, 0, 0);
        sharePeerInviteLayout->setSpacing(8);
        sharePeerInviteList_ = new QListWidget;
        sharePeerInviteList_->setObjectName("WatcherInviteList");
        sharePeerInviteList_->setMinimumHeight(92);
        sharePeerInviteList_->setUniformItemSizes(true);
        sharePeerInviteList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        sharePeerInviteList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        sharePeerInviteList_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
        sharePeerInviteLayout->addWidget(sharePeerInviteList_);
        auto* sharePeerInviteButtons = new QWidget;
        sharePeerInviteButtons->setObjectName("FormRow");
        auto* sharePeerInviteButtonLayout = new QHBoxLayout(sharePeerInviteButtons);
        sharePeerInviteButtonLayout->setContentsMargins(0, 0, 0, 0);
        sharePeerInviteButtonLayout->setSpacing(8);
        pasteSharePeerInviteButton_ = new QPushButton("Paste");
        pasteSharePeerInviteButton_->setObjectName("SecondaryButton");
        pasteSharePeerInviteButton_->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
        pasteSharePeerInviteButton_->setIconSize(QSize(14, 14));
        pasteSharePeerInviteButton_->setCursor(Qt::PointingHandCursor);
        pasteSharePeerInviteButton_->setFixedHeight(kRowHeight);
        removeSharePeerInviteButton_ = new QPushButton("Remove");
        removeSharePeerInviteButton_->setObjectName("SecondaryButton");
        removeSharePeerInviteButton_->setIcon(style()->standardIcon(QStyle::SP_DialogCancelButton));
        removeSharePeerInviteButton_->setIconSize(QSize(14, 14));
        removeSharePeerInviteButton_->setCursor(Qt::PointingHandCursor);
        removeSharePeerInviteButton_->setFixedHeight(kRowHeight);
        clearSharePeerInviteButton_ = new QPushButton("Clear");
        clearSharePeerInviteButton_->setObjectName("SecondaryButton");
        clearSharePeerInviteButton_->setIcon(style()->standardIcon(QStyle::SP_DialogDiscardButton));
        clearSharePeerInviteButton_->setIconSize(QSize(14, 14));
        clearSharePeerInviteButton_->setCursor(Qt::PointingHandCursor);
        clearSharePeerInviteButton_->setFixedHeight(kRowHeight);
        sharePeerInvitePanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
        sharePeerInviteButtonLayout->addStretch(1);
        sharePeerInviteButtonLayout->addWidget(pasteSharePeerInviteButton_);
        sharePeerInviteButtonLayout->addWidget(removeSharePeerInviteButton_);
        sharePeerInviteButtonLayout->addWidget(clearSharePeerInviteButton_);
        sharePeerInviteLayout->addWidget(sharePeerInviteButtons);
        addRow(shareLegacyContent, "Watcher invites", sharePeerInvitePanel);
        internetContent->addWidget(shareLegacyInvitePanel_);

        connect(newShareRoomButton_, &QPushButton::clicked, this, [this] {
            shareSignalRoomEdit_->setText(generatedRoomId());
        });
        connect(copyShareRoomButton_, &QPushButton::clicked, this, [this] {
            copyShareRoomLink();
        });
        connect(shareLegacyInviteCheck_, &QCheckBox::toggled, this, [this] {
            updateInternetAdvancedVisibility();
            updateInternetStatus();
            refreshCommand();
        });
        connect(createShareInviteButton_, &QPushButton::clicked, this, [this] { startInviteGeneration(InviteTarget::Share); });
        connect(copyShareInviteButton_, &QPushButton::clicked, this, [this] {
            copyInvite(shareLocalInviteEdit_, "Room");
        });
        connect(pasteSharePeerInviteButton_, &QPushButton::clicked, this, [this] {
            pasteWatcherInvitesFromClipboard();
        });
        connect(removeSharePeerInviteButton_, &QPushButton::clicked, this, [this] {
            removeSelectedWatcherInvites();
        });
        connect(clearSharePeerInviteButton_, &QPushButton::clicked, this, [this] {
            clearWatcherInvites();
        });
        connect(sharePeerInviteList_, &QListWidget::itemSelectionChanged, this, [this] {
            updateInviteButtons();
        });

        auto* manualPage = new QWidget;
        manualPage->setObjectName("OptionPage");
        auto* manualContent = new QVBoxLayout(manualPage);
        manualContent->setContentsMargins(0, 0, 0, 0);
        manualContent->setSpacing(8);
        shareHostEdit_ = new QLineEdit("127.0.0.1");
        shareHostEdit_->setPlaceholderText("192.168.1.127:5000, 192.168.1.128:5000");
        sharePortSpin_ = new NoWheelSpinBox;
        sharePortSpin_->setRange(1, 65535);
        sharePortSpin_->setValue(5000);
        prepareInput(shareHostEdit_);
        prepareInput(sharePortSpin_);
        addRow(manualContent, "Targets", shareHostEdit_);
        addRow(manualContent, "Port", sharePortSpin_);

        shareConnectionStack_->addPage(nearbyPage);
        shareConnectionStack_->addPage(internetPage);
        shareConnectionStack_->addPage(manualPage);
        connectionContent->addWidget(shareConnectionStack_);

        shareInternetStatusLabel_ = makeLabel("", "StatusHint");
        shareInternetStatusLabel_->setWordWrap(true);
        connectionContent->addWidget(shareInternetStatusLabel_);
        connect(connectionGroup, &QButtonGroup::idClicked, this, [this](int id) {
            setShareConnectionMethod(static_cast<ShareConnectionMethod>(id));
        });
        setShareConnectionMethod(ShareConnectionMethod::InternetInvite);

        QVBoxLayout* videoContent = nullptr;
        layout->addWidget(makePanel("Video", &videoContent));
        auto* displayRow = new QWidget;
        displayRow->setObjectName("FormRow");
        prepareInput(displayRow);
        auto* displayLayout = new QHBoxLayout(displayRow);
        displayLayout->setContentsMargins(0, 0, 0, 0);
        displayLayout->setSpacing(8);
        displayCombo_ = new NoWheelComboBox;
        populateDisplayChoices();
        prepareInput(displayCombo_);
        refreshDisplaysButton_ = new QPushButton("Refresh");
        refreshDisplaysButton_->setObjectName("SecondaryButton");
        refreshDisplaysButton_->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
        refreshDisplaysButton_->setIconSize(QSize(14, 14));
        refreshDisplaysButton_->setCursor(Qt::PointingHandCursor);
        refreshDisplaysButton_->setFixedHeight(kRowHeight);
        displayLayout->addWidget(displayCombo_, 1);
        displayLayout->addWidget(refreshDisplaysButton_);
        fpsSpin_ = new NoWheelSpinBox;
        fpsSpin_->setRange(15, 240);
        fpsSpin_->setValue(60);
        resolutionCombo_ = new NoWheelComboBox;
        resolutionCombo_->addItem("Native", QSize(0, 0));
        resolutionCombo_->addItem("1920 × 1080", QSize(1920, 1080));
        resolutionCombo_->addItem("1600 × 900", QSize(1600, 900));
        resolutionCombo_->addItem("1280 × 720", QSize(1280, 720));
        prepareInput(fpsSpin_);
        prepareInput(resolutionCombo_);
        addRow(videoContent, "Display", displayRow);
        addRow(videoContent, "FPS", fpsSpin_);
        addRow(videoContent, "Resolution", resolutionCombo_);
        connect(refreshDisplaysButton_, &QPushButton::clicked, this, [this] { refreshDisplays(false); });

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

        QVBoxLayout* watchConnectionContent = nullptr;
        layout->addWidget(makePanel("Room", &watchConnectionContent));
        watchPortSpin_ = new NoWheelSpinBox;
        watchPortSpin_->setRange(1, 65535);
        watchPortSpin_->setValue(5000);
        prepareInput(watchPortSpin_);
        addRow(watchConnectionContent, "Port", watchPortSpin_);

        auto* watchMethodTabs = new QFrame;
        watchMethodTabs->setObjectName("ModeBar");
        watchMethodTabs->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        auto* watchMethodTabsLayout = new QHBoxLayout(watchMethodTabs);
        watchMethodTabsLayout->setContentsMargins(6, 6, 6, 6);
        watchMethodTabsLayout->setSpacing(6);
        watchNearbyButton_ = new QPushButton("Nearby");
        watchInternetButton_ = new QPushButton("Internet");
        auto* watchConnectionGroup = new QButtonGroup(this);
        watchConnectionGroup->setExclusive(true);
        const auto prepareWatchTab = [watchConnectionGroup, watchMethodTabsLayout](
                                         QPushButton* button,
                                         WatchConnectionMethod method,
                                         const QString& tooltip) {
            button->setCheckable(true);
            button->setObjectName("ModeButton");
            button->setCursor(Qt::PointingHandCursor);
            button->setToolTip(tooltip);
            watchMethodTabsLayout->addWidget(button);
            watchConnectionGroup->addButton(button, static_cast<int>(method));
        };
        prepareWatchTab(
            watchNearbyButton_,
            WatchConnectionMethod::Nearby,
            "Listen for sharers on the same LAN or Tailscale network.");
        prepareWatchTab(
            watchInternetButton_,
            WatchConnectionMethod::InternetInvite,
            "Paste a room invite from the sharer.");
        watchConnectionContent->addWidget(watchMethodTabs);

        watchConnectionStack_ = new PageStack;
        watchConnectionStack_->setObjectName("OptionStack");

        auto* watchNearbyPage = new QWidget;
        watchNearbyPage->setObjectName("OptionPage");
        auto* watchNearbyContent = new QVBoxLayout(watchNearbyPage);
        watchNearbyContent->setContentsMargins(0, 0, 0, 0);
        watchNearbyContent->setSpacing(8);
        lanDiscoverableCheck_ = new QCheckBox("LAN discoverable");
        lanDiscoverableCheck_->setChecked(true);
        addFullRow(watchNearbyContent, lanDiscoverableCheck_);

        auto* watchInternetPage = new QWidget;
        watchInternetPage->setObjectName("OptionPage");
        auto* watchInternetContent = new QVBoxLayout(watchInternetPage);
        watchInternetContent->setContentsMargins(0, 0, 0, 0);
        watchInternetContent->setSpacing(8);
        watchSignalRoomEdit_ = new QLineEdit;
        watchSignalRoomEdit_->setPlaceholderText("room-name");
        auto* watchRoomRow = new QWidget;
        watchRoomRow->setObjectName("FormRow");
        auto* watchRoomLayout = new QHBoxLayout(watchRoomRow);
        watchRoomLayout->setContentsMargins(0, 0, 0, 0);
        watchRoomLayout->setSpacing(8);
        pasteWatchRoomLinkButton_ = new QPushButton("Paste");
        pasteWatchRoomLinkButton_->setObjectName("SecondaryButton");
        pasteWatchRoomLinkButton_->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
        pasteWatchRoomLinkButton_->setIconSize(QSize(14, 14));
        pasteWatchRoomLinkButton_->setCursor(Qt::PointingHandCursor);
        pasteWatchRoomLinkButton_->setFixedHeight(kRowHeight);
        prepareInput(watchSignalRoomEdit_);
        prepareInput(watchRoomRow);
        watchRoomLayout->addWidget(watchSignalRoomEdit_, 1);
        watchRoomLayout->addWidget(pasteWatchRoomLinkButton_);
        addRow(watchInternetContent, "Room", watchRoomRow);
        watchLegacyInviteCheck_ = new QCheckBox("Manual invite fallback");
        watchLegacyInviteCheck_->setToolTip("Only use this if the Worker room path is unavailable.");
        addFullRow(watchInternetContent, watchLegacyInviteCheck_);
        watchLegacyInvitePanel_ = new QWidget;
        watchLegacyInvitePanel_->setObjectName("OptionPage");
        auto* watchLegacyContent = new QVBoxLayout(watchLegacyInvitePanel_);
        watchLegacyContent->setContentsMargins(0, 0, 0, 0);
        watchLegacyContent->setSpacing(8);
        watchPeerInviteEdit_ = new QLineEdit;
        watchPeerInviteEdit_->setPlaceholderText("Paste room invite from sharer");
        auto* watchPeerInviteRow = new QWidget;
        watchPeerInviteRow->setObjectName("FormRow");
        auto* watchPeerInviteLayout = new QHBoxLayout(watchPeerInviteRow);
        watchPeerInviteLayout->setContentsMargins(0, 0, 0, 0);
        watchPeerInviteLayout->setSpacing(8);
        pasteWatchPeerInviteButton_ = new QPushButton("Paste");
        pasteWatchPeerInviteButton_->setObjectName("SecondaryButton");
        pasteWatchPeerInviteButton_->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
        pasteWatchPeerInviteButton_->setIconSize(QSize(14, 14));
        pasteWatchPeerInviteButton_->setCursor(Qt::PointingHandCursor);
        pasteWatchPeerInviteButton_->setFixedHeight(kRowHeight);
        prepareInput(watchPeerInviteEdit_);
        prepareInput(watchPeerInviteRow);
        watchPeerInviteLayout->addWidget(watchPeerInviteEdit_, 1);
        watchPeerInviteLayout->addWidget(pasteWatchPeerInviteButton_);
        addRow(watchLegacyContent, "Room invite", watchPeerInviteRow);
        watchLocalInviteEdit_ = new QLineEdit;
        watchLocalInviteEdit_->setPlaceholderText("Create response invite");
        auto* watchLocalInviteRow = new QWidget;
        watchLocalInviteRow->setObjectName("FormRow");
        auto* watchLocalInviteLayout = new QHBoxLayout(watchLocalInviteRow);
        watchLocalInviteLayout->setContentsMargins(0, 0, 0, 0);
        watchLocalInviteLayout->setSpacing(8);
        createWatchInviteButton_ = new QPushButton("Create");
        createWatchInviteButton_->setObjectName("SecondaryButton");
        createWatchInviteButton_->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
        createWatchInviteButton_->setIconSize(QSize(14, 14));
        createWatchInviteButton_->setCursor(Qt::PointingHandCursor);
        createWatchInviteButton_->setFixedHeight(kRowHeight);
        copyWatchInviteButton_ = new QPushButton("Copy");
        copyWatchInviteButton_->setObjectName("SecondaryButton");
        copyWatchInviteButton_->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
        copyWatchInviteButton_->setIconSize(QSize(14, 14));
        copyWatchInviteButton_->setCursor(Qt::PointingHandCursor);
        copyWatchInviteButton_->setFixedHeight(kRowHeight);
        prepareInput(watchLocalInviteEdit_);
        prepareInput(watchLocalInviteRow);
        watchLocalInviteLayout->addWidget(watchLocalInviteEdit_, 1);
        watchLocalInviteLayout->addWidget(createWatchInviteButton_);
        watchLocalInviteLayout->addWidget(copyWatchInviteButton_);
        addRow(watchLegacyContent, "My invite", watchLocalInviteRow);
        watchInternetContent->addWidget(watchLegacyInvitePanel_);

        watchConnectionStack_->addPage(watchNearbyPage);
        watchConnectionStack_->addPage(watchInternetPage);
        watchConnectionContent->addWidget(watchConnectionStack_);

        watchInternetStatusLabel_ = makeLabel("", "StatusHint");
        watchInternetStatusLabel_->setWordWrap(true);
        watchConnectionContent->addWidget(watchInternetStatusLabel_);

        connect(pasteWatchRoomLinkButton_, &QPushButton::clicked, this, [this] {
            pasteWatchRoomLink();
        });
        connect(watchLegacyInviteCheck_, &QCheckBox::toggled, this, [this] {
            updateInternetAdvancedVisibility();
            updateInternetStatus();
            refreshCommand();
        });
        connect(pasteWatchPeerInviteButton_, &QPushButton::clicked, this, [this] {
            pasteInviteFromClipboard(watchPeerInviteEdit_, "Room");
        });
        connect(createWatchInviteButton_, &QPushButton::clicked, this, [this] { startInviteGeneration(InviteTarget::Watch); });
        connect(copyWatchInviteButton_, &QPushButton::clicked, this, [this] {
            copyInvite(watchLocalInviteEdit_, "Response");
        });
        connect(watchConnectionGroup, &QButtonGroup::idClicked, this, [this](int id) {
            setWatchConnectionMethod(static_cast<WatchConnectionMethod>(id));
        });
        setWatchConnectionMethod(WatchConnectionMethod::InternetInvite);

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

        stunServerEdit_ = new QLineEdit(QString::fromUtf8(kDefaultStunServer));
        stunServerEdit_->setPlaceholderText("host:port");
        prepareInput(stunServerEdit_);
        addRow(content, "STUN", stunServerEdit_);

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
            clearLocalInvites();
        });
        connect(allowPlaintextCheck_, &QCheckBox::toggled, this, [this] { clearLocalInvites(); });
        connect(stunServerEdit_, &QLineEdit::textChanged, this, [this] { clearLocalInvites(); });
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

    void setMode(int index)
    {
        optionStack_->setCurrentIndex(index);
        shareModeButton_->setChecked(index == 0);
        watchModeButton_->setChecked(index == 1);
        refreshReportPath();
        refreshCommand();
        if (!running_) {
            updateStartButtonText();
        }
    }

    void bindCommandRefresh()
    {
        const auto bindLineEdit = [this](QLineEdit* edit) {
            if (edit == nullptr) {
                return;
            }
            connect(edit, &QLineEdit::textChanged, this, [this] { refreshCommand(); });
        };
        const auto bindSpinBox = [this](QSpinBox* spin) {
            if (spin == nullptr) {
                return;
            }
            connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this] { refreshCommand(); });
        };
        const auto bindCheckBox = [this](QCheckBox* check) {
            if (check == nullptr) {
                return;
            }
            connect(check, &QCheckBox::toggled, this, [this] { refreshCommand(); });
        };

        bindLineEdit(shareHostEdit_);
        connect(shareHostEdit_, &QLineEdit::textChanged, this, [this] { updateInternetStatus(); });
        bindLineEdit(shareSignalRoomEdit_);
        connect(shareSignalRoomEdit_, &QLineEdit::textChanged, this, [this] { updateInternetStatus(); });
        bindLineEdit(shareLocalInviteEdit_);
        connect(shareLocalInviteEdit_, &QLineEdit::textChanged, this, [this] { updateInviteButtons(); });
        bindSpinBox(shareInvitePortSpin_);
        connect(shareInvitePortSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this] { clearShareLocalInvite(); });
        bindSpinBox(sharePortSpin_);
        connect(sharePortSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int port) {
            updateTailscaleReceiverPorts(port);
            updateInternetStatus();
        });
        bindCheckBox(lanDiscoverableCheck_);
        connect(displayCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
            displayCombo_->setToolTip(displayCombo_->currentText());
            refreshCommand();
        });
        bindSpinBox(fpsSpin_);
        connect(resolutionCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { refreshCommand(); });
        connect(audioDeviceCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { refreshCommand(); });
        bindSpinBox(watchPortSpin_);
        connect(watchPortSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
            clearWatchLocalInvite();
            updateInternetStatus();
        });
        connect(lanDiscoverableCheck_, &QCheckBox::toggled, this, [this] { updateInternetStatus(); });
        bindLineEdit(watchSignalRoomEdit_);
        connect(watchSignalRoomEdit_, &QLineEdit::textChanged, this, [this] { updateInternetStatus(); });
        bindLineEdit(watchPeerInviteEdit_);
        connect(watchPeerInviteEdit_, &QLineEdit::textChanged, this, [this] { updateInternetStatus(); });
        bindLineEdit(watchLocalInviteEdit_);
        connect(watchLocalInviteEdit_, &QLineEdit::textChanged, this, [this] { updateInviteButtons(); });
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

    ShareConnectionMethod shareConnectionMethod() const
    {
        return shareConnectionMethod_;
    }

    void setShareConnectionMethod(ShareConnectionMethod method)
    {
        shareConnectionMethod_ = method;
        if (shareConnectionStack_ != nullptr) {
            shareConnectionStack_->setCurrentIndex(static_cast<int>(method));
        }
        if (nearbyConnectionButton_ != nullptr) {
            nearbyConnectionButton_->setChecked(method == ShareConnectionMethod::Nearby);
        }
        if (internetConnectionButton_ != nullptr) {
            internetConnectionButton_->setChecked(method == ShareConnectionMethod::InternetInvite);
        }
        if (manualConnectionButton_ != nullptr) {
            manualConnectionButton_->setChecked(method == ShareConnectionMethod::ManualAddress);
        }
        updateInternetStatus();
        refreshCommand();
    }

    WatchConnectionMethod watchConnectionMethod() const
    {
        return watchConnectionMethod_;
    }

    void setWatchConnectionMethod(WatchConnectionMethod method)
    {
        watchConnectionMethod_ = method;
        if (watchConnectionStack_ != nullptr) {
            watchConnectionStack_->setCurrentIndex(static_cast<int>(method));
        }
        if (watchNearbyButton_ != nullptr) {
            watchNearbyButton_->setChecked(method == WatchConnectionMethod::Nearby);
        }
        if (watchInternetButton_ != nullptr) {
            watchInternetButton_->setChecked(method == WatchConnectionMethod::InternetInvite);
        }
        updateInternetStatus();
        refreshCommand();
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

    void clearShareLocalInvite()
    {
        if (shareLocalInviteEdit_ != nullptr && !shareLocalInviteEdit_->text().isEmpty()) {
            shareLocalInviteEdit_->clear();
        }
    }

    void clearWatchLocalInvite()
    {
        if (watchLocalInviteEdit_ != nullptr && !watchLocalInviteEdit_->text().isEmpty()) {
            watchLocalInviteEdit_->clear();
        }
    }

    void clearLocalInvites()
    {
        clearShareLocalInvite();
        clearWatchLocalInvite();
    }

    void copyInvite(QLineEdit* edit, const QString& label)
    {
        if (edit == nullptr) {
            return;
        }
        const QString invite = edit->text().trimmed();
        if (invite.isEmpty()) {
            QMessageBox::information(this, "Invite", "Create an invite first.");
            return;
        }
        QApplication::clipboard()->setText(invite);
        appendOutput(label + " invite copied to clipboard\n");
    }

    bool shareUsesLegacyInvite() const
    {
        return shareLegacyInviteCheck_ != nullptr && shareLegacyInviteCheck_->isChecked();
    }

    bool watchUsesLegacyInvite() const
    {
        return watchLegacyInviteCheck_ != nullptr && watchLegacyInviteCheck_->isChecked();
    }

    QString shareSignalRoom() const
    {
        return shareSignalRoomEdit_ == nullptr ? QString() : shareSignalRoomEdit_->text().trimmed();
    }

    QString watchSignalRoom() const
    {
        return watchSignalRoomEdit_ == nullptr ? QString() : watchSignalRoomEdit_->text().trimmed();
    }

    void updateInternetAdvancedVisibility()
    {
        if (shareLegacyInvitePanel_ != nullptr) {
            shareLegacyInvitePanel_->setVisible(shareUsesLegacyInvite());
        }
        if (watchLegacyInvitePanel_ != nullptr) {
            watchLegacyInvitePanel_->setVisible(watchUsesLegacyInvite());
        }
    }

    void copyShareRoomLink()
    {
        const QString room = shareSignalRoom();
        if (!validRoomId(room)) {
            QMessageBox::information(this, "Room", "Room names need 3-96 letters, numbers, dashes, or underscores.");
            shareSignalRoomEdit_->setFocus();
            return;
        }

        QApplication::clipboard()->setText(roomLink(room, shareInvitePortSpin_->value()));
        appendOutput("Room link copied to clipboard\n");
    }

    void pasteWatchRoomLink()
    {
        QString room;
        int port = 0;
        if (!parseRoomLink(QApplication::clipboard()->text(), nullptr, &room, &port)) {
            QMessageBox::warning(this, "Room", "The clipboard does not contain a ScreenShare room link.");
            return;
        }

        watchSignalRoomEdit_->setText(room);
        watchPortSpin_->setValue(port);
        appendOutput("Room link pasted from clipboard\n");
    }

    void pasteInviteFromClipboard(QLineEdit* edit, const QString& label)
    {
        if (edit == nullptr) {
            return;
        }
        const QString invite = extractInviteLine(QApplication::clipboard()->text());
        if (invite.isEmpty()) {
            QMessageBox::warning(this, "Invite", "The clipboard does not contain a ScreenShare invite.");
            return;
        }
        edit->setText(invite);
        appendOutput(label + " invite pasted from clipboard\n");
    }

    QString watcherInviteDisplayText(int index, const QString& invite) const
    {
        QString shortened = invite;
        if (shortened.size() > 44) {
            shortened = shortened.left(24) + "..." + shortened.right(12);
        }
        return QStringLiteral("Watcher %1  %2").arg(index + 1).arg(shortened);
    }

    void refreshWatcherInviteListLabels()
    {
        if (sharePeerInviteList_ == nullptr) {
            return;
        }
        for (int index = 0; index < sharePeerInviteList_->count(); ++index) {
            auto* item = sharePeerInviteList_->item(index);
            if (item == nullptr) {
                continue;
            }
            const QString invite = item->data(Qt::UserRole).toString();
            item->setText(watcherInviteDisplayText(index, invite));
            item->setToolTip(invite);
        }
    }

    int addWatcherInvites(const QStringList& invites)
    {
        if (sharePeerInviteList_ == nullptr) {
            return 0;
        }

        int added = 0;
        for (const QString& rawInvite : invites) {
            const QString invite = normalizeInviteText(rawInvite);
            if (!looksLikeNatInvite(invite)) {
                continue;
            }
            bool exists = false;
            for (int index = 0; index < sharePeerInviteList_->count(); ++index) {
                const auto* item = sharePeerInviteList_->item(index);
                if (item != nullptr && item->data(Qt::UserRole).toString() == invite) {
                    exists = true;
                    break;
                }
            }
            if (exists) {
                continue;
            }
            auto* item = new QListWidgetItem;
            item->setData(Qt::UserRole, invite);
            sharePeerInviteList_->addItem(item);
            ++added;
        }

        if (added > 0) {
            refreshWatcherInviteListLabels();
            updateInternetStatus();
            refreshCommand();
            updateInviteButtons();
        }
        return added;
    }

    void pasteWatcherInvitesFromClipboard()
    {
        const QStringList invites = extractInviteLines(QApplication::clipboard()->text());
        if (invites.isEmpty()) {
            QMessageBox::warning(this, "Watcher invites", "The clipboard does not contain a ScreenShare invite.");
            return;
        }
        const int added = addWatcherInvites(invites);
        if (added == 0) {
            QMessageBox::information(this, "Watcher invites", "Those watcher invites are already in the list.");
            return;
        }
        appendOutput(QStringLiteral("Added %1 watcher invite%2\n")
            .arg(added)
            .arg(added == 1 ? QString() : QStringLiteral("s")));
    }

    void removeSelectedWatcherInvites()
    {
        if (sharePeerInviteList_ == nullptr) {
            return;
        }
        const QList<QListWidgetItem*> selected = sharePeerInviteList_->selectedItems();
        for (auto* item : selected) {
            delete sharePeerInviteList_->takeItem(sharePeerInviteList_->row(item));
        }
        if (!selected.isEmpty()) {
            refreshWatcherInviteListLabels();
            updateInternetStatus();
            refreshCommand();
            updateInviteButtons();
        }
    }

    void clearWatcherInvites()
    {
        if (sharePeerInviteList_ == nullptr || sharePeerInviteList_->count() == 0) {
            return;
        }
        sharePeerInviteList_->clear();
        updateInternetStatus();
        refreshCommand();
        updateInviteButtons();
    }

    bool inviteGenerating() const
    {
        return inviteProcess_ != nullptr && inviteProcess_->state() != QProcess::NotRunning;
    }

    int invitePort(InviteTarget target) const
    {
        if (target == InviteTarget::Share) {
            return shareInvitePortSpin_->value();
        }
        if (target == InviteTarget::Watch) {
            return watchPortSpin_->value();
        }
        return 0;
    }

    QString stunServer() const
    {
        QString server = stunServerEdit_ == nullptr ? QString() : stunServerEdit_->text().trimmed();
        return server.isEmpty() ? QString::fromUtf8(kDefaultStunServer) : server;
    }

    bool validateInviteSecurity()
    {
        if (!accessCodeEdit_->text().isEmpty() || allowPlaintextCheck_->isChecked()) {
            return true;
        }
        QMessageBox::warning(
            this,
            "Security choice",
            "Generate or paste an access code, or allow plaintext before creating an invite.");
        accessCodeEdit_->setFocus();
        return false;
    }

    void startInviteGeneration(InviteTarget target)
    {
        if (inviteGenerating()) {
            return;
        }
        if (process_->state() != QProcess::NotRunning) {
            QMessageBox::information(this, "Already running", "Stop the current session before creating an invite.");
            return;
        }
        if (!validateInviteSecurity()) {
            return;
        }

        const QString program = enginePath();
        if (!QFileInfo::exists(program)) {
            QMessageBox::critical(this, "Missing engine", "ScreenShare.exe was not found beside the UI executable.");
            return;
        }

        const int port = invitePort(target);
        QStringList args;
        args << "--make-invite" << QString::number(port)
             << "--stun" << stunServer();
        const QString accessCode = accessCodeEdit_->text();
        if (!accessCode.isEmpty()) {
            args << "--access-code" << accessCode;
        } else {
            args << "--allow-plaintext";
        }

        inviteTarget_ = target;
        inviteOutput_.clear();
        appendOutput("\nCreating NAT invite on UDP port " + QString::number(port) + "...\n");
        inviteProcess_->setProgram(program);
        inviteProcess_->setArguments(args);
        inviteProcess_->setWorkingDirectory(QFileInfo(program).absolutePath());
        inviteProcess_->start();
        updateInviteButtons();
    }

    void finishInviteGeneration(int code, QProcess::ExitStatus status)
    {
        const QString remaining = QString::fromLocal8Bit(inviteProcess_->readAllStandardOutput());
        if (!remaining.isEmpty()) {
            inviteOutput_ += remaining;
            appendOutput(remaining);
        }

        const InviteTarget target = inviteTarget_;
        inviteTarget_ = InviteTarget::None;
        updateInviteButtons();

        if (status != QProcess::NormalExit || code != 0) {
            appendOutput("Invite creation finished with exit code " + QString::number(code) + "\n");
            return;
        }

        const QString invite = extractInviteLine(inviteOutput_);
        if (invite.isEmpty()) {
            appendOutput("Invite creation finished, but no invite line was found\n");
            return;
        }

        if (target == InviteTarget::Share) {
            shareLocalInviteEdit_->setText(invite);
            QApplication::clipboard()->setText(invite);
            appendOutput("Room invite copied. Send it to your friend so they can Join room.\n");
        } else if (target == InviteTarget::Watch) {
            watchLocalInviteEdit_->setText(invite);
            QApplication::clipboard()->setText(invite);
            appendOutput("Response invite copied. Send it back to the sharer if the room invite cannot connect by itself.\n");
        }
        if (!accessCodeEdit_->text().isEmpty()) {
            appendOutput("Use the same access code on both computers.\n");
        }
    }

    QString endpointText(const QString& host, int port) const
    {
        return host.trimmed() + ":" + QString::number(port);
    }

    QString targetWithDefaultPort(const QString& value, int defaultPort) const
    {
        const QString target = value.trimmed();
        if (target.isEmpty() || looksLikeNatInvite(target)) {
            return target;
        }

        const int separator = target.lastIndexOf(':');
        if (separator > 0 && separator < target.size() - 1) {
            bool ok = false;
            const int port = target.mid(separator + 1).toInt(&ok);
            if (ok && port >= 1 && port <= 65535) {
                return target;
            }
            return target;
        }
        if (separator >= 0) {
            return target;
        }
        return target + ":" + QString::number(defaultPort);
    }

    QStringList manualShareTargets() const
    {
        QStringList targets;
        if (shareHostEdit_ == nullptr || sharePortSpin_ == nullptr) {
            return targets;
        }
        const QStringList values = parseExtraShareTargets(shareHostEdit_->text());
        for (const QString& value : values) {
            const QString target = targetWithDefaultPort(value, sharePortSpin_->value());
            if (!target.isEmpty()) {
                targets.push_back(target);
            }
        }
        return targets;
    }

    QVector<DiscoveredReceiver> selectedNearbyReceivers() const
    {
        QVector<DiscoveredReceiver> receivers;
        if (receiverList_ == nullptr) {
            return receivers;
        }
        for (int row = 0; row < receiverList_->count(); ++row) {
            QListWidgetItem* item = receiverList_->item(row);
            if (item == nullptr || !item->isSelected()) {
                continue;
            }
            bool ok = false;
            const int index = item->data(Qt::UserRole).toInt(&ok);
            if (ok && index >= 0 && index < discoveredReceivers_.size()) {
                receivers.push_back(discoveredReceivers_[index]);
            }
        }
        return receivers;
    }

    QStringList selectedNearbyShareTargets() const
    {
        QStringList targets;
        for (const auto& receiver : selectedNearbyReceivers()) {
            targets.push_back(endpointText(receiver.host, receiver.port));
        }
        return targets;
    }

    QStringList currentDirectShareTargets() const
    {
        if (!shareMode()) {
            return {};
        }
        if (shareConnectionMethod() == ShareConnectionMethod::Nearby) {
            return selectedNearbyShareTargets();
        }
        if (shareConnectionMethod() == ShareConnectionMethod::ManualAddress) {
            return manualShareTargets();
        }
        return {};
    }

    QStringList currentWatcherResponseInvites() const
    {
        QStringList invites;
        if (sharePeerInviteList_ == nullptr) {
            return {};
        }
        for (int index = 0; index < sharePeerInviteList_->count(); ++index) {
            const auto* item = sharePeerInviteList_->item(index);
            if (item != nullptr) {
                invites.push_back(item->data(Qt::UserRole).toString());
            }
        }
        return invites;
    }

    QStringList currentArguments() const
    {
        QStringList args;
        if (shareMode()) {
            if (shareConnectionMethod() == ShareConnectionMethod::InternetInvite) {
                if (shareUsesLegacyInvite()) {
                    const QString localInvite = shareLocalInviteEdit_->text().trimmed();
                    const QStringList watcherInvites = currentWatcherResponseInvites();
                    args << "--share" << (watcherInvites.isEmpty() ? localInvite : watcherInvites.front());
                    if (!localInvite.isEmpty()) {
                        args << "--local-invite" << localInvite;
                    }
                    for (int index = 1; index < watcherInvites.size(); ++index) {
                        args << "--share-target" << watcherInvites[index];
                    }
                } else {
                    args << "--share-room" << QString::number(shareInvitePortSpin_->value())
                         << "--signal-room" << shareSignalRoom();
                    if (!stunServer().isEmpty()) {
                        args << "--signal-stun" << stunServer();
                    }
                }
            } else {
                const QStringList targets = currentDirectShareTargets();
                if (!targets.isEmpty()) {
                    args << "--share" << targets.front();
                    for (int index = 1; index < targets.size(); ++index) {
                        args << "--share-target" << targets[index];
                    }
                }
            }
            args << "--display" << QString::number(selectedDisplayIndex());
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
            const WatchConnectionMethod watchMethod = watchConnectionMethod();
            if (watchMethod == WatchConnectionMethod::Nearby) {
                if (lanDiscoverableCheck_->isChecked()) {
                    args << "--lan-advertise";
                }
            } else {
                if (watchUsesLegacyInvite()) {
                    const QString peerInvite = watchPeerInviteEdit_->text().trimmed();
                    if (!peerInvite.isEmpty()) {
                        args << "--peer-invite" << peerInvite;
                    }
                } else {
                    args << "--signal-room" << watchSignalRoom();
                    if (!stunServer().isEmpty()) {
                        args << "--signal-stun" << stunServer();
                    }
                }
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
                args[index + 1] = "<friend invite>";
                ++index;
            } else if (args[index] == "--local-invite") {
                args[index + 1] = "<my invite>";
                ++index;
            } else if (args[index] == "--share-target-local-invite") {
                args[index + 1] = "<extra viewer local invite>";
                ++index;
            } else if (args[index] == "--share" && looksLikeNatInvite(args[index + 1])) {
                args[index + 1] = "<room invite>";
                ++index;
            } else if (args[index] == "--share-target" && looksLikeNatInvite(args[index + 1])) {
                args[index + 1] = "<watcher invite>";
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

    bool validateWorkerRoomFields(bool share)
    {
        QLineEdit* roomEdit = share ? shareSignalRoomEdit_ : watchSignalRoomEdit_;
        const QString room = roomEdit == nullptr ? QString() : roomEdit->text().trimmed();
        if (!validRoomId(room)) {
            QMessageBox::warning(this, "Room name", "Room names need 3-96 letters, numbers, dashes, or underscores.");
            if (roomEdit != nullptr) {
                roomEdit->setFocus();
            }
            return false;
        }
        return true;
    }

    void startProcess()
    {
        if (process_->state() != QProcess::NotRunning) {
            return;
        }
        if (shareMode()) {
            const ShareConnectionMethod method = shareConnectionMethod();
            if (method == ShareConnectionMethod::InternetInvite) {
                if (!shareUsesLegacyInvite()) {
                    if (!validateWorkerRoomFields(true)) {
                        return;
                    }
                } else if (shareLocalInviteEdit_->text().trimmed().isEmpty()) {
                    QMessageBox::warning(
                        this,
                        "Missing room invite",
                        "Create a room invite before starting.");
                    createShareInviteButton_->setFocus();
                    return;
                }
            } else {
                if (currentDirectShareTargets().isEmpty()) {
                    QMessageBox::warning(this, "Missing target", "Choose or enter at least one watcher before sharing.");
                    if (method == ShareConnectionMethod::ManualAddress) {
                        shareHostEdit_->setFocus();
                    }
                    return;
                }
                if (method == ShareConnectionMethod::Nearby && selectedNearbyReceivers().isEmpty()) {
                    QMessageBox::information(
                        this,
                        "Choose a device",
                        "Choose one or more nearby devices, or switch the connection method to Manual address.");
                    return;
                }
            }
        } else if (watchConnectionMethod() == WatchConnectionMethod::InternetInvite &&
                   !watchUsesLegacyInvite()) {
            if (!validateWorkerRoomFields(false)) {
                return;
            }
        } else if (watchConnectionMethod() == WatchConnectionMethod::InternetInvite &&
                   watchPeerInviteEdit_->text().trimmed().isEmpty()) {
            QMessageBox::warning(
                this,
                "Missing room invite",
                "Paste the room invite from the sharer before watching.");
            watchPeerInviteEdit_->setFocus();
            return;
        }
        if (!validateDirectShareTargets()) {
            return;
        }
        if (!confirmShareTarget()) {
            return;
        }
        if (!validateSelectedReceiverSecurity()) {
            return;
        }
        runtimeNatStatus_.clear();
        runtimeNatHint_.clear();
        processOutputParseBuffer_.clear();
        accessCodeWarningShown_ = false;
        runtimeNatShareMode_ = shareMode();
        updateInternetStatus();
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

    QVector<DisplayChoice> fallbackDisplayChoices() const
    {
        QVector<DisplayChoice> displays;
        const QList<QScreen*> screens = QApplication::screens();
        if (screens.isEmpty()) {
            displays.push_back(DisplayChoice{});
            return displays;
        }

        for (int index = 0; index < screens.size(); ++index) {
            const QRect geometry = screens[index]->geometry();
            DisplayChoice display;
            display.index = index;
            display.outputName = screens[index]->name().trimmed();
            display.width = geometry.width();
            display.height = geometry.height();
            display.left = geometry.x();
            display.top = geometry.y();
            displays.push_back(std::move(display));
        }
        return displays;
    }

    void updateDisplayList(const QVector<DisplayChoice>& displays)
    {
        if (displayCombo_ == nullptr) {
            return;
        }

        const int previousDisplay = selectedDisplayIndex();
        const bool wasBlocked = displayCombo_->blockSignals(true);
        displayCombo_->clear();

        const QVector<DisplayChoice> choices = displays.isEmpty() ? fallbackDisplayChoices() : displays;
        QScreen* primaryScreen = QApplication::primaryScreen();
        for (const auto& display : choices) {
            displayCombo_->addItem(displayChoiceText(display, primaryScreen), display.index);
            if (!display.adapterName.isEmpty()) {
                displayCombo_->setItemData(displayCombo_->count() - 1, display.adapterName, Qt::ToolTipRole);
            }
        }

        const int preferredIndex = displayCombo_->findData(previousDisplay);
        displayCombo_->setCurrentIndex(preferredIndex >= 0 ? preferredIndex : 0);
        displayCombo_->setToolTip(displayCombo_->currentText());
        displayCombo_->blockSignals(wasBlocked);
    }

    void populateDisplayChoices()
    {
        updateDisplayList(fallbackDisplayChoices());
    }

    int selectedDisplayIndex() const
    {
        if (displayCombo_ == nullptr || displayCombo_->currentIndex() < 0) {
            return 0;
        }
        return displayCombo_->currentData().toInt();
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
        if (shareMode() && shareConnectionMethod() == ShareConnectionMethod::InternetInvite) {
            return true;
        }
        if (!shareMode()) {
            return true;
        }

        bool hasLoopback = false;
        for (const QString& target : currentDirectShareTargets()) {
            const int separator = target.lastIndexOf(':');
            const QString host = separator > 0 ? target.left(separator) : target;
            if (isLoopbackHost(host)) {
                hasLoopback = true;
                break;
            }
        }
        if (!hasLoopback) {
            return true;
        }

        const auto result = QMessageBox::question(
            this,
            "Share to this computer?",
            "One of the target addresses is localhost, so Share will send to this same computer. "
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

    bool validateDirectShareTargets()
    {
        if (!shareMode() || shareConnectionMethod() != ShareConnectionMethod::ManualAddress) {
            return true;
        }

        const QStringList targets = manualShareTargets();
        for (const QString& target : targets) {
            const QString error = directShareTargetError(target);
            if (!error.isEmpty()) {
                QMessageBox::warning(this, "Manual targets", error + "\n\nProblem target: " + target);
                shareHostEdit_->setFocus();
                shareHostEdit_->selectAll();
                return false;
            }
        }
        return true;
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
        if (receiverList_ == nullptr) {
            discoveredReceivers_ = deduplicateReceivers(receivers);
            return;
        }

        QSet<QString> previouslySelectedTargets;
        for (const QString& target : selectedNearbyShareTargets()) {
            previouslySelectedTargets.insert(target);
        }
        const QString selectedHost = selectedReceiverHost_;
        const int selectedPort = selectedReceiverPort_;

        discoveredReceivers_ = deduplicateReceivers(receivers);
        receiverList_->clear();
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
            const QString target = endpointText(receiver.host, receiver.port);
            if (previouslySelectedTargets.contains(target) ||
                (previouslySelectedTargets.isEmpty() && receiver.host == selectedHost && receiver.port == selectedPort)) {
                item->setSelected(true);
            }
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
        const int selected = selectedNearbyReceivers().size();
        if (selected > 0) {
            receiverStatusLabel_->setText(QStringLiteral("%1 selected / %2 target%3")
                .arg(selected)
                .arg(count)
                .arg(count == 1 ? QString() : QStringLiteral("s")));
            return;
        }
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
        setShareConnectionMethod(ShareConnectionMethod::Nearby);
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
        updateInternetStatus();
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

    void refreshDisplays(bool automatic)
    {
        if (displayProcess_->state() != QProcess::NotRunning) {
            return;
        }

        const QString program = enginePath();
        if (!QFileInfo::exists(program)) {
            if (!automatic) {
                QMessageBox::critical(this, "Missing engine", "ScreenShare.exe was not found beside the UI executable.");
            }
            return;
        }

        displayOutput_.clear();
        if (!automatic) {
            appendOutput("\nRefreshing displays...\n");
        }
        displayProcess_->setProgram(program);
        displayProcess_->setArguments({"--list"});
        displayProcess_->setWorkingDirectory(QFileInfo(program).absolutePath());
        setDisplayRefreshing(true);
        displayProcess_->start();
    }

    void finishDisplayRefresh(int code, QProcess::ExitStatus status)
    {
        setDisplayRefreshing(false);
        if (status != QProcess::NormalExit || code != 0) {
            return;
        }

        const QVector<DisplayChoice> displays = parseDisplayChoices(displayOutput_);
        updateDisplayList(displays);
        refreshCommand();
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
        if (!shareMode()) {
            return false;
        }
        if (shareConnectionMethod() != ShareConnectionMethod::Nearby) {
            return false;
        }
        return !selectedNearbyReceivers().isEmpty();
    }

    bool selectedReceiverAccessCodeMatches(const QString& accessCode) const
    {
        return selectedReceiverAccessFingerprint_.isEmpty() ||
               selectedReceiverAccessFingerprint_ == "NONE" ||
               accessCodeFingerprintText(accessCode) == selectedReceiverAccessFingerprint_;
    }

    void clearAccessCodeForRetry(const QString& message, const QString& title = "Access code mismatch")
    {
        accessCodeEdit_->clear();
        allowPlaintextCheck_->setChecked(false);
        accessCodeEdit_->setFocus();
        QMessageBox::warning(this, title, message);
    }

    bool validateSelectedReceiverSecurity()
    {
        if (!shareMode() || shareConnectionMethod() != ShareConnectionMethod::Nearby) {
            return true;
        }
        const QVector<DiscoveredReceiver> receivers = selectedNearbyReceivers();
        if (receivers.isEmpty()) {
            return true;
        }
        const QString accessCode = accessCodeEdit_->text();
        for (const auto& receiver : receivers) {
            if (!receiver.securityKnown) {
                continue;
            }
            const QString receiverName = receiver.name.trimmed().isEmpty() ? QStringLiteral("ScreenShare") : receiver.name.trimmed();
            if (!receiver.encrypted) {
                if (!accessCode.isEmpty()) {
                    QMessageBox::warning(
                        this,
                        "Security mismatch",
                        receiverName + " is plaintext. Clear the access code or restart Watch with the same access code.");
                    return false;
                }
                continue;
            }
            if (accessCode.isEmpty()) {
                accessCodeEdit_->setFocus();
                QMessageBox::warning(
                    this,
                    "Access code required",
                    "Enter the access code for " + receiverName + ".");
                return false;
            }
            if (!receiver.accessFingerprint.isEmpty() &&
                receiver.accessFingerprint != "NONE" &&
                accessCodeFingerprintText(accessCode) != receiver.accessFingerprint) {
                clearAccessCodeForRetry("That access code does not match " + receiverName + ". Try again.");
                return false;
            }
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
            if (discovering) {
                statusBadge_->setText("◔  Finding");
                statusBadge_->setProperty("class", "StatusConnecting");
                statusBadge_->setToolTip("Scanning LAN and Tailscale targets.");
                repolish(statusBadge_);
            } else {
                applyStatusBadge();
            }
        }
    }

    void setDisplayRefreshing(bool refreshing)
    {
        if (refreshDisplaysButton_ != nullptr) {
            refreshDisplaysButton_->setEnabled(!refreshing && process_->state() == QProcess::NotRunning);
            refreshDisplaysButton_->setText(refreshing ? "Scanning..." : "Refresh");
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

    void updateInviteButtons()
    {
        updateInternetAdvancedVisibility();
        const bool creating = inviteGenerating();
        const bool running = process_ != nullptr && process_->state() != QProcess::NotRunning;
        const bool canCreate = !creating && !running;
        if (newShareRoomButton_ != nullptr) {
            newShareRoomButton_->setEnabled(!running);
        }
        if (copyShareRoomButton_ != nullptr) {
            copyShareRoomButton_->setEnabled(!running);
        }
        if (pasteWatchRoomLinkButton_ != nullptr) {
            pasteWatchRoomLinkButton_->setEnabled(!running);
        }
        if (createShareInviteButton_ != nullptr) {
            createShareInviteButton_->setEnabled(canCreate);
            createShareInviteButton_->setText(creating && inviteTarget_ == InviteTarget::Share ? "Creating..." : "Create");
        }
        if (copyShareInviteButton_ != nullptr) {
            copyShareInviteButton_->setEnabled(!creating && !shareLocalInviteEdit_->text().trimmed().isEmpty());
        }
        if (pasteSharePeerInviteButton_ != nullptr) {
            pasteSharePeerInviteButton_->setEnabled(!creating && !running);
        }
        if (removeSharePeerInviteButton_ != nullptr && sharePeerInviteList_ != nullptr) {
            removeSharePeerInviteButton_->setEnabled(
                !creating &&
                !running &&
                !sharePeerInviteList_->selectedItems().isEmpty());
        }
        if (clearSharePeerInviteButton_ != nullptr && sharePeerInviteList_ != nullptr) {
            clearSharePeerInviteButton_->setEnabled(!creating && !running && sharePeerInviteList_->count() > 0);
        }
        if (pasteWatchPeerInviteButton_ != nullptr) {
            pasteWatchPeerInviteButton_->setEnabled(!creating && !running);
        }
        if (createWatchInviteButton_ != nullptr) {
            createWatchInviteButton_->setEnabled(canCreate);
            createWatchInviteButton_->setText(creating && inviteTarget_ == InviteTarget::Watch ? "Creating..." : "Create");
        }
        if (copyWatchInviteButton_ != nullptr) {
            copyWatchInviteButton_->setEnabled(!creating && !watchLocalInviteEdit_->text().trimmed().isEmpty());
        }
        updateInternetStatus();
    }

    void updateInternetStatus()
    {
        if (shareInternetStatusLabel_ != nullptr) {
            const QString runtimeStatus = runtimeInternetStatusText(true);
            shareInternetStatusLabel_->setText(runtimeStatus.isEmpty() ?
                shareConnectionStatusText() :
                runtimeStatus);
        }
        if (watchInternetStatusLabel_ != nullptr) {
            const QString runtimeStatus = runtimeInternetStatusText(false);
            watchInternetStatusLabel_->setText(runtimeStatus.isEmpty() ?
                watchConnectionStatusText() :
                runtimeStatus);
        }
    }

    QString watchConnectionStatusText() const
    {
        switch (watchConnectionMethod()) {
        case WatchConnectionMethod::Nearby:
            if (lanDiscoverableCheck_ != nullptr && lanDiscoverableCheck_->isChecked()) {
                return "Ready to start: listening on port " +
                       QString::number(watchPortSpin_->value()) +
                       " and discoverable on the local network.";
            }
            return "Ready to start: listening on port " +
                   QString::number(watchPortSpin_->value()) +
                   ". Toggle LAN discoverable if friends should find this room automatically.";
        case WatchConnectionMethod::InternetInvite:
            if (watchUsesLegacyInvite()) {
                return watchRoomInviteStatusText(
                    watchPeerInviteEdit_ != nullptr && !watchPeerInviteEdit_->text().trimmed().isEmpty());
            }
            return watchWorkerRoomStatusText();
        }
        return {};
    }

    QString shareConnectionStatusText() const
    {
        switch (shareConnectionMethod()) {
        case ShareConnectionMethod::Nearby: {
            const int count = selectedNearbyReceivers().size();
            if (count > 0) {
                return QStringLiteral("Ready to start: sharing to %1 nearby watcher%2.")
                    .arg(count)
                    .arg(count == 1 ? QString() : QStringLiteral("s"));
            }
            return "Choose one or more nearby devices, or switch to Manual address or Internet invite.";
        }
        case ShareConnectionMethod::ManualAddress: {
            const QStringList targets = manualShareTargets();
            if (targets.isEmpty()) {
                return "Enter one or more watcher targets.";
            }
            if (targets.size() == 1) {
                return "Ready to start: sharing to " + targets.front() + ".";
            }
            return QStringLiteral("Ready to start: sharing to %1 manual watchers.").arg(targets.size());
        }
        case ShareConnectionMethod::InternetInvite:
            if (shareUsesLegacyInvite()) {
                return shareRoomInviteStatusText(
                    !shareLocalInviteEdit_->text().trimmed().isEmpty(),
                    currentWatcherResponseInvites().size());
            }
            return shareWorkerRoomStatusText();
        }
        return {};
    }

    QString shareWorkerRoomStatusText() const
    {
        if (!validRoomId(shareSignalRoom())) {
            return "Choose a room name with letters, numbers, dashes, or underscores.";
        }
        return "Ready: copy the room link, send it to your friend, then start sharing.";
    }

    QString watchWorkerRoomStatusText() const
    {
        if (watchSignalRoom().isEmpty()) {
            return "Paste the room link from the sharer, or enter the room name manually.";
        }
        if (!validRoomId(watchSignalRoom())) {
            return "Room names need letters, numbers, dashes, or underscores.";
        }
        return "Ready to watch: this room will use signaling and direct UDP.";
    }

    QString shareRoomInviteStatusText(bool hasRoomInvite, int watcherInviteCount) const
    {
        if (!hasRoomInvite) {
            return "Room setup: create a room invite and send it to your friend.";
        }
        if (watcherInviteCount <= 0) {
            return "Ready: share this room invite with each watcher. If Internet probing stalls, ask one watcher for My invite.";
        }
        return QStringLiteral("Ready: using the room invite plus %1 watcher response invite%2.")
            .arg(watcherInviteCount)
            .arg(watcherInviteCount == 1 ? QString() : QStringLiteral("s"));
    }

    QString watchRoomInviteStatusText(bool hasRoomInvite) const
    {
        if (!hasRoomInvite) {
            return "Paste the room invite from the sharer.";
        }
        if (watchLocalInviteEdit_ == nullptr || watchLocalInviteEdit_->text().trimmed().isEmpty()) {
            return "Ready to watch. For blocked NAT, create My invite and send it back to the sharer.";
        }
        return "Ready: send My invite back to the sharer, start watching, then ask them to share.";
    }

    QString runtimeInternetStatusText(bool share) const
    {
        if (running_ && share == shareMode() && peerActivitySeen_ && !peerConnected_) {
            return share ?
                "Disconnected: receiver feedback stopped." :
                "Disconnected: media stopped arriving.";
        }
        if (runtimeNatStatus_.isEmpty() ||
            runtimeNatStatus_ == "none" ||
            share != runtimeNatShareMode_) {
            return {};
        }

        const QString& status = runtimeNatStatus_;
        if (share) {
            if (status == "connected") {
                return "Live: receiver feedback connected.";
            }
            if (status == "retargeted_waiting_for_feedback") {
                return "Live: probe seen; waiting for receiver feedback.";
            }
            if (status == "probe_seen") {
                return "Live: probe seen; checking the media path.";
            }
            if (status == "probe_rejected") {
                return "Live: probe rejected. Check the access code on both sides.";
            }
            if (status == "waiting_for_probe") {
                return "Live: no watcher probe has reached this PC.";
            }
            if (status == "forced_endpoint_waiting_for_feedback") {
                return "Live: sending to the selected endpoint; waiting for feedback.";
            }
            if (status == "starting") {
                return "Live: starting sender.";
            }
        } else {
            if (status == "receiving") {
                return "Live: media is arriving.";
            }
            if (status == "media_rejected") {
                return "Live: media rejected. Check access code or plaintext mode.";
            }
            if (status == "incoming_unaccepted") {
                return "Live: packets arrived but were not accepted.";
            }
            if (status == "probe_send_errors") {
                return "Live: probe send failed. Check the pasted sender invite.";
            }
            if (status == "probing") {
                return "Live: probing the room invite; no media yet.";
            }
            if (status == "waiting_to_probe") {
                return "Live: waiting to send the first probe.";
            }
        }

        if (!runtimeNatHint_.isEmpty() && runtimeNatHint_ != "none") {
            return "Live: " + status + " (" + runtimeNatHint_ + ")";
        }
        return "Live: " + status;
    }

    void setRunning(bool running)
    {
        running_ = running;
        if (running) {
            actionButton_->setText("Stop");
            actionButton_->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
            actionButton_->setProperty("class", "Danger");
            resetPeerStatus();
        } else {
            updateStartButtonText();
            actionButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
            actionButton_->setProperty("class", "");
            resetPeerStatus();
            runtimeNatStatus_.clear();
            runtimeNatHint_.clear();
            processOutputParseBuffer_.clear();
        }
        applyStatusBadge();
        repolish(actionButton_);

        shareModeButton_->setEnabled(!running);
        watchModeButton_->setEnabled(!running);
        if (findLanButton_ != nullptr) {
            findLanButton_->setEnabled(!running && !receiverScanRunning());
        }
        if (receiverList_ != nullptr) {
            receiverList_->setEnabled(!running && !receiverScanRunning());
        }
        if (nearbyConnectionButton_ != nullptr) {
            nearbyConnectionButton_->setEnabled(!running);
        }
        if (internetConnectionButton_ != nullptr) {
            internetConnectionButton_->setEnabled(!running);
        }
        if (manualConnectionButton_ != nullptr) {
            manualConnectionButton_->setEnabled(!running);
        }
        if (shareHostEdit_ != nullptr) {
            shareHostEdit_->setEnabled(!running);
        }
        if (sharePortSpin_ != nullptr) {
            sharePortSpin_->setEnabled(!running);
        }
        if (sharePeerInviteList_ != nullptr) {
            sharePeerInviteList_->setEnabled(!running);
        }
        if (watchNearbyButton_ != nullptr) {
            watchNearbyButton_->setEnabled(!running);
        }
        if (watchInternetButton_ != nullptr) {
            watchInternetButton_->setEnabled(!running);
        }
        if (displayCombo_ != nullptr) {
            displayCombo_->setEnabled(!running);
        }
        if (refreshDisplaysButton_ != nullptr) {
            refreshDisplaysButton_->setEnabled(!running && displayProcess_->state() == QProcess::NotRunning);
        }
        if (audioDeviceCombo_ != nullptr) {
            audioDeviceCombo_->setEnabled(!running);
        }
        if (refreshAudioDevicesButton_ != nullptr) {
            refreshAudioDevicesButton_->setEnabled(!running && audioDeviceProcess_->state() == QProcess::NotRunning);
        }
        updateInviteButtons();
    }

    void updateStartButtonText()
    {
        if (actionButton_ != nullptr) {
            actionButton_->setText(shareMode() ? "Share" : "Watch");
        }
    }

    void applyStatusBadge()
    {
        if (statusBadge_ == nullptr) {
            return;
        }
        const bool running = running_;
        if (!running) {
            statusBadge_->setText("○  Idle");
            statusBadge_->setProperty("class", "StatusIdle");
            statusBadge_->setToolTip("Engine is not running.");
            if (statusPulseAnimation_ != nullptr) {
                statusPulseAnimation_->stop();
            }
            if (statusOpacityEffect_ != nullptr) {
                statusOpacityEffect_->setOpacity(1.0);
            }
        } else if (peerConnected_) {
            statusBadge_->setText("●  Live");
            statusBadge_->setProperty("class", "StatusRunning");
            statusBadge_->setToolTip(shareMode() ?
                "Receiver connected: feedback packets are arriving now." :
                "Sharer connected: media frames are arriving now.");
            if (statusPulseAnimation_ != nullptr && statusPulseAnimation_->state() != QAbstractAnimation::Running) {
                statusPulseAnimation_->start();
            }
        } else if (peerActivitySeen_) {
            statusBadge_->setText("×  Disconnected");
            statusBadge_->setProperty("class", "StatusDisconnected");
            statusBadge_->setToolTip(shareMode() ?
                "Receiver stopped responding. Keep Share running, then restart Watch or reconnect." :
                "Sharer stopped sending media. The preview clears until media arrives again.");
            if (statusPulseAnimation_ != nullptr) {
                statusPulseAnimation_->stop();
            }
            if (statusOpacityEffect_ != nullptr) {
                statusOpacityEffect_->setOpacity(1.0);
            }
        } else {
            statusBadge_->setText("◔  Connecting");
            statusBadge_->setProperty("class", "StatusConnecting");
            statusBadge_->setToolTip(shareMode() ?
                "Share is running, but receiver feedback has not arrived yet." :
                "Watch is running, but no media has arrived yet.");
            if (statusPulseAnimation_ != nullptr && statusPulseAnimation_->state() != QAbstractAnimation::Running) {
                statusPulseAnimation_->start();
            }
        }
        repolish(statusBadge_);
    }

    void handleProcessOutput(const QString& text)
    {
        appendOutput(text);
        processOutputParseBuffer_ += text;
        // Each sender stat line is roughly 2 KB. Keep multiple full lines so
        // the connection-state parser always has a complete tail to look at.
        constexpr qsizetype MaxParseBuffer = 32768;
        if (processOutputParseBuffer_.size() > MaxParseBuffer) {
            processOutputParseBuffer_.remove(0, processOutputParseBuffer_.size() - MaxParseBuffer);
        }
        handleAccessCodeProblem(processOutputParseBuffer_);
        updateRuntimeNatStatus(processOutputParseBuffer_);
        updateConnectionState(processOutputParseBuffer_);
    }

    void handleAccessCodeProblem(const QString& text)
    {
        if (accessCodeWarningShown_) {
            return;
        }

        const AccessCodeProblem problem = detectAccessCodeProblem(text);
        if (problem == AccessCodeProblem::None) {
            return;
        }

        accessCodeWarningShown_ = true;
        if (problem == AccessCodeProblem::Required) {
            clearAccessCodeForRetry(
                "This room or receiver needs an access code. Enter the same access code on both computers, or enable plaintext on both sides.",
                "Access code required");
            return;
        }

        clearAccessCodeForRetry(
            process_ != nullptr && process_->state() != QProcess::NotRunning ?
                "The room invite or incoming packets were rejected with this access code. Stop the current run, enter the same access code on both computers, then start again." :
                "The room invite or incoming packets were rejected with this access code. Enter the same access code on both computers, then start again.");
    }

    void resetPeerStatus()
    {
        peerConnected_ = false;
        peerActivitySeen_ = false;
        lastShareFeedbackPackets_ = 0;
        lastShareNatProbePackets_ = 0;
        lastWatchCompletedFrames_ = 0;
        lastWatchPayloadBytes_ = 0;
        lastWatchDecodedFrames_ = 0;
        lastWatchAudioPackets_ = 0;
    }

    void markPeerActivity()
    {
        peerActivitySeen_ = true;
        peerActivityTimer_.restart();
        if (!peerConnected_) {
            peerConnected_ = true;
            applyStatusBadge();
            updateInternetStatus();
        }
    }

    void checkPeerActivityTimeout()
    {
        if (!running_ || !peerConnected_ || !peerActivitySeen_) {
            return;
        }
        if (peerActivityTimer_.isValid() && peerActivityTimer_.elapsed() > kPeerActivityTimeoutMs) {
            peerConnected_ = false;
            applyStatusBadge();
            updateInternetStatus();
        }
    }

    void updateRuntimeNatStatus(const QString& text)
    {
        const QString status = lastLogFieldValue(text, "nat_status");
        if (status.isEmpty()) {
            return;
        }
        runtimeNatStatus_ = status;
        const QString hint = lastLogFieldValue(text, "nat_hint");
        if (!hint.isEmpty()) {
            runtimeNatHint_ = hint;
        }
        runtimeNatShareMode_ = shareMode();
        updateInternetStatus();
    }

    void updateConnectionState(const QString& text)
    {
        if (process_ == nullptr || process_->state() == QProcess::NotRunning) {
            return;
        }
        bool activeNow = false;
        if (shareMode()) {
            const bool feedbackActive = counterIncreased(text, "udp_feedback_packets", lastShareFeedbackPackets_);
            const bool probeActive = counterIncreased(text, "udp_nat_probe_packets", lastShareNatProbePackets_);
            activeNow = feedbackActive || probeActive;
        } else {
            const bool framesActive = counterIncreased(text, "completed_frames", lastWatchCompletedFrames_);
            const bool payloadActive = counterIncreased(text, "payload_bytes", lastWatchPayloadBytes_);
            const bool decodedActive = counterIncreased(text, "h264_decoded_frames", lastWatchDecodedFrames_);
            const bool audioActive = counterIncreased(text, "audio_packets", lastWatchAudioPackets_);
            activeNow = framesActive || payloadActive || decodedActive || audioActive;
        }
        if (activeNow) {
            markPeerActivity();
        } else {
            checkPeerActivityTimeout();
        }
    }

    static bool counterIncreased(const QString& text, const QString& field, qulonglong& previous)
    {
        const QString value = lastLogFieldValue(text, field);
        if (value.isEmpty()) {
            return false;
        }
        bool ok = false;
        const qulonglong current = value.toULongLong(&ok);
        if (!ok) {
            return false;
        }
        const bool increased = current > previous || (current < previous && current > 0);
        previous = current;
        return increased;
    }

    void appendOutput(const QString& text)
    {
        // The Output panel was removed from the UI. Engine stdout is still
        // captured for NAT-status parsing and saved-report logs; mirror it
        // to qDebug so developers can still tail it from a terminal build.
        if (outputEdit_ != nullptr) {
            outputEdit_->moveCursor(QTextCursor::End);
            outputEdit_->insertPlainText(text);
            outputEdit_->moveCursor(QTextCursor::End);
            return;
        }
        QString trimmed = text;
        while (trimmed.endsWith('\n') || trimmed.endsWith('\r')) {
            trimmed.chop(1);
        }
        if (!trimmed.isEmpty()) {
            qDebug().noquote() << trimmed;
        }
    }

    PageStack* optionStack_ = nullptr;
    QPushButton* shareModeButton_ = nullptr;
    QPushButton* watchModeButton_ = nullptr;
    QLabel* statusBadge_ = nullptr;
    QGraphicsOpacityEffect* statusOpacityEffect_ = nullptr;
    QPropertyAnimation* statusPulseAnimation_ = nullptr;
    QCheckBox* darkModeCheck_ = nullptr;
    QLabel* commandPreview_ = nullptr;
    QPlainTextEdit* outputEdit_ = nullptr;
    QPushButton* actionButton_ = nullptr;
    QProcess* process_ = nullptr;
    bool running_ = false;
    bool peerConnected_ = false;
    bool peerActivitySeen_ = false;
    QElapsedTimer peerActivityTimer_;
    qulonglong lastShareFeedbackPackets_ = 0;
    qulonglong lastShareNatProbePackets_ = 0;
    qulonglong lastWatchCompletedFrames_ = 0;
    qulonglong lastWatchPayloadBytes_ = 0;
    qulonglong lastWatchDecodedFrames_ = 0;
    qulonglong lastWatchAudioPackets_ = 0;
    QProcess* discoveryProcess_ = nullptr;
    QProcess* tailscaleProcess_ = nullptr;
    QProcess* displayProcess_ = nullptr;
    QProcess* audioDeviceProcess_ = nullptr;
    QProcess* inviteProcess_ = nullptr;
    QTimer* receiverRefreshTimer_ = nullptr;
    QTimer* peerStatusTimer_ = nullptr;

    QListWidget* receiverList_ = nullptr;
    QLabel* receiverStatusLabel_ = nullptr;
    QPushButton* nearbyConnectionButton_ = nullptr;
    QPushButton* internetConnectionButton_ = nullptr;
    QPushButton* manualConnectionButton_ = nullptr;
    PageStack* shareConnectionStack_ = nullptr;
    ShareConnectionMethod shareConnectionMethod_ = ShareConnectionMethod::InternetInvite;
    QLineEdit* shareSignalRoomEdit_ = nullptr;
    QPushButton* newShareRoomButton_ = nullptr;
    QPushButton* copyShareRoomButton_ = nullptr;
    QCheckBox* shareLegacyInviteCheck_ = nullptr;
    QWidget* shareLegacyInvitePanel_ = nullptr;
    QLineEdit* shareHostEdit_ = nullptr;
    QLineEdit* shareLocalInviteEdit_ = nullptr;
    QListWidget* sharePeerInviteList_ = nullptr;
    QSpinBox* shareInvitePortSpin_ = nullptr;
    QPushButton* createShareInviteButton_ = nullptr;
    QPushButton* copyShareInviteButton_ = nullptr;
    QPushButton* pasteSharePeerInviteButton_ = nullptr;
    QPushButton* removeSharePeerInviteButton_ = nullptr;
    QPushButton* clearSharePeerInviteButton_ = nullptr;
    QLabel* shareInternetStatusLabel_ = nullptr;
    QSpinBox* sharePortSpin_ = nullptr;
    QPushButton* findLanButton_ = nullptr;
    QComboBox* displayCombo_ = nullptr;
    QPushButton* refreshDisplaysButton_ = nullptr;
    QSpinBox* fpsSpin_ = nullptr;
    QComboBox* resolutionCombo_ = nullptr;
    QComboBox* audioDeviceCombo_ = nullptr;
    QPushButton* refreshAudioDevicesButton_ = nullptr;
    QLabel* audioDeviceStatusLabel_ = nullptr;
    QSpinBox* watchPortSpin_ = nullptr;
    QPushButton* watchNearbyButton_ = nullptr;
    QPushButton* watchInternetButton_ = nullptr;
    PageStack* watchConnectionStack_ = nullptr;
    WatchConnectionMethod watchConnectionMethod_ = WatchConnectionMethod::InternetInvite;
    QLineEdit* watchSignalRoomEdit_ = nullptr;
    QPushButton* pasteWatchRoomLinkButton_ = nullptr;
    QCheckBox* watchLegacyInviteCheck_ = nullptr;
    QWidget* watchLegacyInvitePanel_ = nullptr;
    QLineEdit* watchPeerInviteEdit_ = nullptr;
    QPushButton* pasteWatchPeerInviteButton_ = nullptr;
    QLineEdit* watchLocalInviteEdit_ = nullptr;
    QPushButton* createWatchInviteButton_ = nullptr;
    QPushButton* copyWatchInviteButton_ = nullptr;
    QLabel* watchInternetStatusLabel_ = nullptr;
    QCheckBox* lanDiscoverableCheck_ = nullptr;
    QCheckBox* mutedCheck_ = nullptr;
    QSpinBox* volumeSpin_ = nullptr;
    QSpinBox* previewLatencySpin_ = nullptr;

    QLineEdit* accessCodeEdit_ = nullptr;
    QPushButton* generateAccessCodeButton_ = nullptr;
    QPushButton* copyAccessCodeButton_ = nullptr;
    QCheckBox* allowPlaintextCheck_ = nullptr;
    QLineEdit* stunServerEdit_ = nullptr;
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
    QString displayOutput_;
    QString audioDeviceOutput_;
    QString inviteOutput_;
    InviteTarget inviteTarget_ = InviteTarget::None;
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
    QString runtimeNatStatus_;
    QString runtimeNatHint_;
    QString processOutputParseBuffer_;
    bool runtimeNatShareMode_ = true;
    bool accessCodeWarningShown_ = false;
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
QWidget#OptionStack {
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
QLabel[class="StatusHint"] {
    background: #10161d;
    color: #aeb9c8;
    border: 1px solid #26313d;
    border-radius: 7px;
    padding: 8px 10px;
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
QLabel[class="StatusIdle"], QLabel[class="StatusConnecting"], QLabel[class="StatusRunning"], QLabel[class="StatusDisconnected"] {
    border-radius: 20px;
    padding: 0 22px;
    font-weight: 700;
    font-size: 11pt;
    letter-spacing: 0.4px;
    min-width: 140px;
}
QLabel[class="StatusIdle"] {
    background: #1a212a;
    color: #93a0b0;
    border: 1px solid #2a3340;
}
QLabel[class="StatusConnecting"] {
    background: #2e2516;
    color: #ffc26b;
    border: 1px solid #9f7a2c;
}
QLabel[class="StatusRunning"] {
    background: #0f2e2a;
    color: #5dffd6;
    border: 1px solid #29a890;
}
QLabel[class="StatusDisconnected"] {
    background: #35191d;
    color: #ff8f9b;
    border: 1px solid #8d3741;
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
QListWidget#ReceiverList, QListWidget#WatcherInviteList {
    background: #0b0f14;
    border: 1px solid #2a3340;
    border-radius: 8px;
    padding: 4px;
    color: #e6ecf2;
}
QListWidget#ReceiverList::item, QListWidget#WatcherInviteList::item {
    border-radius: 6px;
    padding: 8px 10px;
}
QListWidget#ReceiverList::item:selected, QListWidget#WatcherInviteList::item:selected {
    background: #1a9b89;
    color: #ffffff;
}
QListWidget#ReceiverList::item:hover, QListWidget#WatcherInviteList::item:hover {
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
QWidget#OptionStack {
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
QLabel[class="StatusHint"] {
    background: #f0f4f8;
    color: #536170;
    border: 1px solid #d6dee8;
    border-radius: 7px;
    padding: 8px 10px;
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
QLabel[class="StatusIdle"], QLabel[class="StatusConnecting"], QLabel[class="StatusRunning"], QLabel[class="StatusDisconnected"] {
    border-radius: 20px;
    padding: 0 22px;
    font-weight: 700;
    font-size: 11pt;
    letter-spacing: 0.4px;
    min-width: 140px;
}
QLabel[class="StatusIdle"] {
    background: #e6eaf1;
    color: #4f5d6e;
    border: 1px solid #c8d0db;
}
QLabel[class="StatusConnecting"] {
    background: #fff3df;
    color: #8a5a16;
    border: 1px solid #d3a45a;
}
QLabel[class="StatusRunning"] {
    background: #d9f1ea;
    color: #0f6b5d;
    border: 1px solid #79c8b3;
}
QLabel[class="StatusDisconnected"] {
    background: #ffe1e5;
    color: #9b2834;
    border: 1px solid #e3959f;
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
QListWidget#ReceiverList, QListWidget#WatcherInviteList {
    background: #ffffff;
    border: 1px solid #cfd6df;
    border-radius: 8px;
    padding: 4px;
    color: #15202b;
}
QListWidget#ReceiverList::item, QListWidget#WatcherInviteList::item {
    border-radius: 6px;
    padding: 8px 10px;
}
QListWidget#ReceiverList::item:selected, QListWidget#WatcherInviteList::item:selected {
    background: #157a6e;
    color: #ffffff;
}
QListWidget#ReceiverList::item:hover, QListWidget#WatcherInviteList::item:hover {
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
    bool guiSmokeTest = false;
    const QStringList arguments = startupArguments(argc, argv);
    for (int index = 1; index < arguments.size(); ++index) {
        const QString arg = arguments[index];
        if (arg == "--gui-smoke-test") {
            guiSmokeTest = true;
            continue;
        }
        if (arg == "--self-test") {
            if (!QFileInfo::exists(enginePath(arguments.front()))) {
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
            const QString invite = extractInviteLine(QString::fromUtf8(
                "noise\n"
                "send_this_invite_to_peer=ss1e:abcDEF_123\n"));
            const QString commandInvite = extractInviteLine(QString::fromUtf8(
                ".\\ScreenShare.exe --share 'nat_invite=screenshare-invite-v1;public=1.2.3.4:5000'"));
            const QString compactCommandInvite = extractInviteLine(QString::fromUtf8(
                ".\\ScreenShare.exe --share ss1p:abcDEF_123"));
            const QString natStatus = lastLogFieldValue(QString::fromUtf8(
                "nat_status=probing nat_hint=start_share\n"
                "nat_status=receiving nat_hint=media_received\n"), "nat_status");
            const bool missingAccessCodeDetected =
                detectAccessCodeProblem("Encrypted compact NAT invite requires --access-code CODE") ==
                AccessCodeProblem::Required;
            const bool mismatchedAccessCodeDetected =
                detectAccessCodeProblem("Encrypted compact NAT invite could not be decrypted; check the access code") ==
                AccessCodeProblem::Mismatch;
            const bool rejectedPacketsDetected =
                detectAccessCodeProblem("access_rejected_datagrams=2 crypto_rejected_datagrams=0") ==
                AccessCodeProblem::Mismatch;
            const auto displays = parseDisplayChoices(QString::fromUtf8(
                "[0] \\\\.\\DISPLAY1 2560x1440 at (0,0) adapter=\"NVIDIA GeForce RTX\" attached=yes\n"
                "[1] \\\\.\\DISPLAY2 1920x1080 at (-1920,0) adapter=\"NVIDIA GeForce RTX\" attached=yes\n"));
            QString parsedRoomServer;
            QString parsedRoom;
            int parsedRoomPort = 0;
            const bool parsedRoomLink = parseRoomLink(
                "copied room: screenshare-room-v1;room=room-abc_123;port=5001",
                &parsedRoomServer,
                &parsedRoom,
                &parsedRoomPort);
            return peers.size() == 1 &&
                peers.front().host == "100.64.0.2" &&
                invite.startsWith("ss1e:") &&
                commandInvite.startsWith("nat_invite=screenshare-invite-v1") &&
                compactCommandInvite.startsWith("ss1p:") &&
                natStatus == "receiving" &&
                missingAccessCodeDetected &&
                mismatchedAccessCodeDetected &&
                rejectedPacketsDetected &&
                displays.size() == 2 &&
                displays[1].index == 1 &&
                displays[1].width == 1920 &&
                displays[1].left == -1920 &&
                parsedRoomLink &&
                parsedRoomServer == defaultSignalServer() &&
                parsedRoom == "room-abc_123" &&
                parsedRoomPort == 5001 ? 0 : 2;
        }
    }

    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    app.setStyleSheet(appStyleSheet(true));

    if (guiSmokeTest) {
        return 0;
    }

    MainWindow window;
    window.show();
    return app.exec();
}
