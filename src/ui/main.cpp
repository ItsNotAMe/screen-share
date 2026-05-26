#include "core/SessionCommand.h"
#include "core/SessionRuntimeControl.h"
#include "transport/UdpCrypto.h"
#include "ui/ActiveShareWindow.h"
#include "ui/ActiveWatchWindow.h"
#include "ui/AppShellWindow.h"
#include "ui/CreateRoomWindow.h"
#include "ui/HomeWindow.h"
#include "ui/JoinRoomWindow.h"
#include "ui/QtSessionBackend.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QByteArray>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QAbstractAnimation>
#include <QtCore/QPropertyAnimation>
#include <QtCore/QEasingCurve>
#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QIODevice>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QPoint>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QSize>
#include <QtCore/QStringList>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <QtCore/QVector>
#include <QtGui/QClipboard>
#include <QtGui/QIcon>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtGui/QScreen>
#include <QtGui/QTextCursor>
#include <QtGui/QWheelEvent>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtSvg/QSvgRenderer>
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
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

constexpr const char* kRoomLinkPrefix = "screenshare-room-v1;";
constexpr const char* kDefaultSignalServer = "https://screenshare-signaling.bit-yeet.workers.dev";
constexpr int kDisplaySizeRole = Qt::UserRole + 1;
constexpr const char* kIconNameProperty = "screenShareIconName";
constexpr const char* kIconSizeProperty = "screenShareIconSize";

std::string toStdUtf8(const QString& text)
{
    const QByteArray utf8 = text.toUtf8();
    return std::string(utf8.constData(), static_cast<size_t>(utf8.size()));
}

QStringList toQStringList(const std::vector<std::string>& values)
{
    QStringList result;
    result.reserve(static_cast<qsizetype>(values.size()));
    for (const std::string& value : values) {
        result.push_back(QString::fromStdString(value));
    }
    return result;
}

std::vector<std::string> toStdStringVector(const QStringList& values)
{
    std::vector<std::string> result;
    result.reserve(static_cast<size_t>(values.size()));
    for (const QString& value : values) {
        result.push_back(toStdUtf8(value));
    }
    return result;
}

QIcon appIcon()
{
    return QIcon(QStringLiteral(":/screenshare/brand/screenshare-mark.svg"));
}

QIcon uiIcon(const char* name)
{
    return QIcon(QStringLiteral(":/screenshare/ui/icons/%1.svg").arg(QString::fromUtf8(name)));
}

QByteArray readUiIconSvg(const QString& name)
{
    QFile file(QStringLiteral(":/screenshare/ui/icons/%1.svg").arg(name));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QPixmap renderUiIconPixmap(const QString& name, int size, const char* color, qreal devicePixelRatio)
{
    QByteArray svg = readUiIconSvg(name);
    if (svg.isEmpty()) {
        return {};
    }
    svg.replace("currentColor", color);

    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) {
        return {};
    }

    const int pixelSize = std::max(1, static_cast<int>(std::ceil(size * devicePixelRatio)));
    QPixmap pixmap(pixelSize, pixelSize);
    pixmap.setDevicePixelRatio(devicePixelRatio);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    renderer.render(&painter, QRectF(0, 0, size, size));
    return pixmap;
}

bool canRenderUiIconSvg(const QString& name, const char* color)
{
    QByteArray svg = readUiIconSvg(name);
    if (svg.isEmpty()) {
        return false;
    }
    svg.replace("currentColor", color);
    return QSvgRenderer(svg).isValid();
}

const char* iconColorForButton(const QPushButton* button, bool darkMode, QIcon::Mode mode, QIcon::State state)
{
    const QString objectName = button == nullptr ? QString() : button->objectName();
    const QString className = button == nullptr ? QString() : button->property("class").toString();
    const bool primary = objectName == QStringLiteral("PrimaryButton");
    const bool secondary = objectName == QStringLiteral("SecondaryButton");
    const bool ghost = objectName == QStringLiteral("GhostButton");
    const bool modeButton = objectName == QStringLiteral("ModeButton");
    const bool checked = state == QIcon::On || mode == QIcon::Selected;

    if (mode == QIcon::Disabled) {
        return darkMode ? "#5d6776" : "#8a95a3";
    }
    if (primary || className == QStringLiteral("Danger") || checked) {
        return "#ffffff";
    }
    if (mode == QIcon::Active) {
        return darkMode ? "#e6ecf2" : "#15202b";
    }
    if (secondary) {
        return darkMode ? "#dde4ee" : "#243140";
    }
    if (ghost || modeButton) {
        return darkMode ? "#a5b1c0" : "#5e6b7a";
    }
    return darkMode ? "#e6ecf2" : "#15202b";
}

QIcon buttonIcon(const QPushButton* button, const QString& name, int size, bool darkMode)
{
    QIcon icon;
    constexpr qreal kNormalScale = 1.0;
    constexpr qreal kHighScale = 2.0;
    for (const qreal scale : {kNormalScale, kHighScale}) {
        icon.addPixmap(
            renderUiIconPixmap(name, size, iconColorForButton(button, darkMode, QIcon::Normal, QIcon::Off), scale),
            QIcon::Normal,
            QIcon::Off);
        icon.addPixmap(
            renderUiIconPixmap(name, size, iconColorForButton(button, darkMode, QIcon::Normal, QIcon::On), scale),
            QIcon::Normal,
            QIcon::On);
        icon.addPixmap(
            renderUiIconPixmap(name, size, iconColorForButton(button, darkMode, QIcon::Active, QIcon::Off), scale),
            QIcon::Active,
            QIcon::Off);
        icon.addPixmap(
            renderUiIconPixmap(name, size, iconColorForButton(button, darkMode, QIcon::Active, QIcon::On), scale),
            QIcon::Active,
            QIcon::On);
        icon.addPixmap(
            renderUiIconPixmap(name, size, iconColorForButton(button, darkMode, QIcon::Disabled, QIcon::Off), scale),
            QIcon::Disabled,
            QIcon::Off);
        icon.addPixmap(
            renderUiIconPixmap(name, size, iconColorForButton(button, darkMode, QIcon::Disabled, QIcon::On), scale),
            QIcon::Disabled,
            QIcon::On);
        icon.addPixmap(
            renderUiIconPixmap(name, size, iconColorForButton(button, darkMode, QIcon::Selected, QIcon::Off), scale),
            QIcon::Selected,
            QIcon::Off);
        icon.addPixmap(
            renderUiIconPixmap(name, size, iconColorForButton(button, darkMode, QIcon::Selected, QIcon::On), scale),
            QIcon::Selected,
            QIcon::On);
    }
    return icon;
}

void refreshButtonIcon(QPushButton* button, bool darkMode)
{
    if (button == nullptr) {
        return;
    }
    const QString iconName = button->property(kIconNameProperty).toString();
    const int iconSize = button->property(kIconSizeProperty).toInt();
    if (iconName.isEmpty() || iconSize <= 0) {
        return;
    }
    button->setIcon(buttonIcon(button, iconName, iconSize, darkMode));
    button->setIconSize(QSize(iconSize, iconSize));
}

void setButtonIcon(QPushButton* button, const char* name, int size = 14, bool darkMode = true)
{
    if (button == nullptr) {
        return;
    }
    button->setProperty(kIconNameProperty, QString::fromUtf8(name));
    button->setProperty(kIconSizeProperty, size);
    refreshButtonIcon(button, darkMode);
    button->setIconSize(QSize(size, size));
}

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

QString defaultRoomName()
{
    const QByteArray computerName = qgetenv("COMPUTERNAME");
    if (!computerName.isEmpty()) {
        return QString::fromLocal8Bit(computerName) + "'s room";
    }
    const QByteArray userName = qgetenv("USERNAME");
    if (!userName.isEmpty()) {
        return QString::fromLocal8Bit(userName) + "'s room";
    }
    return "ScreenShare room";
}

bool validRoomId(const QString& roomId)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{3,96}$"));
    return pattern.match(roomId).hasMatch();
}

bool validOptionalRoomText(const QString& text, int maxLength)
{
    if (text.size() > maxLength) {
        return false;
    }
    for (const QChar ch : text) {
        if (ch.unicode() < 0x20 || ch.unicode() == 0x7f) {
            return false;
        }
    }
    return true;
}

bool validRoomKey(const QString& roomKey)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{8,128}$"));
    return pattern.match(roomKey).hasMatch();
}

QString roomLink(const QString& room, const QString& roomKey)
{
    QString link = QStringLiteral("%1room=%2")
        .arg(QString::fromUtf8(kRoomLinkPrefix), room.trimmed());
    if (!roomKey.trimmed().isEmpty()) {
        link += QStringLiteral(";key=%1").arg(roomKey.trimmed());
    }
    return link;
}

QString defaultSignalServer()
{
    return QString::fromUtf8(kDefaultSignalServer);
}

