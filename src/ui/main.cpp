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
#include "ui/ScreenAwakeGuard.h"
#include "ui/UpdateManager.h"

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
#include <QtCore/QJsonValue>
#include <QtCore/QRegularExpression>
#include <QtCore/QSize>
#include <QtCore/QStringList>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVector>
#include <QtGui/QIcon>
#include <QtSvg/QSvgRenderer>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cmath>
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

bool canRenderUiIconSvg(const QString& name, const char* color)
{
    QByteArray svg = readUiIconSvg(name);
    if (svg.isEmpty()) {
        return false;
    }
    svg.replace("currentColor", color);
    return QSvgRenderer(svg).isValid();
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

struct ActiveRoom {
    QString roomId;
    QString name;
    int peerCount = 0;
    qint64 updatedAt = 0;
    qint64 expiresAt = 0;
    bool passwordProtected = false;
};

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
                "--preview-latency-ms", "40",
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
                "--preview-latency-ms", "40",
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
                screenshare::ParseRuntimeStreamSettingsRequest("resolution = 1920x1080\nhost_video_paused = true\n");
            const auto ignoredRuntimeSettings =
                screenshare::ParseRuntimeStreamSettingsRequest("resolution = 1919x1080\n");
            const bool runtimeResolutionOk =
                runtimeSettings &&
                runtimeSettings->resolution &&
                runtimeSettings->resolution->mode == screenshare::RuntimeResolutionMode::Fixed &&
                runtimeSettings->resolution->width == 1920 &&
                runtimeSettings->resolution->height == 1080 &&
                runtimeSettings->videoPaused &&
                *runtimeSettings->videoPaused &&
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

    if (guiSmokeTest) {
        return 0;
    }

    AppShellWindow window;

    auto* sessionBackend = new QtSessionBackend(&window);
    auto* screenAwakeGuard = new ScreenAwakeGuard(&window);
    HomeWindow* homeWindow = nullptr;
    CreateRoomWindow* createRoomWindow = nullptr;
    JoinRoomWindow* joinRoomWindow = nullptr;
    ActiveShareWindow* activeShareWindow = nullptr;
    ActiveWatchWindow* activeWatchWindow = nullptr;
    auto showHome = [&window, &homeWindow, screenAwakeGuard] {
        screenAwakeGuard->setActive(false);
        if (homeWindow != nullptr) {
            homeWindow->refreshRooms();
        }
        window.setCurrentWidget(homeWindow);
    };
    auto showCreateRoom = [&window, &createRoomWindow] {
        window.setCurrentWidget(createRoomWindow);
    };
    auto showJoinRoom = [&window, &joinRoomWindow] {
        joinRoomWindow->refreshRooms();
        window.setCurrentWidget(joinRoomWindow);
    };
    auto showActiveShare = [&window, &activeShareWindow, screenAwakeGuard](const ShareSessionUiState& session) {
        screenAwakeGuard->setActive(true);
        if (!window.isMaximized()) {
            window.resize(std::max(window.width(), 940), std::max(window.height(), 690));
        }
        activeShareWindow->setSession(session);
        window.setCurrentWidget(activeShareWindow);
    };
    auto showActiveWatch = [&window, &activeWatchWindow, screenAwakeGuard](const WatchSessionUiState& session) {
        screenAwakeGuard->setActive(true);
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
        [&window, &showHome, &createRoomWindow] {
            if (createRoomWindow != nullptr) {
                createRoomWindow->resetForNextRoom();
            }
            showHome();
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
        [&window, &showHome] {
            showHome();
            if (!window.isMaximized()) {
                window.resize(820, 640);
            }
        },
        [&window, &showHome] {
            showHome();
            if (!window.isMaximized()) {
                window.resize(820, 640);
            }
            window.showToast("The host ended the session.");
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

    auto* updateManager = new UpdateManager(&window, &window);
    QTimer::singleShot(1500, updateManager, [updateManager] {
        updateManager->checkForUpdates();
    });
    return app.exec();
}