bool parseRoomLink(const QString& text, QString* server, QString* room, int* port, QString* roomKey = nullptr)
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
    QString parsedRoomKey;
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
        } else if (key == "key") {
            parsedRoomKey = value;
        } else if (key == "port") {
            bool ok = false;
            const int valuePort = value.toInt(&ok);
            if (ok && valuePort >= 1 && valuePort <= 65535) {
                parsedPort = valuePort;
            }
        }
    }

    if (!validRoomId(parsedRoom)) {
        return false;
    }
    if (!parsedRoomKey.isEmpty() && !validRoomKey(parsedRoomKey)) {
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
    if (roomKey != nullptr) {
        *roomKey = parsedRoomKey;
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

DisplayChoice displayChoiceFromSessionDisplay(const screenshare::SessionDisplayInfo& info)
{
    DisplayChoice display;
    display.index = info.index;
    display.outputName = QString::fromStdString(info.outputName).trimmed();
    display.width = info.width;
    display.height = info.height;
    display.left = info.left;
    display.top = info.top;
    display.adapterName = QString::fromStdString(info.adapterName).trimmed();
    display.attached = info.attached;
    return display;
}

QVector<DisplayChoice> displayChoicesFromSessionDisplays(const std::vector<screenshare::SessionDisplayInfo>& infos)
{
    QVector<DisplayChoice> displays;
    displays.reserve(static_cast<qsizetype>(infos.size()));
    for (const auto& info : infos) {
        displays.push_back(displayChoiceFromSessionDisplay(info));
    }
    return displays;
}

bool validResolutionSize(const QSize& size)
{
    return size.width() > 0 && size.height() > 0;
}

QSize evenResolutionSize(QSize size)
{
    if (!validResolutionSize(size)) {
        return {};
    }
    size.setWidth(size.width() - (size.width() % 2));
    size.setHeight(size.height() - (size.height() % 2));
    return validResolutionSize(size) ? size : QSize();
}

QString resolutionChoiceValue(const QSize& size)
{
    return QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
}

QString resolutionChoiceText(const QSize& size)
{
    return QStringLiteral("%1 × %2").arg(size.width()).arg(size.height());
}

QVector<QSize> resolutionChoicesForDisplay(QSize displaySize)
{
    displaySize = evenResolutionSize(displaySize);
    const bool hasDisplaySize = validResolutionSize(displaySize);
    const double displayAspect = hasDisplaySize ?
        static_cast<double>(displaySize.width()) / static_cast<double>(displaySize.height()) :
        16.0 / 9.0;

    QVector<QSize> choices;
    auto addChoice = [&](QSize size) {
        size = evenResolutionSize(size);
        if (!validResolutionSize(size)) {
            return;
        }
        if (hasDisplaySize && (size.width() > displaySize.width() || size.height() > displaySize.height())) {
            return;
        }
        const auto duplicate = std::find_if(choices.begin(), choices.end(), [&](const QSize& existing) {
            return existing.width() == size.width() && existing.height() == size.height();
        });
        if (duplicate == choices.end()) {
            choices.push_back(size);
        }
    };

    addChoice(displaySize);

    const QVector<QSize> common16x9 = {
        QSize(3840, 2160),
        QSize(2560, 1440),
        QSize(1920, 1080),
        QSize(1600, 900),
        QSize(1280, 720),
    };
    for (const QSize& size : common16x9) {
        const double aspect = static_cast<double>(size.width()) / static_cast<double>(size.height());
        if (!hasDisplaySize || std::abs(aspect - displayAspect) < 0.02) {
            addChoice(size);
        }
    }

    if (hasDisplaySize && choices.size() < 3) {
        for (const double scale : {0.75, 0.625, 0.5}) {
            addChoice(QSize(
                static_cast<int>(std::round(static_cast<double>(displaySize.width()) * scale)),
                static_cast<int>(std::round(static_cast<double>(displaySize.height()) * scale))));
        }
    }

    if (choices.isEmpty()) {
        for (const QSize& size : common16x9) {
            addChoice(size);
        }
    }

    std::sort(choices.begin(), choices.end(), [](const QSize& lhs, const QSize& rhs) {
        const qint64 lhsArea = static_cast<qint64>(lhs.width()) * static_cast<qint64>(lhs.height());
        const qint64 rhsArea = static_cast<qint64>(rhs.width()) * static_cast<qint64>(rhs.height());
        if (lhsArea != rhsArea) {
            return lhsArea > rhsArea;
        }
        return lhs.width() > rhs.width();
    });
    return choices;
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
constexpr int kRoomDirectoryRefreshMs = 10000;
constexpr int kPeerStatusPollMs = 500;
constexpr int kWatchPeerActivityTimeoutMs = 3000;
constexpr int kSharePeerActivityTimeoutMs = 5000;
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

struct ActiveRoom {
    QString roomId;
    QString name;
    int peerCount = 0;
    qint64 updatedAt = 0;
    qint64 expiresAt = 0;
    bool passwordProtected = false;
};

struct AudioOutputDevice {
    QString name;
    QString id;
    bool isDefault = false;
};

QVector<AudioOutputDevice> audioOutputDevicesFromSessionDevices(
    const std::vector<screenshare::SessionAudioDeviceInfo>& infos)
{
    QVector<AudioOutputDevice> devices;
    devices.reserve(static_cast<qsizetype>(infos.size()));
    for (const auto& info : infos) {
        if (info.source != screenshare::SessionAudioDeviceSource::SystemOutput) {
            continue;
        }
        AudioOutputDevice device;
        device.name = QString::fromStdString(info.name).trimmed();
        device.id = QString::fromStdString(info.id).trimmed();
        device.isDefault = info.isDefault;
        if (!device.id.isEmpty()) {
            devices.push_back(std::move(device));
        }
    }
    return devices;
}

QVector<ActiveRoom> parseActiveRooms(const QByteArray& payload)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }

    const QJsonArray rooms = document.object().value("rooms").toArray();
    QVector<ActiveRoom> activeRooms;
    activeRooms.reserve(rooms.size());
    for (const QJsonValue& value : rooms) {
        const QJsonObject object = value.toObject();
        ActiveRoom room;
        room.roomId = object.value("roomId").toString().trimmed();
        room.name = object.value("name").toString().trimmed();
        if (room.name.size() > 80 || !validOptionalRoomText(room.name, 80)) {
            room.name.clear();
        }
        room.peerCount = object.value("peerCount").toInt();
        room.updatedAt = static_cast<qint64>(object.value("updatedAt").toDouble());
        room.expiresAt = static_cast<qint64>(object.value("expiresAt").toDouble());
        room.passwordProtected = object.value("passwordProtected").toBool(object.value("requiresRoomKey").toBool(false));
        if (validRoomId(room.roomId) && room.peerCount > 0 && room.updatedAt > 0) {
            activeRooms.push_back(std::move(room));
        }
    }
    std::sort(activeRooms.begin(), activeRooms.end(), [](const ActiveRoom& lhs, const ActiveRoom& rhs) {
        if (lhs.updatedAt != rhs.updatedAt) {
            return lhs.updatedAt > rhs.updatedAt;
        }
        return lhs.roomId < rhs.roomId;
    });
    return activeRooms;
}

QString formatRoomAge(qint64 updatedAt)
{
    if (updatedAt <= 0) {
        return "unknown";
    }
    const qint64 ageMs = std::max<qint64>(0, QDateTime::currentMSecsSinceEpoch() - updatedAt);
    const qint64 seconds = ageMs / 1000;
    if (seconds < 5) {
        return "just now";
    }
    if (seconds < 60) {
        return QString::number(seconds) + "s ago";
    }
    const qint64 minutes = seconds / 60;
    if (minutes < 60) {
        return QString::number(minutes) + "m ago";
    }
    return QString::number(minutes / 60) + "h ago";
}

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
        setWindowIcon(appIcon());
        resize(640, 820);
        setMinimumSize(560, 720);

        sessionBackend_ = new QtSessionBackend(this);
        sessionBackend_->setOutputHandler([this](const QString& text) {
            handleSessionOutput(text);
        });
        sessionBackend_->setMessageHandler([this](const QString& text) {
            appendOutput(text);
        });
        sessionBackend_->setStartedHandler([this] {
            appendOutput("Started native engine\n");
            setRunning(true);
        });
        sessionBackend_->setErrorHandler([this](const QString& errorString) {
            appendOutput("Session error: " + errorString + "\n");
        });
        sessionBackend_->setFinishedHandler([this](const QtSessionBackend::FinishInfo& info) {
            if (!info.remainingOutput.isEmpty()) {
                handleSessionOutput(info.remainingOutput);
            }
            if (info.stopRequested) {
                appendOutput("Session stopped cleanly\n");
            } else if (info.failed) {
                appendOutput("Session failed with exit code " + QString::number(info.exitCode) + "\n");
            } else {
                appendOutput("Session finished with exit code " + QString::number(info.exitCode) + "\n");
            }
            setRunning(false);
        });
        sessionBackend_->setStatusHandler([this](const screenshare::SessionEvent& event) {
            handleSessionStatus(event);
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

        roomDirectoryNetwork_ = new QNetworkAccessManager(this);

        receiverRefreshTimer_ = new QTimer(this);
        receiverRefreshTimer_->setInterval(kReceiverRefreshMs);
        connect(receiverRefreshTimer_, &QTimer::timeout, this, [this] { startDiscovery(true); });

        roomDirectoryRefreshTimer_ = new QTimer(this);
        roomDirectoryRefreshTimer_->setInterval(kRoomDirectoryRefreshMs);
        connect(roomDirectoryRefreshTimer_, &QTimer::timeout, this, [this] {
            if (shouldRefreshRoomDirectory()) {
                startRoomDirectoryRefresh(true);
            }
        });

        peerStatusTimer_ = new QTimer(this);
        peerStatusTimer_->setInterval(kPeerStatusPollMs);
        connect(peerStatusTimer_, &QTimer::timeout, this, [this] { checkPeerActivityTimeout(); });

        buildUi();
        updateInternetAdvancedVisibility();
        updateSecurityVisibility();
        refreshReportPath();
        refreshCommand();
        setRunning(false);
        receiverRefreshTimer_->start();
        roomDirectoryRefreshTimer_->start();
        peerStatusTimer_->start();
        QTimer::singleShot(400, this, [this] { startDiscovery(true); });
        QTimer::singleShot(500, this, [this] { startRoomDirectoryRefresh(true); });
        QTimer::singleShot(550, this, [this] { refreshDisplays(true); });
        QTimer::singleShot(700, this, [this] { refreshAudioDevices(true); });
    }

    void showCreateRoom()
    {
        setMode(0);
        show();
        raise();
        activateWindow();
    }

    void showJoinRoom()
    {
        setMode(1);
        show();
        raise();
        activateWindow();
    }

private:
    struct ShareViewerStatus {
        int group = -1;
        QString endpoint;
        QString engineState;
        QString health;
        QString session;
        qulonglong feedbackPackets = 0;
        qulonglong completedFrames = 0;
        qulonglong resyncs = 0;
        qulonglong pendingDatagrams = 0;
        qulonglong queueMs = 0;
        bool everFeedback = false;
        qint64 lastActiveMs = -1;
    };

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
        setButtonIcon(actionButton_, "share", 16);
        actionButton_->setCursor(Qt::PointingHandCursor);
        actionButton_->setMinimumHeight(40);
        actionButton_->setMinimumWidth(140);
        connect(actionButton_, &QPushButton::clicked, this, [this] { toggleSession(); });
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
        setButtonIcon(shareModeButton_, "share", 16);
        setButtonIcon(watchModeButton_, "watch", 16);
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
        setButtonIcon(findLanButton_, "refresh");
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
        setButtonIcon(newShareRoomButton_, "room");
        newShareRoomButton_->setCursor(Qt::PointingHandCursor);
        newShareRoomButton_->setFixedHeight(kRowHeight);
        copyShareRoomButton_ = new QPushButton("Copy");
        copyShareRoomButton_->setObjectName("SecondaryButton");
        setButtonIcon(copyShareRoomButton_, "copy");
        copyShareRoomButton_->setCursor(Qt::PointingHandCursor);
        copyShareRoomButton_->setFixedHeight(kRowHeight);
        prepareInput(shareSignalRoomEdit_);
        prepareInput(shareRoomRow);
        shareRoomLayout->addWidget(shareSignalRoomEdit_, 1);
        shareRoomLayout->addWidget(newShareRoomButton_);
        shareRoomLayout->addWidget(copyShareRoomButton_);
        addRow(internetContent, "Room ID", shareRoomRow);
        shareRoomNameEdit_ = new QLineEdit(defaultRoomName());
        shareRoomNameEdit_->setPlaceholderText("Friendly room name");
        prepareInput(shareRoomNameEdit_);
        addRow(internetContent, "Name", shareRoomNameEdit_);
        shareRoomPasswordEdit_ = new QLineEdit;
        shareRoomPasswordEdit_->setPlaceholderText("Optional");
        shareRoomPasswordEdit_->setEchoMode(QLineEdit::Password);
        prepareInput(shareRoomPasswordEdit_);
        addRow(internetContent, "Password", shareRoomPasswordEdit_);
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
        setButtonIcon(createShareInviteButton_, "room");
        createShareInviteButton_->setCursor(Qt::PointingHandCursor);
        createShareInviteButton_->setFixedHeight(kRowHeight);
        copyShareInviteButton_ = new QPushButton("Copy");
        copyShareInviteButton_->setObjectName("SecondaryButton");
        setButtonIcon(copyShareInviteButton_, "copy");
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
        setButtonIcon(pasteSharePeerInviteButton_, "paste");
        pasteSharePeerInviteButton_->setCursor(Qt::PointingHandCursor);
        pasteSharePeerInviteButton_->setFixedHeight(kRowHeight);
        removeSharePeerInviteButton_ = new QPushButton("Remove");
        removeSharePeerInviteButton_->setObjectName("SecondaryButton");
        setButtonIcon(removeSharePeerInviteButton_, "remove");
        removeSharePeerInviteButton_->setCursor(Qt::PointingHandCursor);
        removeSharePeerInviteButton_->setFixedHeight(kRowHeight);
        clearSharePeerInviteButton_ = new QPushButton("Clear");
        clearSharePeerInviteButton_->setObjectName("SecondaryButton");
        setButtonIcon(clearSharePeerInviteButton_, "remove");
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
            updateSecurityVisibility();
            refreshCommand();
        });
        connect(copyShareRoomButton_, &QPushButton::clicked, this, [this] {
            copyShareRoomLink();
        });
        connect(shareLegacyInviteCheck_, &QCheckBox::toggled, this, [this] {
            updateInternetAdvancedVisibility();
            updateSecurityVisibility();
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
        shareViewerStatusLabel_ = makeLabel("", "StatusHint");
        shareViewerStatusLabel_->setWordWrap(true);
        shareViewerStatusLabel_->setVisible(false);
        connectionContent->addWidget(shareViewerStatusLabel_);
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
        setButtonIcon(refreshDisplaysButton_, "refresh");
        refreshDisplaysButton_->setCursor(Qt::PointingHandCursor);
        refreshDisplaysButton_->setFixedHeight(kRowHeight);
        displayLayout->addWidget(displayCombo_, 1);
        displayLayout->addWidget(refreshDisplaysButton_);
        fpsSpin_ = new NoWheelSpinBox;
        fpsSpin_->setRange(15, 240);
        fpsSpin_->setValue(60);
        resolutionCombo_ = new NoWheelComboBox;
        populateResolutionChoices();
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
        setButtonIcon(refreshAudioDevicesButton_, "refresh");
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
        watchSignalRoomEdit_->setPlaceholderText("Paste room link");
        auto* watchRoomRow = new QWidget;
        watchRoomRow->setObjectName("FormRow");
        auto* watchRoomLayout = new QHBoxLayout(watchRoomRow);
        watchRoomLayout->setContentsMargins(0, 0, 0, 0);
        watchRoomLayout->setSpacing(8);
        pasteWatchRoomLinkButton_ = new QPushButton("Paste");
        pasteWatchRoomLinkButton_->setObjectName("SecondaryButton");
        setButtonIcon(pasteWatchRoomLinkButton_, "paste");
        pasteWatchRoomLinkButton_->setCursor(Qt::PointingHandCursor);
        pasteWatchRoomLinkButton_->setFixedHeight(kRowHeight);
        prepareInput(watchSignalRoomEdit_);
        prepareInput(watchRoomRow);
        watchRoomLayout->addWidget(watchSignalRoomEdit_, 1);
        watchRoomLayout->addWidget(pasteWatchRoomLinkButton_);
        addRow(watchInternetContent, "Room", watchRoomRow);
        watchRoomPasswordEdit_ = new QLineEdit;
        watchRoomPasswordEdit_->setPlaceholderText("Only for locked rooms");
        watchRoomPasswordEdit_->setEchoMode(QLineEdit::Password);
        prepareInput(watchRoomPasswordEdit_);
        addRow(watchInternetContent, "Password", watchRoomPasswordEdit_);
        activeRoomList_ = new QListWidget;
        activeRoomList_->setObjectName("ActiveRoomList");
        activeRoomList_->setMinimumHeight(108);
        activeRoomList_->setUniformItemSizes(true);
        activeRoomList_->setSelectionMode(QAbstractItemView::SingleSelection);
        activeRoomList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        activeRoomList_->setToolTip("Pick an active room to join. Rooms use automatic UDP encryption.");
        watchInternetContent->addWidget(activeRoomList_);
        auto* activeRoomsFooter = new QWidget;
        activeRoomsFooter->setObjectName("FormRow");
        auto* activeRoomsFooterLayout = new QHBoxLayout(activeRoomsFooter);
        activeRoomsFooterLayout->setContentsMargins(0, 0, 0, 0);
        activeRoomsFooterLayout->setSpacing(8);
        activeRoomStatusLabel_ = makeLabel("Rooms update automatically", "Subtle");
        activeRoomsFooterLayout->addWidget(activeRoomStatusLabel_, 1);
        refreshRoomsButton_ = new QPushButton("Refresh");
        refreshRoomsButton_->setObjectName("SecondaryButton");
        setButtonIcon(refreshRoomsButton_, "refresh");
        refreshRoomsButton_->setCursor(Qt::PointingHandCursor);
        refreshRoomsButton_->setFixedHeight(kRowHeight);
        activeRoomsFooterLayout->addWidget(refreshRoomsButton_);
        watchInternetContent->addWidget(activeRoomsFooter);
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
        setButtonIcon(pasteWatchPeerInviteButton_, "paste");
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
        setButtonIcon(createWatchInviteButton_, "room");
        createWatchInviteButton_->setCursor(Qt::PointingHandCursor);
        createWatchInviteButton_->setFixedHeight(kRowHeight);
        copyWatchInviteButton_ = new QPushButton("Copy");
        copyWatchInviteButton_->setObjectName("SecondaryButton");
        setButtonIcon(copyWatchInviteButton_, "copy");
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
        connect(refreshRoomsButton_, &QPushButton::clicked, this, [this] {
            startRoomDirectoryRefresh(false);
        });
        connect(activeRoomList_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
            selectActiveRoomItem(item);
        });
        connect(watchLegacyInviteCheck_, &QCheckBox::toggled, this, [this] {
            updateInternetAdvancedVisibility();
            updateSecurityVisibility();
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
        setButtonIcon(generateAccessCodeButton_, "lock");
        generateAccessCodeButton_->setCursor(Qt::PointingHandCursor);
        generateAccessCodeButton_->setFixedHeight(kRowHeight);
        copyAccessCodeButton_ = new QPushButton("Copy");
        copyAccessCodeButton_->setObjectName("SecondaryButton");
        setButtonIcon(copyAccessCodeButton_, "copy");
        copyAccessCodeButton_->setCursor(Qt::PointingHandCursor);
        copyAccessCodeButton_->setFixedHeight(kRowHeight);
        accessLayout->addWidget(accessCodeEdit_, 1);
        accessLayout->addWidget(generateAccessCodeButton_);
        accessLayout->addWidget(copyAccessCodeButton_);
        accessCodeRow_ = new QWidget;
        auto* accessOuterLayout = new QHBoxLayout(accessCodeRow_);
        accessOuterLayout->setContentsMargins(0, 0, 0, 0);
        accessOuterLayout->setSpacing(14);
        auto* accessLabel = makeLabel("Access code", "FieldLabel");
        accessLabel->setFixedWidth(kLabelWidth);
        accessOuterLayout->addWidget(accessLabel);
        accessOuterLayout->addWidget(accessRow, 1);
        content->addWidget(accessCodeRow_);

        roomKeyStatusLabel_ = makeLabel("Encrypted room links include an automatic key.");
        roomKeyStatusLabel_->setMinimumHeight(kRowHeight);
        roomKeyStatusLabel_->setWordWrap(true);
        content->addWidget(roomKeyStatusLabel_);

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
        setButtonIcon(browseReportButton_, "report");
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
        updateSecurityVisibility();
        updateInternetStatus();
        refreshCommand();
        if (!running_) {
            updateStartButtonText();
        }
        if (shouldRefreshRoomDirectory()) {
            startRoomDirectoryRefresh(true);
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
        bindLineEdit(shareRoomNameEdit_);
        connect(shareRoomNameEdit_, &QLineEdit::textChanged, this, [this] { updateInternetStatus(); });
        bindLineEdit(shareRoomPasswordEdit_);
        connect(shareRoomPasswordEdit_, &QLineEdit::textChanged, this, [this] {
            updateSecurityVisibility();
            updateInternetStatus();
        });
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
            populateResolutionChoices();
            refreshCommand();
        });
        bindSpinBox(fpsSpin_);
        connect(resolutionCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
            resolutionCombo_->setToolTip(resolutionCombo_->currentText());
            refreshCommand();
            applyRuntimeStreamSettings();
        });
        connect(audioDeviceCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { refreshCommand(); });
        bindSpinBox(watchPortSpin_);
        connect(watchPortSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
            clearWatchLocalInvite();
            updateInternetStatus();
        });
        connect(lanDiscoverableCheck_, &QCheckBox::toggled, this, [this] { updateInternetStatus(); });
        bindLineEdit(watchSignalRoomEdit_);
        connect(watchSignalRoomEdit_, &QLineEdit::textChanged, this, [this] { updateInternetStatus(); });
        connect(watchSignalRoomEdit_, &QLineEdit::textEdited, this, [this](const QString& text) {
            QString room;
            QString roomKey;
            int port = 0;
            if (parseRoomLink(text, nullptr, &room, &port, &roomKey)) {
                watchRoomKey_ = roomKey;
                watchSignalRoomEdit_->setText(room);
                updateSecurityVisibility();
                refreshCommand();
                return;
            }
            watchRoomKey_.clear();
            updateSecurityVisibility();
            refreshCommand();
        });
        bindLineEdit(watchRoomPasswordEdit_);
        connect(watchRoomPasswordEdit_, &QLineEdit::textChanged, this, [this] {
            updateSecurityVisibility();
            updateInternetStatus();
        });
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
        updateSecurityVisibility();
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
        updateSecurityVisibility();
        updateInternetStatus();
        refreshCommand();
        if (shouldRefreshRoomDirectory()) {
            startRoomDirectoryRefresh(true);
        }
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

    QString activeRoomDisplayText(const ActiveRoom& room) const
    {
        const QString title = room.name.isEmpty()
            ? room.roomId
            : QStringLiteral("%1  (%2)").arg(room.name, room.roomId);
        return QStringLiteral("%1    %2    %3 peer%4    %5")
            .arg(title)
            .arg(room.passwordProtected ? QStringLiteral("Locked") : QStringLiteral("Open"))
            .arg(room.peerCount)
            .arg(room.peerCount == 1 ? QString() : QStringLiteral("s"))
            .arg(formatRoomAge(room.updatedAt));
    }

    QString roomDirectoryUrl() const
    {
        QString base = defaultSignalServer();
        while (base.endsWith('/')) {
            base.chop(1);
        }
        return base + "/rooms?limit=100";
    }

    bool shouldRefreshRoomDirectory() const
    {
        return optionStack_ != nullptr &&
               !running_ &&
               !shareMode() &&
               watchConnectionMethod() == WatchConnectionMethod::InternetInvite &&
               !watchUsesLegacyInvite();
    }

    void setRoomDirectoryRefreshing(bool refreshing)
    {
        roomDirectoryRefreshing_ = refreshing;
        const bool enabled = !refreshing && !running_;
        if (refreshRoomsButton_ != nullptr) {
            refreshRoomsButton_->setEnabled(enabled);
            refreshRoomsButton_->setText(refreshing ? "Loading..." : "Refresh");
        }
        if (activeRoomList_ != nullptr) {
            activeRoomList_->setEnabled(enabled);
        }
        if (refreshing && activeRoomStatusLabel_ != nullptr) {
            activeRoomStatusLabel_->setText("Loading active rooms");
        }
    }

    void startRoomDirectoryRefresh(bool quiet)
    {
        if (roomDirectoryNetwork_ == nullptr || roomDirectoryRefreshing_) {
            return;
        }

        roomDirectoryRefreshQuiet_ = quiet;
        setRoomDirectoryRefreshing(true);
        QNetworkRequest request{QUrl(roomDirectoryUrl())};
        request.setHeader(QNetworkRequest::UserAgentHeader, "ScreenShareUi");
        request.setRawHeader("Accept", "application/json");
        roomDirectoryReply_ = roomDirectoryNetwork_->get(request);
        connect(roomDirectoryReply_, &QNetworkReply::finished, this, [this] {
            finishRoomDirectoryRefresh();
        });
    }

    void finishRoomDirectoryRefresh()
    {
        QNetworkReply* reply = roomDirectoryReply_;
        roomDirectoryReply_ = nullptr;
        if (reply == nullptr) {
            setRoomDirectoryRefreshing(false);
            return;
        }

        const QNetworkReply::NetworkError error = reply->error();
        const QString errorText = reply->errorString();
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        setRoomDirectoryRefreshing(false);

        if (error != QNetworkReply::NoError) {
            if (activeRoomStatusLabel_ != nullptr) {
                activeRoomStatusLabel_->setText("Room list unavailable");
            }
            if (!roomDirectoryRefreshQuiet_) {
                QMessageBox::warning(this, "Rooms", "Could not load active rooms: " + errorText);
            }
            return;
        }

        activeRooms_ = parseActiveRooms(body);
        updateActiveRoomList();
    }

    void updateActiveRoomList()
    {
        if (activeRoomList_ == nullptr) {
            return;
        }

        const QString selectedRoom = watchSignalRoom();
        activeRoomList_->clear();
        for (int index = 0; index < activeRooms_.size(); ++index) {
            const ActiveRoom& room = activeRooms_[index];
            auto* item = new QListWidgetItem(activeRoomDisplayText(room));
            item->setData(Qt::UserRole, index);
            item->setToolTip(QStringLiteral("%1%2\n%3 peer%4\n%5")
                .arg(room.roomId)
                .arg(room.name.isEmpty() ? QString() : QStringLiteral("\n%1").arg(room.name))
                .arg(room.peerCount)
                .arg(room.peerCount == 1 ? QString() : QStringLiteral("s"))
                .arg(room.passwordProtected
                    ? QStringLiteral("Password required; UDP encryption stays automatic")
                    : QStringLiteral("Automatic UDP encryption")));
            activeRoomList_->addItem(item);
            if (room.roomId == selectedRoom) {
                item->setSelected(true);
            }
        }
        updateActiveRoomStatus();
    }

    void updateActiveRoomStatus()
    {
        if (activeRoomStatusLabel_ == nullptr || roomDirectoryRefreshing_) {
            return;
        }
        if (activeRooms_.isEmpty()) {
            activeRoomStatusLabel_->setText("No active rooms");
            return;
        }
        activeRoomStatusLabel_->setText(QStringLiteral("%1 active room%2")
            .arg(activeRooms_.size())
            .arg(activeRooms_.size() == 1 ? QString() : QStringLiteral("s")));
    }

    void selectActiveRoomItem(QListWidgetItem* item)
    {
        if (item == nullptr) {
            return;
        }
        bool ok = false;
        const int index = item->data(Qt::UserRole).toInt(&ok);
        if (!ok || index < 0 || index >= activeRooms_.size()) {
            return;
        }

        setWatchConnectionMethod(WatchConnectionMethod::InternetInvite);
        watchRoomKey_.clear();
        watchSignalRoomEdit_->setText(activeRooms_[index].roomId);
        updateSecurityVisibility();
        updateInternetStatus();
        refreshCommand();
        if (activeRoomStatusLabel_ != nullptr) {
            activeRoomStatusLabel_->setText(activeRooms_[index].passwordProtected
                ? "Selected locked room. Enter the password, then Start."
                : "Selected room. Press Start to join.");
        }
        if (activeRooms_[index].passwordProtected && watchRoomPasswordEdit_ != nullptr &&
            watchRoomPasswordEdit_->text().isEmpty()) {
            watchRoomPasswordEdit_->setFocus();
        }
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

    QString shareRoomName() const
    {
        return shareRoomNameEdit_ == nullptr ? QString() : shareRoomNameEdit_->text().trimmed();
    }

    QString shareRoomPassword() const
    {
        return shareRoomPasswordEdit_ == nullptr ? QString() : shareRoomPasswordEdit_->text();
    }

    QString watchSignalRoom() const
    {
        return watchSignalRoomEdit_ == nullptr ? QString() : watchSignalRoomEdit_->text().trimmed();
    }

    QString watchRoomPassword() const
    {
        return watchRoomPasswordEdit_ == nullptr ? QString() : watchRoomPasswordEdit_->text();
    }

    const ActiveRoom* selectedActiveRoom() const
    {
        const QString roomId = watchSignalRoom();
        if (roomId.isEmpty()) {
            return nullptr;
        }
        const auto it = std::find_if(activeRooms_.begin(), activeRooms_.end(), [&](const ActiveRoom& room) {
            return room.roomId == roomId;
        });
        return it == activeRooms_.end() ? nullptr : &*it;
    }

    bool usingWorkerRoomFlow() const
    {
        if (shareMode()) {
            return shareConnectionMethod() == ShareConnectionMethod::InternetInvite && !shareUsesLegacyInvite();
        }
        return watchConnectionMethod() == WatchConnectionMethod::InternetInvite && !watchUsesLegacyInvite();
    }

    QString currentRoomKey() const
    {
        if (!usingWorkerRoomFlow()) {
            return {};
        }
        const QString explicitKey = shareMode() ? QString() : watchRoomKey_;
        if (!explicitKey.isEmpty()) {
            return explicitKey;
        }
        return {};
    }

    QString effectiveAccessCode() const
    {
        if (usingWorkerRoomFlow()) {
            return currentRoomKey();
        }
        return accessCodeEdit_ == nullptr ? QString() : accessCodeEdit_->text();
    }

    void updateSecurityVisibility()
    {
        const bool workerRoom = usingWorkerRoomFlow();
        if (accessCodeRow_ != nullptr) {
            accessCodeRow_->setVisible(!workerRoom);
        }
        if (allowPlaintextCheck_ != nullptr) {
            if (workerRoom && allowPlaintextCheck_->isChecked()) {
                allowPlaintextCheck_->setChecked(false);
            }
            allowPlaintextCheck_->setVisible(!workerRoom);
        }
        if (roomKeyStatusLabel_ != nullptr) {
            roomKeyStatusLabel_->setVisible(workerRoom);
            if (workerRoom) {
                roomKeyStatusLabel_->setText(shareMode()
                    ? (shareRoomPassword().isEmpty()
                        ? "Open room. UDP encryption is automatic."
                        : "Locked room. The password is verified over HTTPS and mixed into UDP encryption.")
                    : (watchRoomPassword().isEmpty()
                        ? "Choose a room. UDP encryption is automatic."
                        : "This password is verified over HTTPS and mixed into UDP encryption."));
            }
        }
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
        QApplication::clipboard()->setText(roomLink(room, QString()));
        appendOutput(shareRoomPassword().isEmpty()
            ? "Room link copied to clipboard\n"
            : "Room link copied to clipboard. Send the password separately.\n");
    }

    void pasteWatchRoomLink()
    {
        QString room;
        QString roomKey;
        int port = 0;
        if (!parseRoomLink(QApplication::clipboard()->text(), nullptr, &room, &port, &roomKey)) {
            QMessageBox::warning(this, "Room", "The clipboard does not contain a ScreenShare room link.");
            return;
        }

        watchSignalRoomEdit_->setText(room);
        watchRoomKey_ = roomKey;
        updateSecurityVisibility();
        refreshCommand();
        appendOutput(roomKey.isEmpty()
            ? "Room link pasted from clipboard\n"
            : "Room link with legacy encryption key pasted from clipboard\n");
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
        if (sessionRunning()) {
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

    QString currentResolutionChoice() const
    {
        return resolutionCombo_ == nullptr ? QStringLiteral("auto") : resolutionCombo_->currentData().toString();
    }

    QSize currentFixedResolution() const
    {
        const QString choice = currentResolutionChoice();
        const QRegularExpressionMatch match =
            QRegularExpression(QStringLiteral(R"(^(\d+)x(\d+)$)")).match(choice);
        if (!match.hasMatch()) {
            return {};
        }
        return QSize(match.captured(1).toInt(), match.captured(2).toInt());
    }

    QString resolutionStatusText() const
    {
        const QString choice = currentResolutionChoice();
        if (choice == "auto") {
            return "Auto";
        }
        if (choice == "native") {
            return "Native";
        }
        const QSize resolution = currentFixedResolution();
        if (resolution.width() > 0 && resolution.height() > 0) {
            return QStringLiteral("%1x%2").arg(resolution.width()).arg(resolution.height());
        }
        return "Auto";
    }

    QString currentReportPath() const
    {
        if (reportCheck_ == nullptr ||
            reportPathEdit_ == nullptr ||
            !reportCheck_->isChecked()) {
            return {};
        }
        return reportPathEdit_->text().trimmed();
    }

    screenshare::StreamSettings currentStreamSettings() const
    {
        screenshare::StreamSettings settings;
        settings.fps = fpsSpin_ == nullptr ? 60 : fpsSpin_->value();
        settings.adaptBitrate = true;
        settings.adaptResolution = currentResolutionChoice() == "auto";

        const QSize resolution = currentFixedResolution();
        if (resolution.width() > 0 && resolution.height() > 0) {
            settings.outputResolution = screenshare::SessionResolution{
                resolution.width(),
                resolution.height(),
            };
        }
        return settings;
    }

    screenshare::ShareSessionConfig currentShareSessionConfig() const
    {
        screenshare::ShareSessionConfig config;
        if (shareConnectionMethod() == ShareConnectionMethod::InternetInvite && !shareUsesLegacyInvite()) {
            config.connectionMode = screenshare::ShareConnectionMode::Room;
        } else if (shareConnectionMethod() == ShareConnectionMethod::InternetInvite) {
            config.connectionMode = screenshare::ShareConnectionMode::ManualInvite;
        } else {
            config.connectionMode = screenshare::ShareConnectionMode::DirectTargets;
        }
        config.displayIndex = selectedDisplayIndex();
        config.roomPort = static_cast<uint16_t>(shareInvitePortSpin_->value());
        config.roomId = toStdUtf8(shareSignalRoom());
        config.roomName = toStdUtf8(shareRoomName());
        config.roomPassword = toStdUtf8(shareRoomPassword());
        config.signalingStunServer = toStdUtf8(stunServer());
        config.udpAccessCode = toStdUtf8(effectiveAccessCode());
        config.allowPlaintext = allowPlaintextCheck_ != nullptr && allowPlaintextCheck_->isChecked();
        config.reportPath = toStdUtf8(currentReportPath());
        config.audioDeviceId = audioDeviceCombo_ == nullptr ?
            std::string() :
            toStdUtf8(audioDeviceCombo_->currentData().toString());
        config.targets = toStdStringVector(currentDirectShareTargets());
        config.localInvite = shareLocalInviteEdit_ == nullptr ?
            std::string() :
            toStdUtf8(shareLocalInviteEdit_->text().trimmed());
        config.watcherInvites = toStdStringVector(currentWatcherResponseInvites());
        config.captureSystemAudio = true;
        config.stream = currentStreamSettings();
        return config;
    }

    screenshare::WatchSessionConfig currentWatchSessionConfig() const
    {
        screenshare::WatchSessionConfig config;
        if (watchConnectionMethod() == WatchConnectionMethod::InternetInvite && !watchUsesLegacyInvite()) {
            config.connectionMode = screenshare::WatchConnectionMode::Room;
        } else if (watchConnectionMethod() == WatchConnectionMethod::InternetInvite) {
            config.connectionMode = screenshare::WatchConnectionMode::ManualInvite;
        } else {
            config.connectionMode = screenshare::WatchConnectionMode::DirectListen;
        }
        config.listenPort = static_cast<uint16_t>(watchPortSpin_->value());
        config.roomId = toStdUtf8(watchSignalRoom());
        config.roomPassword = watchRoomKey_.isEmpty() ? toStdUtf8(watchRoomPassword()) : std::string();
        config.signalingStunServer = toStdUtf8(stunServer());
        config.udpAccessCode = toStdUtf8(effectiveAccessCode());
        config.allowPlaintext = allowPlaintextCheck_ != nullptr && allowPlaintextCheck_->isChecked();
        config.reportPath = toStdUtf8(currentReportPath());
        config.playAudio = true;
        config.muted = mutedCheck_ != nullptr && mutedCheck_->isChecked();
        config.lanAdvertise = lanDiscoverableCheck_ != nullptr && lanDiscoverableCheck_->isChecked();
        config.peerInvite = watchPeerInviteEdit_ == nullptr ?
            std::string() :
            toStdUtf8(watchPeerInviteEdit_->text().trimmed());
        config.previewLatencyMs = previewLatencySpin_ == nullptr ? 100 : previewLatencySpin_->value();
        config.audioPlaybackVolumePercent = volumeSpin_ == nullptr ? 100 : volumeSpin_->value();
        return config;
    }

    QStringList currentArguments() const
    {
        if (shareMode()) {
            return toQStringList(screenshare::BuildShareArguments(currentShareSessionConfig()));
        }
        return toQStringList(screenshare::BuildWatchArguments(currentWatchSessionConfig()));
    }

    QStringList displayArguments() const
    {
        QStringList args;
        try {
            args = currentArguments();
        } catch (const std::exception&) {
            return {};
        }
        for (int index = 0; index + 1 < args.size(); ++index) {
            if (args[index] == "--access-code") {
                args[index + 1] = "<redacted>";
                ++index;
            } else if (args[index] == "--signal-room-password") {
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

    void applyRuntimeStreamSettings()
    {
        if (!running_ || !shareMode() || sessionBackend_ == nullptr) {
            return;
        }

        sessionBackend_->applyStreamSettings(currentStreamSettings());
        appendOutput("Requested stream settings: resolution " + resolutionStatusText() + "\n");
    }

    void applyTheme(bool darkMode)
    {
        qApp->setStyleSheet(appStyleSheet(darkMode));
        refreshButtonIcons(darkMode);
        repolish(this);
    }

    void refreshButtonIcons(bool darkMode)
    {
        for (QPushButton* button : findChildren<QPushButton*>()) {
            refreshButtonIcon(button, darkMode);
        }
    }

    bool sessionRunning() const
    {
        return sessionBackend_ != nullptr && sessionBackend_->isRunning();
    }

    bool sessionStopRequested() const
    {
        return sessionBackend_ != nullptr && sessionBackend_->stopRequested();
    }

    void toggleSession()
    {
        if (!sessionRunning()) {
            startSession();
        } else {
            stopSession();
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
        if (share && !validOptionalRoomText(shareRoomName(), 80)) {
            QMessageBox::warning(this, "Room name", "The friendly room name must be 80 characters or fewer and cannot contain control characters.");
            if (shareRoomNameEdit_ != nullptr) {
                shareRoomNameEdit_->setFocus();
            }
            return false;
        }
        const QString password = share ? shareRoomPassword() : watchRoomPassword();
        if (!password.isEmpty() && !validOptionalRoomText(password, 128)) {
            QMessageBox::warning(this, "Room password", "Room passwords must be 128 characters or fewer and cannot contain control characters.");
            QLineEdit* passwordEdit = share ? shareRoomPasswordEdit_ : watchRoomPasswordEdit_;
            if (passwordEdit != nullptr) {
                passwordEdit->setFocus();
            }
            return false;
        }
        if (!share) {
            const ActiveRoom* roomInfo = selectedActiveRoom();
            if (roomInfo != nullptr && roomInfo->passwordProtected && password.isEmpty()) {
                QMessageBox::warning(this, "Room password", "This room is locked. Enter its password before watching.");
                if (watchRoomPasswordEdit_ != nullptr) {
                    watchRoomPasswordEdit_->setFocus();
                }
                return false;
            }
        }
        return true;
    }

    void startSession()
    {
        if (sessionRunning()) {
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
        accessCodeWarningShown_ = false;
        roomAlreadyOpenWarningShown_ = false;
        roomWatcherMismatchLogged_ = false;
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
        if (!usingWorkerRoomFlow() && effectiveAccessCode().isEmpty() && !allowPlaintextCheck_->isChecked()) {
            QMessageBox::warning(
                this,
                "Security choice",
                "Generate or paste an access code, or allow plaintext for this run.");
            return;
        }
        const QStringList displayArgs = displayArguments();
        appendOutput("\n" + formatCommand(enginePath(), displayArgs) + "\n");
        QString errorMessage;
        const bool started = shareMode() ?
            sessionBackend_->startShare(currentShareSessionConfig(), &errorMessage) :
            sessionBackend_->startWatch(currentWatchSessionConfig(), &errorMessage);
        if (!started && !errorMessage.isEmpty()) {
            appendOutput("Session error: " + errorMessage + "\n");
        }
    }

    void stopSession()
    {
        if (!sessionRunning() || sessionStopRequested()) {
            return;
        }
        appendOutput("Stopping...\n");
        sessionBackend_->stop();
        applyStatusBadge();
        updateInternetStatus();
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
            displayCombo_->setItemData(displayCombo_->count() - 1, QSize(display.width, display.height), kDisplaySizeRole);
            if (!display.adapterName.isEmpty()) {
                displayCombo_->setItemData(displayCombo_->count() - 1, display.adapterName, Qt::ToolTipRole);
            }
        }

        const int preferredIndex = displayCombo_->findData(previousDisplay);
        displayCombo_->setCurrentIndex(preferredIndex >= 0 ? preferredIndex : 0);
        displayCombo_->setToolTip(displayCombo_->currentText());
        displayCombo_->blockSignals(wasBlocked);
        populateResolutionChoices();
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

    QSize selectedDisplaySize() const
    {
        if (displayCombo_ == nullptr || displayCombo_->currentIndex() < 0) {
            return {};
        }
        return displayCombo_->itemData(displayCombo_->currentIndex(), kDisplaySizeRole).toSize();
    }

    void populateResolutionChoices()
    {
        if (resolutionCombo_ == nullptr) {
            return;
        }

        const QString previousChoice = currentResolutionChoice();
        const QSize displaySize = selectedDisplaySize();
        const bool wasBlocked = resolutionCombo_->blockSignals(true);
        resolutionCombo_->clear();
        resolutionCombo_->addItem("Auto", QStringLiteral("auto"));
        for (const QSize& size : resolutionChoicesForDisplay(displaySize)) {
            resolutionCombo_->addItem(resolutionChoiceText(size), resolutionChoiceValue(size));
        }

        int preferredIndex = resolutionCombo_->findData(previousChoice);
        if (preferredIndex < 0 && previousChoice == "native" && validResolutionSize(displaySize)) {
            preferredIndex = resolutionCombo_->findData(resolutionChoiceValue(evenResolutionSize(displaySize)));
        }
        resolutionCombo_->setCurrentIndex(preferredIndex >= 0 ? preferredIndex : 0);
        resolutionCombo_->setToolTip(resolutionCombo_->currentText());
        resolutionCombo_->blockSignals(wasBlocked);
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
        if (sessionRunning()) {
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
        if (displayRefreshing_ || sessionRunning() || sessionBackend_ == nullptr) {
            return;
        }

        if (!automatic) {
            appendOutput("\nRefreshing displays...\n");
        }
        setDisplayRefreshing(true);

        QString errorMessage;
        const auto displays = sessionBackend_->listDisplays(&errorMessage);
        setDisplayRefreshing(false);
        if (!errorMessage.isEmpty()) {
            if (!automatic) {
                appendOutput("Display refresh error: " + errorMessage + "\n");
                QMessageBox::warning(this, "Display refresh failed", errorMessage);
            }
            return;
        }

        updateDisplayList(displayChoicesFromSessionDisplays(displays));
        refreshCommand();
    }

    void refreshAudioDevices(bool automatic)
    {
        if (audioDeviceRefreshing_ || sessionRunning() || sessionBackend_ == nullptr) {
            return;
        }

        if (!automatic) {
            appendOutput("\nRefreshing audio devices...\n");
        }
        setAudioDeviceRefreshing(true);

        QString errorMessage;
        const auto devices = audioOutputDevicesFromSessionDevices(sessionBackend_->listAudioDevices(&errorMessage));
        setAudioDeviceRefreshing(false);
        if (!errorMessage.isEmpty()) {
            if (!automatic) {
                appendOutput("Audio device refresh error: " + errorMessage + "\n");
                QMessageBox::warning(this, "Audio refresh failed", errorMessage);
            }
            return;
        }

        updateAudioDeviceList(devices);
        if (!automatic) {
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

    void clearRoomKeyForRetry(const QString& message, const QString& title)
    {
        if (!shareMode()) {
            watchRoomKey_.clear();
            if (watchRoomPasswordEdit_ != nullptr && usingWorkerRoomFlow()) {
                watchRoomPasswordEdit_->clear();
                watchRoomPasswordEdit_->setFocus();
            } else if (watchSignalRoomEdit_ != nullptr) {
                watchSignalRoomEdit_->setFocus();
            }
        } else if (copyShareRoomButton_ != nullptr) {
            copyShareRoomButton_->setFocus();
        }
        updateSecurityVisibility();
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
            findLanButton_->setEnabled(!discovering && !sessionRunning());
            findLanButton_->setText(discovering ? "Scanning..." : "Refresh");
        }
        updateReceiverStatus(discovering);
        if (receiverList_ != nullptr) {
            receiverList_->setEnabled(!discovering);
        }
        if (!sessionRunning()) {
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
        displayRefreshing_ = refreshing;
        if (refreshDisplaysButton_ != nullptr) {
            refreshDisplaysButton_->setEnabled(!refreshing && !sessionRunning());
            refreshDisplaysButton_->setText(refreshing ? "Scanning..." : "Refresh");
        }
    }

    void setAudioDeviceRefreshing(bool refreshing)
    {
        audioDeviceRefreshing_ = refreshing;
        if (refreshAudioDevicesButton_ != nullptr) {
            refreshAudioDevicesButton_->setEnabled(!refreshing && !sessionRunning());
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
        const bool running = sessionRunning();
        const bool canCreate = !creating && !running;
        if (newShareRoomButton_ != nullptr) {
            newShareRoomButton_->setEnabled(!running);
        }
        if (copyShareRoomButton_ != nullptr) {
            copyShareRoomButton_->setEnabled(!running);
        }
        if (shareRoomNameEdit_ != nullptr) {
            shareRoomNameEdit_->setEnabled(!running);
        }
        if (shareRoomPasswordEdit_ != nullptr) {
            shareRoomPasswordEdit_->setEnabled(!running);
        }
        if (pasteWatchRoomLinkButton_ != nullptr) {
            pasteWatchRoomLinkButton_->setEnabled(!running);
        }
        if (watchRoomPasswordEdit_ != nullptr) {
            watchRoomPasswordEdit_->setEnabled(!running);
        }
        if (refreshRoomsButton_ != nullptr) {
            refreshRoomsButton_->setEnabled(!running && !roomDirectoryRefreshing_);
        }
        if (activeRoomList_ != nullptr) {
            activeRoomList_->setEnabled(!running && !roomDirectoryRefreshing_);
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
        updateShareViewerStatusLabel();
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
            return "Choose a room ID with letters, numbers, dashes, or underscores.";
        }
        if (!validOptionalRoomText(shareRoomName(), 80)) {
            return "Shorten the room name or remove control characters.";
        }
        if (!shareRoomPassword().isEmpty() && !validOptionalRoomText(shareRoomPassword(), 128)) {
            return "Shorten the room password or remove control characters.";
        }
        if (!shareRoomPassword().isEmpty()) {
            return "Ready: start sharing. Friends can pick this locked room, then enter the password.";
        }
        return "Ready: start sharing. Friends can pick this room from the list or use the copied link.";
    }

    QString watchWorkerRoomStatusText() const
    {
        if (watchSignalRoom().isEmpty()) {
            return "Choose an active room or paste a room link.";
        }
        if (!validRoomId(watchSignalRoom())) {
            return "Room names need letters, numbers, dashes, or underscores.";
        }
        if (!watchRoomPassword().isEmpty() && !validOptionalRoomText(watchRoomPassword(), 128)) {
            return "Shorten the room password or remove control characters.";
        }
        const ActiveRoom* room = selectedActiveRoom();
        if (room != nullptr && room->passwordProtected && watchRoomPassword().isEmpty()) {
            return "This room is locked. Enter the password to watch.";
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
        if (running_ && sessionStopRequested() && share == shareMode()) {
            return share ? "Stopping Share..." : "Stopping Watch...";
        }
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
                return usingWorkerRoomFlow() ?
                    "Live: a watcher failed the room/password check. Share is still available." :
                    "Live: probe rejected. Check the access code on both sides.";
            }
            if (status == "direct_udp_blocked") {
                return "Blocked: signaling worked, but direct UDP did not cross the networks.";
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
                return usingWorkerRoomFlow() ?
                    "Live: room password or encryption mismatch." :
                    "Live: media rejected. Check access code or plaintext mode.";
            }
            if (status == "incoming_unaccepted") {
                return "Live: packets arrived but were not accepted.";
            }
            if (status == "probe_send_errors") {
                return "Live: probe send failed. Check the pasted sender invite.";
            }
            if (status == "direct_udp_blocked") {
                return "Blocked: signaling worked, but no direct UDP media arrived.";
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
            setButtonIcon(actionButton_, "stop", 16);
            actionButton_->setProperty("class", "Danger");
            resetPeerStatus();
        } else {
            updateStartButtonText();
            actionButton_->setProperty("class", "");
            resetPeerStatus();
            runtimeNatStatus_.clear();
            runtimeNatHint_.clear();
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
        if (shareSignalRoomEdit_ != nullptr) {
            shareSignalRoomEdit_->setEnabled(!running);
        }
        if (shareRoomNameEdit_ != nullptr) {
            shareRoomNameEdit_->setEnabled(!running);
        }
        if (shareRoomPasswordEdit_ != nullptr) {
            shareRoomPasswordEdit_->setEnabled(!running);
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
        if (watchSignalRoomEdit_ != nullptr) {
            watchSignalRoomEdit_->setEnabled(!running);
        }
        if (watchRoomPasswordEdit_ != nullptr) {
            watchRoomPasswordEdit_->setEnabled(!running);
        }
        if (displayCombo_ != nullptr) {
            displayCombo_->setEnabled(!running);
        }
        if (refreshDisplaysButton_ != nullptr) {
            refreshDisplaysButton_->setEnabled(!running && !displayRefreshing_);
        }
        if (fpsSpin_ != nullptr) {
            fpsSpin_->setEnabled(!running);
        }
        if (resolutionCombo_ != nullptr) {
            resolutionCombo_->setEnabled(shareMode() || !running);
        }
        if (audioDeviceCombo_ != nullptr) {
            audioDeviceCombo_->setEnabled(!running);
        }
        if (refreshAudioDevicesButton_ != nullptr) {
            refreshAudioDevicesButton_->setEnabled(!running && !audioDeviceRefreshing_);
        }
        updateInviteButtons();
        if (!running && shouldRefreshRoomDirectory()) {
            startRoomDirectoryRefresh(true);
        }
    }

    void updateStartButtonText()
    {
        if (actionButton_ != nullptr) {
            actionButton_->setText(shareMode() ? "Share" : "Watch");
            setButtonIcon(actionButton_, shareMode() ? "share" : "watch", 16);
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
        } else if (sessionStopRequested()) {
            statusBadge_->setText("◌  Stopping");
            statusBadge_->setProperty("class", "StatusConnecting");
            statusBadge_->setToolTip(shareMode() ?
                "Share is stopping." :
                "Watch is stopping.");
            if (statusPulseAnimation_ != nullptr && statusPulseAnimation_->state() != QAbstractAnimation::Running) {
                statusPulseAnimation_->start();
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

    void handleSessionOutput(const QString& text)
    {
        appendOutput(text);
    }

    void handleSessionStatus(const screenshare::SessionEvent& event)
    {
        latestSessionStatus_ = event.status;

        if (event.type == screenshare::SessionEventType::ViewerListChanged) {
            updateShareViewerStatus(event.status.viewers);
        } else if (event.type == screenshare::SessionEventType::NatStatusChanged) {
            updateRuntimeNatStatus(event.status);
        } else if (event.type == screenshare::SessionEventType::Issue) {
            handleSessionIssue(event);
        }

        if (!sessionRunning()) {
            return;
        }

        if (event.status.state == screenshare::SessionState::Live) {
            if (!shareMode()) {
                markPeerActivity();
            }
            applyStatusBadge();
            updateInternetStatus();
        } else if (event.status.state == screenshare::SessionState::Connecting ||
            event.status.state == screenshare::SessionState::Disconnected ||
            event.status.state == screenshare::SessionState::Recovering) {
            checkPeerActivityTimeout();
            applyStatusBadge();
            updateInternetStatus();
        }
    }

    void handleSessionIssue(const screenshare::SessionEvent& event)
    {
        switch (event.issue) {
        case screenshare::SessionIssue::RoomAlreadyOpen:
            handleRoomAlreadyOpenProblem();
            return;
        case screenshare::SessionIssue::PreviewClosed:
            handlePreviewClosedSignal();
            return;
        case screenshare::SessionIssue::HostLeft:
            appendOutput("Host left the room.\n");
            return;
        case screenshare::SessionIssue::AccessCodeRequired:
        case screenshare::SessionIssue::AccessCodeMismatch:
            handleAccessCodeProblem(event.issue);
            return;
        case screenshare::SessionIssue::None:
            return;
        }
    }

    void handleRoomAlreadyOpenProblem()
    {
        if (roomAlreadyOpenWarningShown_) {
            return;
        }

        roomAlreadyOpenWarningShown_ = true;
        stopSession();
        if (shareSignalRoomEdit_ != nullptr) {
            shareSignalRoomEdit_->setFocus();
        }
        QMessageBox::warning(
            this,
            "Room already open",
            "That room ID is already active as an open room, so it cannot be locked mid-session. Choose New room, or remove the password to join the open room.");
    }

    void handlePreviewClosedSignal()
    {
        if (shareMode() ||
            sessionStopRequested() ||
            !sessionRunning()) {
            return;
        }

        appendOutput("Preview window closed; stopping Watch...\n");
        stopSession();
        applyStatusBadge();
        updateInternetStatus();
    }

    void handleAccessCodeProblem(screenshare::SessionIssue issue)
    {
        if (accessCodeWarningShown_) {
            return;
        }

        if (issue != screenshare::SessionIssue::AccessCodeRequired &&
            issue != screenshare::SessionIssue::AccessCodeMismatch) {
            return;
        }

        if (usingWorkerRoomFlow() && shareMode() && issue == screenshare::SessionIssue::AccessCodeMismatch) {
            if (!roomWatcherMismatchLogged_) {
                appendOutput("A watcher failed the room/password encryption check; keeping Share running.\n");
                roomWatcherMismatchLogged_ = true;
            }
            return;
        }

        accessCodeWarningShown_ = true;
        if (usingWorkerRoomFlow()) {
            if (issue == screenshare::SessionIssue::AccessCodeRequired) {
                stopSession();
                clearRoomKeyForRetry(
                    "This room expects encrypted traffic. Choose the same room on both computers, then start again. If the room is locked, enter the room password.",
                    "Room encryption required");
                return;
            }

            stopSession();
            clearRoomKeyForRetry(
                sessionRunning() ?
                    "The stream was rejected by the room encryption key. Stop the current run, check the room/password, then start again." :
                    "The stream was rejected by the room encryption key. Check the room/password, then start again.",
                "Room encryption mismatch");
            return;
        }

        if (issue == screenshare::SessionIssue::AccessCodeRequired) {
            clearAccessCodeForRetry(
                "This room or receiver needs an access code. Enter the same access code on both computers, or enable plaintext on both sides.",
                "Access code required");
            return;
        }

        clearAccessCodeForRetry(
            sessionRunning() ?
                "The room invite or incoming packets were rejected with this access code. Stop the current run, enter the same access code on both computers, then start again." :
                "The room invite or incoming packets were rejected with this access code. Enter the same access code on both computers, then start again.");
    }

    void resetPeerStatus()
    {
        peerConnected_ = false;
        peerActivitySeen_ = false;
        latestSessionStatus_ = screenshare::SessionStatus{};
        shareViewers_.clear();
        shareViewerClock_.restart();
        updateShareViewerStatusLabel();
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
        if (running_ && shareMode()) {
            updateShareViewerStatusLabel();
        }
        if (!running_ || !peerConnected_ || !peerActivitySeen_) {
            return;
        }
        const int timeoutMs = shareMode() ? kSharePeerActivityTimeoutMs : kWatchPeerActivityTimeoutMs;
        if (peerActivityTimer_.isValid() && peerActivityTimer_.elapsed() > timeoutMs) {
            peerConnected_ = false;
            applyStatusBadge();
            updateInternetStatus();
        }
    }

    static QString sessionStateName(screenshare::SessionState state)
    {
        return QString::fromLatin1(screenshare::ToString(state));
    }

    ShareViewerStatus& shareViewerForEndpoint(int group, const QString& endpoint)
    {
        for (ShareViewerStatus& viewer : shareViewers_) {
            if (viewer.group == group && viewer.endpoint == endpoint) {
                return viewer;
            }
        }

        ShareViewerStatus viewer;
        viewer.group = group;
        viewer.endpoint = endpoint;
        shareViewers_.append(viewer);
        return shareViewers_.last();
    }

    void removeViewersNotIn(const QSet<QString>& visibleViewerKeys)
    {
        for (int index = shareViewers_.size() - 1; index >= 0; --index) {
            const ShareViewerStatus& viewer = shareViewers_[index];
            const QString key = QString::number(viewer.group) + "|" + viewer.endpoint;
            if (!visibleViewerKeys.contains(key)) {
                shareViewers_.removeAt(index);
            }
        }
    }

    void updateShareViewerStatus(const std::vector<screenshare::SessionViewer>& viewers)
    {
        if (!shareMode()) {
            return;
        }
        if (!shareViewerClock_.isValid()) {
            shareViewerClock_.restart();
        }

        bool activeNow = false;
        QSet<QString> visibleViewerKeys;
        const qint64 nowMs = shareViewerClock_.elapsed();
        for (const screenshare::SessionViewer& sessionViewer : viewers) {
            const QString endpoint = QString::fromStdString(sessionViewer.endpoint);
            if (endpoint.isEmpty()) {
                continue;
            }
            visibleViewerKeys.insert(QString::number(sessionViewer.group) + "|" + endpoint);

            ShareViewerStatus& viewer = shareViewerForEndpoint(sessionViewer.group, endpoint);
            viewer.engineState = sessionStateName(sessionViewer.state);
            viewer.health = QString::fromStdString(sessionViewer.health);
            viewer.session = QString::fromStdString(sessionViewer.sessionFingerprint);
            viewer.pendingDatagrams = sessionViewer.pendingDatagrams;
            viewer.queueMs = sessionViewer.queueDelayMs;
            viewer.completedFrames = sessionViewer.completedFrames;
            viewer.resyncs = sessionViewer.decodeResyncs;
            viewer.feedbackPackets = sessionViewer.feedbackPackets;
            if (sessionViewer.hasFeedback) {
                viewer.everFeedback = true;
            }
            if (sessionViewer.activeNow) {
                viewer.everFeedback = true;
                viewer.lastActiveMs = nowMs;
                activeNow = true;
            }
        }

        removeViewersNotIn(visibleViewerKeys);
        if (activeNow) {
            markPeerActivity();
        }
        updateInternetStatus();
    }

    QString shareViewerStateText(const ShareViewerStatus& viewer) const
    {
        if (viewer.engineState == "failed") {
            return "Failed";
        }
        if (viewer.everFeedback &&
            viewer.lastActiveMs >= 0 &&
            shareViewerClock_.isValid() &&
            shareViewerClock_.elapsed() - viewer.lastActiveMs <= kSharePeerActivityTimeoutMs) {
            return "Live";
        }
        if (viewer.everFeedback) {
            return "Disconnected";
        }
        return "Waiting";
    }

    void updateShareViewerStatusLabel()
    {
        if (shareViewerStatusLabel_ == nullptr) {
            return;
        }
        const bool visible = shareMode() && (running_ || !shareViewers_.isEmpty());
        shareViewerStatusLabel_->setVisible(visible);
        if (!visible) {
            shareViewerStatusLabel_->clear();
            return;
        }

        if (shareViewers_.isEmpty()) {
            shareViewerStatusLabel_->setText("Viewers: waiting for watchers.");
            return;
        }

        int liveCount = 0;
        int failedCount = 0;
        QVector<int> displayedIndexes;
        displayedIndexes.reserve(shareViewers_.size());
        for (int index = 0; index < shareViewers_.size(); ++index) {
            displayedIndexes.push_back(index);
        }

        QStringList rows;
        constexpr int MaxRows = 4;
        for (const int viewerIndex : displayedIndexes) {
            const ShareViewerStatus& viewer = shareViewers_[viewerIndex];
            const QString state = shareViewerStateText(viewer);
            if (state == "Live") {
                ++liveCount;
            } else if (state == "Failed") {
                ++failedCount;
            }
            if (rows.size() >= MaxRows) {
                continue;
            }

            QStringList details;
            if (!viewer.health.isEmpty() && viewer.health != "none" && viewer.health != "unknown") {
                details.push_back(viewer.health);
            }
            if (!viewer.session.isEmpty() && viewer.session != "none") {
                details.push_back("session " + viewer.session);
            }
            if (viewer.queueMs > 0) {
                details.push_back(QString::number(viewer.queueMs) + " ms queue");
            }
            if (viewer.resyncs > 0) {
                details.push_back(QString::number(viewer.resyncs) + " resyncs");
            }

            QString row = state + ": " + viewer.endpoint;
            if (!details.isEmpty()) {
                row += " (" + details.join(", ") + ")";
            }
            rows.push_back(row);
        }

        QString summary = QStringLiteral("Viewers: %1 live / %2 total")
            .arg(liveCount)
            .arg(displayedIndexes.size());
        if (failedCount > 0) {
            summary += QStringLiteral(", %1 failed").arg(failedCount);
        }
        if (displayedIndexes.size() > MaxRows) {
            rows.push_back(QStringLiteral("+%1 more").arg(displayedIndexes.size() - MaxRows));
        }
        shareViewerStatusLabel_->setText(summary + "\n" + rows.join("\n"));
    }

    void updateRuntimeNatStatus(const screenshare::SessionStatus& status)
    {
        const QString natStatus = QString::fromStdString(status.natStatus);
        if (natStatus.isEmpty()) {
            return;
        }
        runtimeNatStatus_ = natStatus;
        const QString hint = QString::fromStdString(status.natHint);
        if (!hint.isEmpty()) {
            runtimeNatHint_ = hint;
        }
        runtimeNatShareMode_ = status.role == screenshare::SessionRole::Share;
        updateInternetStatus();
    }

    void appendOutput(const QString& text)
    {
        // The Output panel was removed from the UI. Engine stdout is still
        // captured for saved-report logs; mirror it to qDebug so developers
        // can still tail it from a terminal build.
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
    QtSessionBackend* sessionBackend_ = nullptr;
    bool running_ = false;
    bool peerConnected_ = false;
    bool peerActivitySeen_ = false;
    QElapsedTimer peerActivityTimer_;
    QProcess* discoveryProcess_ = nullptr;
    QProcess* tailscaleProcess_ = nullptr;
    QProcess* inviteProcess_ = nullptr;
    QNetworkAccessManager* roomDirectoryNetwork_ = nullptr;
    QNetworkReply* roomDirectoryReply_ = nullptr;
    QTimer* receiverRefreshTimer_ = nullptr;
    QTimer* roomDirectoryRefreshTimer_ = nullptr;
    QTimer* peerStatusTimer_ = nullptr;

    QListWidget* receiverList_ = nullptr;
    QLabel* receiverStatusLabel_ = nullptr;
    QPushButton* nearbyConnectionButton_ = nullptr;
    QPushButton* internetConnectionButton_ = nullptr;
    QPushButton* manualConnectionButton_ = nullptr;
    PageStack* shareConnectionStack_ = nullptr;
    ShareConnectionMethod shareConnectionMethod_ = ShareConnectionMethod::InternetInvite;
    QLineEdit* shareSignalRoomEdit_ = nullptr;
    QLineEdit* shareRoomNameEdit_ = nullptr;
    QLineEdit* shareRoomPasswordEdit_ = nullptr;
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
    QLabel* shareViewerStatusLabel_ = nullptr;
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
    QString watchRoomKey_;
    QLineEdit* watchRoomPasswordEdit_ = nullptr;
    QPushButton* pasteWatchRoomLinkButton_ = nullptr;
    QListWidget* activeRoomList_ = nullptr;
    QLabel* activeRoomStatusLabel_ = nullptr;
    QPushButton* refreshRoomsButton_ = nullptr;
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

    QWidget* accessCodeRow_ = nullptr;
    QLineEdit* accessCodeEdit_ = nullptr;
    QPushButton* generateAccessCodeButton_ = nullptr;
    QPushButton* copyAccessCodeButton_ = nullptr;
    QLabel* roomKeyStatusLabel_ = nullptr;
    QCheckBox* allowPlaintextCheck_ = nullptr;
    QLineEdit* stunServerEdit_ = nullptr;
    QCheckBox* reportCheck_ = nullptr;
    QLineEdit* reportPathEdit_ = nullptr;
    QPushButton* browseReportButton_ = nullptr;
    bool reportPathEdited_ = false;
    QString discoveryOutput_;
    QString tailscaleOutput_;
    QString inviteOutput_;
    InviteTarget inviteTarget_ = InviteTarget::None;
    QVector<DiscoveredReceiver> discoveredReceivers_;
    QVector<DiscoveredReceiver> lanReceivers_;
    QVector<DiscoveredReceiver> tailscaleReceivers_;
    QVector<ActiveRoom> activeRooms_;
    bool discoverySelectFirst_ = false;
    bool discoveryLogOutput_ = false;
    bool tailscaleLogOutput_ = false;
    bool displayRefreshing_ = false;
    bool audioDeviceRefreshing_ = false;
    bool roomDirectoryRefreshing_ = false;
    bool roomDirectoryRefreshQuiet_ = false;
    bool receiverScanNotificationShown_ = false;
    bool selectedReceiverKnown_ = false;
    bool selectedReceiverSecurityKnown_ = false;
    bool selectedReceiverEncrypted_ = false;
    QString selectedReceiverHost_;
    int selectedReceiverPort_ = 0;
    QString selectedReceiverAccessFingerprint_;
    QString runtimeNatStatus_;
    QString runtimeNatHint_;
    screenshare::SessionStatus latestSessionStatus_;
    QVector<ShareViewerStatus> shareViewers_;
    QElapsedTimer shareViewerClock_;
    bool runtimeNatShareMode_ = true;
    bool accessCodeWarningShown_ = false;
    bool roomAlreadyOpenWarningShown_ = false;
    bool roomWatcherMismatchLogged_ = false;
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
QListWidget#ReceiverList, QListWidget#WatcherInviteList, QListWidget#ActiveRoomList {
    background: #0b0f14;
    border: 1px solid #2a3340;
    border-radius: 8px;
    padding: 4px;
    color: #e6ecf2;
}
QListWidget#ReceiverList::item, QListWidget#WatcherInviteList::item, QListWidget#ActiveRoomList::item {
    border-radius: 6px;
    padding: 8px 10px;
}
QListWidget#ReceiverList::item:selected, QListWidget#WatcherInviteList::item:selected, QListWidget#ActiveRoomList::item:selected {
    background: #1a9b89;
    color: #ffffff;
}
QListWidget#ReceiverList::item:hover, QListWidget#WatcherInviteList::item:hover, QListWidget#ActiveRoomList::item:hover {
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
QListWidget#ReceiverList, QListWidget#WatcherInviteList, QListWidget#ActiveRoomList {
    background: #ffffff;
    border: 1px solid #cfd6df;
    border-radius: 8px;
    padding: 4px;
    color: #15202b;
}
QListWidget#ReceiverList::item, QListWidget#WatcherInviteList::item, QListWidget#ActiveRoomList::item {
    border-radius: 6px;
    padding: 8px 10px;
}
QListWidget#ReceiverList::item:selected, QListWidget#WatcherInviteList::item:selected, QListWidget#ActiveRoomList::item:selected {
    background: #157a6e;
    color: #ffffff;
}
QListWidget#ReceiverList::item:hover, QListWidget#WatcherInviteList::item:hover, QListWidget#ActiveRoomList::item:hover {
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
    bool classicUi = false;
    const QStringList arguments = startupArguments(argc, argv);
    for (int index = 1; index < arguments.size(); ++index) {
        const QString arg = arguments[index];
        if (arg == "--gui-smoke-test") {
            guiSmokeTest = true;
            continue;
        }
        if (arg == "--classic-ui") {
            classicUi = true;
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
            const bool resourcesAvailable =
                !appIcon().isNull() &&
                !uiIcon("share").isNull() &&
                !uiIcon("watch").isNull() &&
                canRenderUiIconSvg(QStringLiteral("share"), "#ffffff") &&
                canRenderUiIconSvg(QStringLiteral("watch"), "#ffffff") &&
                QFileInfo::exists(QStringLiteral(":/screenshare/brand/screenshare-mark.svg")) &&
                QFileInfo::exists(QStringLiteral(":/screenshare/ui/icons/share.svg")) &&
                QFileInfo::exists(QStringLiteral(":/screenshare/ui/icons/watch.svg"));
            const auto displays = displayChoicesFromSessionDisplays({
                screenshare::SessionDisplayInfo{
                    0, "\\\\.\\DISPLAY1", 2560, 1440, 0, 0, "NVIDIA GeForce RTX", true},
                screenshare::SessionDisplayInfo{
                    1, "\\\\.\\DISPLAY2", 1920, 1080, -1920, 0, "NVIDIA GeForce RTX", true},
            });
            const auto qhdResolutionChoices = resolutionChoicesForDisplay(QSize(2560, 1440));
            const auto fhdResolutionChoices = resolutionChoicesForDisplay(QSize(1920, 1080));
            const auto activeRooms = parseActiveRooms(QByteArrayLiteral(R"json({
                "ok": true,
                "rooms": [
                    {
                        "roomId": "room-beta",
                        "name": "Beta Room",
                        "peerCount": 2,
                        "updatedAt": 1760000001000,
                        "expiresAt": 1760000181000,
                        "requiresRoomKey": false,
                        "passwordProtected": false
                    },
                    {
                        "roomId": "bad room",
                        "peerCount": 1,
                        "updatedAt": 1760000002000,
                        "expiresAt": 1760000182000,
                        "requiresRoomKey": false
                    },
                    {
                        "roomId": "room-alpha",
                        "name": "Alpha Room",
                        "peerCount": 1,
                        "updatedAt": 1760000002000,
                        "expiresAt": 1760000182000,
                        "requiresRoomKey": true,
                        "passwordProtected": true
                    }
                ]
            })json"));
            QString parsedRoomServer;
            QString parsedRoom;
            QString parsedRoomKey;
            int parsedRoomPort = 0;
            const bool parsedRoomLink = parseRoomLink(
                "copied room: screenshare-room-v1;room=room-abc_123;key=ABCDE-23456-FGHIJ-789PQ",
                &parsedRoomServer,
                &parsedRoom,
                &parsedRoomPort,
                &parsedRoomKey);
            QString parsedLegacyRoom;
            int parsedLegacyRoomPort = 0;
            const bool parsedLegacyRoomLink = parseRoomLink(
                "copied room: screenshare-room-v1;room=room-abc_123;port=5001",
                nullptr,
                &parsedLegacyRoom,
                &parsedLegacyRoomPort);
            QString parsedShortRoom;
            QString parsedShortRoomKey;
            const bool parsedShortRoomLink = parseRoomLink(
                roomLink("room-public", QString()),
                nullptr,
                &parsedShortRoom,
                nullptr,
                &parsedShortRoomKey);
            screenshare::ShareSessionConfig shareCommandConfig;
            shareCommandConfig.displayIndex = 1;
            shareCommandConfig.roomPort = 5001;
            shareCommandConfig.roomId = "room-alpha";
            shareCommandConfig.roomName = "Alpha Room";
            shareCommandConfig.roomPassword = "pw";
            shareCommandConfig.signalingStunServer = "stun.example:19302";
            shareCommandConfig.reportPath = "sender-report.zip";
            shareCommandConfig.audioDeviceId = "audio-device";
            shareCommandConfig.stream.fps = 30;
            shareCommandConfig.stream.adaptResolution = false;
            shareCommandConfig.stream.outputResolution = screenshare::SessionResolution{1920, 1080};
            const QStringList shareRoomArguments = toQStringList(
                screenshare::BuildShareArguments(shareCommandConfig));
            const bool shareRoomArgumentsOk = shareRoomArguments == QStringList{
                "--share-room", "5001",
                "--signal-room", "room-alpha",
                "--signal-room-name", "Alpha Room",
                "--signal-room-password", "pw",
                "--display", "1",
                "--fps", "30",
                "--no-adapt-resolution",
                "--width", "1920",
                "--height", "1080",
                "--audio-device-id", "audio-device",
                "--signal-stun", "stun.example:19302",
                "--save-report", "sender-report.zip",
            };
            screenshare::WatchSessionConfig watchCommandConfig;
            watchCommandConfig.listenPort = 5000;
            watchCommandConfig.roomId = "room-alpha";
            watchCommandConfig.signalingStunServer = "stun.example:19302";
            watchCommandConfig.udpAccessCode = "room-key";
            watchCommandConfig.reportPath = "receiver-report.zip";
            watchCommandConfig.muted = true;
            watchCommandConfig.previewLatencyMs = 125;
            watchCommandConfig.audioPlaybackVolumePercent = 75;
            const QStringList watchRoomArguments = toQStringList(
                screenshare::BuildWatchArguments(watchCommandConfig));
            const bool watchRoomArgumentsOk = watchRoomArguments == QStringList{
                "--watch", "5000",
                "--signal-room", "room-alpha",
                "--preview-latency-ms", "125",
                "--audio-playback-volume", "75",
                "--audio-playback-muted",
                "--signal-stun", "stun.example:19302",
                "--access-code", "room-key",
                "--save-report", "receiver-report.zip",
            };
            screenshare::ShareSessionConfig directShareConfig;
            directShareConfig.connectionMode = screenshare::ShareConnectionMode::DirectTargets;
            directShareConfig.displayIndex = 2;
            directShareConfig.targets = {"10.0.0.2:5000", "10.0.0.3:5000"};
            directShareConfig.stream.fps = 45;
            directShareConfig.stream.adaptResolution = true;
            directShareConfig.allowPlaintext = true;
            const QStringList directShareArguments = toQStringList(
                screenshare::BuildShareArguments(directShareConfig));
            const bool directShareArgumentsOk = directShareArguments == QStringList{
                "--share", "10.0.0.2:5000",
                "--share-target", "10.0.0.3:5000",
                "--display", "2",
                "--fps", "45",
                "--allow-plaintext",
            };
            screenshare::ShareSessionConfig inviteShareConfig;
            inviteShareConfig.connectionMode = screenshare::ShareConnectionMode::ManualInvite;
            inviteShareConfig.displayIndex = 0;
            inviteShareConfig.localInvite = "ss1e:room";
            inviteShareConfig.watcherInvites = {"ss1e:watcher-a", "ss1e:watcher-b"};
            inviteShareConfig.stream.fps = 60;
            const QStringList inviteShareArguments = toQStringList(
                screenshare::BuildShareArguments(inviteShareConfig));
            const bool inviteShareArgumentsOk = inviteShareArguments == QStringList{
                "--share", "ss1e:watcher-a",
                "--local-invite", "ss1e:room",
                "--share-target", "ss1e:watcher-b",
                "--display", "0",
                "--fps", "60",
            };
            screenshare::WatchSessionConfig nearbyWatchConfig;
            nearbyWatchConfig.connectionMode = screenshare::WatchConnectionMode::DirectListen;
            nearbyWatchConfig.listenPort = 5000;
            nearbyWatchConfig.lanAdvertise = true;
            nearbyWatchConfig.muted = true;
            const QStringList nearbyWatchArguments = toQStringList(
                screenshare::BuildWatchArguments(nearbyWatchConfig));
            const bool nearbyWatchArgumentsOk = nearbyWatchArguments == QStringList{
                "--watch", "5000",
                "--lan-advertise",
                "--preview-latency-ms", "100",
                "--audio-playback-volume", "100",
                "--audio-playback-muted",
            };
            screenshare::WatchSessionConfig inviteWatchConfig;
            inviteWatchConfig.connectionMode = screenshare::WatchConnectionMode::ManualInvite;
            inviteWatchConfig.listenPort = 5001;
            inviteWatchConfig.peerInvite = "ss1e:sender";
            const QStringList inviteWatchArguments = toQStringList(
                screenshare::BuildWatchArguments(inviteWatchConfig));
            const bool inviteWatchArgumentsOk = inviteWatchArguments == QStringList{
                "--watch", "5001",
                "--peer-invite", "ss1e:sender",
                "--preview-latency-ms", "100",
                "--audio-playback-volume", "100",
            };
            bool missingDirectShareRejected = false;
            try {
                screenshare::ShareSessionConfig invalidDirectShareConfig;
                invalidDirectShareConfig.connectionMode = screenshare::ShareConnectionMode::DirectTargets;
                static_cast<void>(screenshare::BuildShareArguments(invalidDirectShareConfig));
            } catch (const std::invalid_argument&) {
                missingDirectShareRejected = true;
            }
            const auto runtimeSettings =
                screenshare::ParseRuntimeStreamSettingsRequest("resolution = 1920x1080\n");
            const auto ignoredRuntimeSettings =
                screenshare::ParseRuntimeStreamSettingsRequest("resolution = 1919x1080\n");
            const bool runtimeResolutionOk =
                runtimeSettings &&
                runtimeSettings->resolution &&
                runtimeSettings->resolution->mode == screenshare::RuntimeResolutionMode::Fixed &&
                runtimeSettings->resolution->width == 1920 &&
                runtimeSettings->resolution->height == 1080 &&
                !ignoredRuntimeSettings;
            screenshare::MemorySessionRuntimeControl memoryControl;
            screenshare::RuntimeResolutionRequest memoryResolution;
            memoryResolution.mode = screenshare::RuntimeResolutionMode::Native;
            screenshare::RuntimeStreamSettingsRequest memorySettings;
            memorySettings.resolution = memoryResolution;
            memoryControl.RequestStreamSettings(memorySettings);
            const auto memorySettingsResult = memoryControl.TakeStreamSettingsRequest();
            const bool memoryResolutionOk =
                memorySettingsResult &&
                memorySettingsResult->resolution &&
                memorySettingsResult->resolution->mode == screenshare::RuntimeResolutionMode::Native &&
                !memoryControl.TakeStreamSettingsRequest();
            const bool memoryStopInitiallyClear = !memoryControl.StopRequested();
            memoryControl.RequestStop();
            const bool memoryStopRequested = memoryControl.StopRequested();
            memoryControl.Reset();
            const bool memoryControlReset =
                !memoryControl.StopRequested() &&
                !memoryControl.TakeStreamSettingsRequest();
            class AppSessionSelfTestObserver final : public screenshare::ISessionEventSink {
            public:
                void OnSessionEvent(const screenshare::SessionEvent& event) override
                {
                    std::scoped_lock lock(mutex_);
                    if (screenshare::IsTerminalSessionState(event.status.state)) {
                        terminalState_ = event.status.state;
                        done_ = true;
                        condition_.notify_all();
                    }
                }

                bool wait()
                {
                    std::unique_lock lock(mutex_);
                    return condition_.wait_for(lock, std::chrono::seconds(5), [this] {
                        return done_;
                    });
                }

                bool failed() const
                {
                    std::scoped_lock lock(mutex_);
                    return done_ && terminalState_ == screenshare::SessionState::Failed;
                }

            private:
                mutable std::mutex mutex_;
                std::condition_variable condition_;
                screenshare::SessionState terminalState_ = screenshare::SessionState::Idle;
                bool done_ = false;
            };
            AppSessionSelfTestObserver typedSessionSelfTestObserver;
            bool typedSessionValidationOk = false;
            {
                screenshare::ScreenShareSession appSelfTestSession;
                screenshare::ShareSessionConfig invalidDirectShareConfig;
                invalidDirectShareConfig.connectionMode = screenshare::ShareConnectionMode::DirectTargets;
                appSelfTestSession.StartShare(invalidDirectShareConfig, typedSessionSelfTestObserver);
                const bool typedSessionFinished = typedSessionSelfTestObserver.wait();
                const screenshare::SessionStatus typedSessionStatus = appSelfTestSession.GetStatus();
                typedSessionValidationOk =
                    typedSessionFinished &&
                    typedSessionSelfTestObserver.failed() &&
                    typedSessionStatus.state == screenshare::SessionState::Failed;
            }
            const bool selfTestOk = peers.size() == 1 &&
                peers.front().host == "100.64.0.2" &&
                invite.startsWith("ss1e:") &&
                commandInvite.startsWith("nat_invite=screenshare-invite-v1") &&
                compactCommandInvite.startsWith("ss1p:") &&
                natStatus == "receiving" &&
                resourcesAvailable &&
                directShareArgumentsOk &&
                inviteShareArgumentsOk &&
                nearbyWatchArgumentsOk &&
                inviteWatchArgumentsOk &&
                missingDirectShareRejected &&
                displays.size() == 2 &&
                displays[1].index == 1 &&
                displays[1].width == 1920 &&
                displays[1].left == -1920 &&
                qhdResolutionChoices.contains(QSize(2560, 1440)) &&
                qhdResolutionChoices.contains(QSize(1920, 1080)) &&
                !qhdResolutionChoices.contains(QSize(3840, 2160)) &&
                fhdResolutionChoices.contains(QSize(1920, 1080)) &&
                !fhdResolutionChoices.contains(QSize(2560, 1440)) &&
                activeRooms.size() == 2 &&
                activeRooms[0].roomId == "room-alpha" &&
                activeRooms[0].name == "Alpha Room" &&
                activeRooms[0].passwordProtected &&
                activeRooms[1].roomId == "room-beta" &&
                activeRooms[1].name == "Beta Room" &&
                !activeRooms[1].passwordProtected &&
                parsedRoomLink &&
                parsedRoomServer == defaultSignalServer() &&
                parsedRoom == "room-abc_123" &&
                parsedRoomKey == "ABCDE-23456-FGHIJ-789PQ" &&
                parsedRoomPort == 0 &&
                parsedLegacyRoomLink &&
                parsedLegacyRoom == "room-abc_123" &&
                parsedLegacyRoomPort == 5001 &&
                parsedShortRoomLink &&
                parsedShortRoom == "room-public" &&
                parsedShortRoomKey.isEmpty() &&
                shareRoomArgumentsOk &&
                watchRoomArgumentsOk &&
                runtimeResolutionOk &&
                memoryResolutionOk &&
                memoryStopInitiallyClear &&
                memoryStopRequested &&
                memoryControlReset &&
                typedSessionValidationOk;
            return selfTestOk ? 0 : 2;
        }
    }

    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    app.setWindowIcon(appIcon());
    app.setStyleSheet(appStyleSheet(true));

    if (guiSmokeTest) {
        return 0;
    }

    if (classicUi) {
        MainWindow window;
        window.show();
        return app.exec();
    }

    AppShellWindow window;

    auto* sessionBackend = new QtSessionBackend(&window);
    HomeWindow* homeWindow = nullptr;
    CreateRoomWindow* createRoomWindow = nullptr;
    JoinRoomWindow* joinRoomWindow = nullptr;
    ActiveShareWindow* activeShareWindow = nullptr;
    ActiveWatchWindow* activeWatchWindow = nullptr;
    auto showHome = [&window, &homeWindow] {
        window.setCurrentWidget(homeWindow);
    };
    auto showCreateRoom = [&window, &createRoomWindow] {
        window.setCurrentWidget(createRoomWindow);
    };
    auto showJoinRoom = [&window, &joinRoomWindow] {
        joinRoomWindow->refreshRooms();
        window.setCurrentWidget(joinRoomWindow);
    };
    auto showActiveShare = [&window, &activeShareWindow](const ShareSessionUiState& session) {
        if (!window.isMaximized()) {
            window.resize(std::max(window.width(), 940), std::max(window.height(), 690));
        }
        activeShareWindow->setSession(session);
        window.setCurrentWidget(activeShareWindow);
    };
    auto showActiveWatch = [&window, &activeWatchWindow](const WatchSessionUiState& session) {
        if (!window.isMaximized()) {
            window.resize(std::max(window.width(), 940), std::max(window.height(), 690));
        }
        window.setCurrentWidget(activeWatchWindow);
        activeWatchWindow->setSession(session);
    };

    homeWindow = new HomeWindow(HomeWindow::Actions{
        [&showCreateRoom] { showCreateRoom(); },
        [&showJoinRoom] { showJoinRoom(); },
        [&showActiveWatch](const WatchSessionUiState& session) { showActiveWatch(session); },
    });
    createRoomWindow = new CreateRoomWindow(sessionBackend, CreateRoomWindow::Actions{
        [&showHome] { showHome(); },
        [&showActiveShare](const ShareSessionUiState& session) { showActiveShare(session); },
    });
    activeShareWindow = new ActiveShareWindow(sessionBackend, ActiveShareWindow::Actions{
        [&window, &homeWindow, &createRoomWindow] {
            if (createRoomWindow != nullptr) {
                createRoomWindow->resetForNextRoom();
            }
            window.setCurrentWidget(homeWindow);
            if (!window.isMaximized()) {
                window.resize(820, 640);
            }
        },
    });
    joinRoomWindow = new JoinRoomWindow(sessionBackend, JoinRoomWindow::Actions{
        [&showHome] { showHome(); },
        [&showActiveWatch](const WatchSessionUiState& session) { showActiveWatch(session); },
    });
    activeWatchWindow = new ActiveWatchWindow(sessionBackend, ActiveWatchWindow::Actions{
        [&window, &homeWindow] {
            window.setCurrentWidget(homeWindow);
            if (!window.isMaximized()) {
                window.resize(820, 640);
            }
        },
    });

    window.addPage(homeWindow);
    window.addPage(createRoomWindow);
    window.addPage(joinRoomWindow);
    window.addPage(activeShareWindow);
    window.addPage(activeWatchWindow);
    window.resize(820, 640);
    window.setMinimumSize(740, 600);
    window.show();
    return app.exec();
}
