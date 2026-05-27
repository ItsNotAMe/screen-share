#include "ui/JoinRoomWindow.h"

#include "ui/UiStyle.h"

#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QIODevice>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtCore/QRegularExpression>
#include <QtCore/QSizeF>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtSvg/QSvgRenderer>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLayoutItem>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace {

constexpr const char* kRoomLinkPrefix = "screenshare-room-v1;";
constexpr const char* kDefaultSignalServer = "https://screenshare-signaling.bit-yeet.workers.dev";
constexpr const char* kDefaultStunServer = "stun.l.google.com:19302";

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

std::string toStdUtf8(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<size_t>(bytes.size()));
}

bool validRoomId(const QString& roomId)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{3,96}$"));
    return pattern.match(roomId).hasMatch();
}

bool parseRoomLink(const QString& text, QString* roomId)
{
    const QString trimmed = text.trimmed();
    const int prefixIndex = trimmed.indexOf(QString::fromUtf8(kRoomLinkPrefix));
    if (prefixIndex < 0) {
        return false;
    }

    const QString payload = trimmed.mid(prefixIndex + QString::fromUtf8(kRoomLinkPrefix).size());
    const QStringList fields = payload.split(';', Qt::SkipEmptyParts);
    for (const QString& field : fields) {
        const int separator = field.indexOf('=');
        if (separator <= 0) {
            continue;
        }
        if (field.left(separator).trimmed() == "room") {
            const QString parsed = field.mid(separator + 1).trimmed();
            if (!validRoomId(parsed)) {
                return false;
            }
            if (roomId != nullptr) {
                *roomId = parsed;
            }
            return true;
        }
    }
    return false;
}

QVector<JoinRoomInfo> parseRooms(const QByteArray& payload)
{
    const QJsonDocument document = QJsonDocument::fromJson(payload);
    if (!document.isObject()) {
        return {};
    }

    const QJsonArray rooms = document.object().value(QStringLiteral("rooms")).toArray();
    QVector<JoinRoomInfo> result;
    result.reserve(rooms.size());
    for (const QJsonValue& value : rooms) {
        const QJsonObject object = value.toObject();
        JoinRoomInfo room;
        room.roomId = object.value(QStringLiteral("roomId")).toString().trimmed();
        if (!validRoomId(room.roomId)) {
            continue;
        }
        room.name = object.value(QStringLiteral("name")).toString().trimmed();
        if (room.name.isEmpty()) {
            room.name = room.roomId;
        }
        room.peerCount = std::max(0, object.value(QStringLiteral("peerCount")).toInt(0));
        room.passwordProtected =
            object.value(QStringLiteral("passwordProtected")).toBool(
                object.value(QStringLiteral("requiresRoomKey")).toBool(false));
        room.updatedAt = static_cast<qint64>(object.value(QStringLiteral("updatedAt")).toDouble(0));
        result.push_back(std::move(room));
    }
    std::sort(result.begin(), result.end(), [](const JoinRoomInfo& lhs, const JoinRoomInfo& rhs) {
        if (lhs.updatedAt != rhs.updatedAt) {
            return lhs.updatedAt > rhs.updatedAt;
        }
        return lhs.name.localeAwareCompare(rhs.name) < 0;
    });
    return result;
}

} // namespace

JoinRoomWindow::JoinRoomWindow(QtSessionBackend* backend, Actions actions, QWidget* parent)
    : QWidget(parent), backend_(backend), actions_(std::move(actions))
{
    setObjectName("JoinRoomWindow");
    setWindowTitle("Join Room - ScreenShare");
    setWindowIcon(QIcon(QStringLiteral(":/screenshare/brand/screenshare-mark.svg")));
    setStyleSheet(uiStyleSheet());
    resize(820, 640);
    setMinimumSize(740, 600);

    if (backend_ == nullptr) {
        backend_ = new QtSessionBackend(this);
    }
    installBackendHandlers();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(buildShell(), 1);

    roomNetwork_ = new QNetworkAccessManager(this);
    refreshRooms();
}

QWidget* JoinRoomWindow::buildShell()
{
    auto* host = new QWidget;
    host->setObjectName("JoinRoomContent");
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(36, 24, 36, 24);
    layout->setSpacing(14);
    layout->addWidget(buildHeader());
    layout->addWidget(buildRoomsSection(), 1);
    layout->addWidget(buildLinkSection());
    layout->addWidget(buildAdvancedSection());
    statusLabel_ = textLabel("Ready to join", "JoinStatusIdle");
    layout->addWidget(statusLabel_);
    return host;
}

QWidget* JoinRoomWindow::buildHeader()
{
    auto* header = new QWidget;
    header->setObjectName("RoomTransparentBlock");
    header->setFixedHeight(40);
    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);

    auto* back = iconButton("", "RoomBackButton", "back");
    back->setFixedSize(36, 36);
    connect(back, &QPushButton::clicked, this, [this] {
        if (backend_ != nullptr && backend_->isRunning()) {
            QMessageBox::information(this, "Watching is running", "Stop watching before returning to the home screen.");
            return;
        }
        if (actions_.back) {
            actions_.back();
        }
    });
    layout->addWidget(back, 0, Qt::AlignVCenter);
    layout->addWidget(textLabel("Join Room", "RoomHeaderTitle"), 1, Qt::AlignVCenter);
    return header;
}

QWidget* JoinRoomWindow::buildRoomsSection()
{
    auto* section = new QWidget;
    section->setObjectName("JoinTransparentBlock");
    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* heading = new QHBoxLayout;
    heading->setContentsMargins(0, 0, 0, 0);
    heading->addWidget(textLabel("Active Rooms (Internet)", "JoinSectionTitle"));
    heading->addStretch(1);
    refreshButton_ = iconButton("Refresh", "HomeGhost", "refresh");
    connect(refreshButton_, &QPushButton::clicked, this, [this] {
        refreshRooms();
    });
    heading->addWidget(refreshButton_);
    layout->addLayout(heading);

    auto* list = new QFrame;
    list->setObjectName("JoinRoomList");
    roomListLayout_ = new QVBoxLayout(list);
    roomListLayout_->setContentsMargins(0, 0, 0, 0);
    roomListLayout_->setSpacing(0);
    roomStatusLabel_ = textLabel("Loading rooms...", "HomeEmptyState");
    roomStatusLabel_->setAlignment(Qt::AlignCenter);
    roomListLayout_->addWidget(roomStatusLabel_, 1);
    layout->addWidget(list, 1);
    return section;
}

QWidget* JoinRoomWindow::buildRoomRow(const JoinRoomInfo& room)
{
    auto* row = new QFrame;
    row->setObjectName("JoinRoomRow");
    row->setMinimumHeight(72);
    row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(16, 10, 14, 10);
    layout->setSpacing(14);
    layout->addWidget(iconLabel(
        room.passwordProtected ? "lock" : "room",
        30,
        room.passwordProtected ? QStringLiteral("#ffd56a") : QStringLiteral("#62e8dc")));

    auto* text = new QVBoxLayout;
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(2);
    text->addWidget(textLabel(room.name, "JoinRoomName"));
    text->addWidget(textLabel(
        room.peerCount == 1 ? QStringLiteral("1 viewer") : QStringLiteral("%1 viewers").arg(room.peerCount),
        "JoinRoomMeta"));
    layout->addLayout(text, 1);

    auto* privacy = textLabel(room.passwordProtected ? "● Locked" : "● Public",
                              room.passwordProtected ? "JoinLockedStatus" : "JoinPublicStatus");
    privacy->setAlignment(Qt::AlignCenter);
    privacy->setFixedWidth(100);
    layout->addWidget(privacy, 0, Qt::AlignVCenter);

    auto* join = iconButton("Join", "JoinRoomButton", "watch");
    join->setFixedSize(86, 42);
    connect(join, &QPushButton::clicked, this, [this, room] {
        joinRoom(room);
    });
    layout->addWidget(join, 0, Qt::AlignVCenter);
    return row;
}

QWidget* JoinRoomWindow::buildLinkSection()
{
    auto* section = new QWidget;
    section->setObjectName("JoinTransparentBlock");
    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(textLabel("Or join with a room link", "JoinSectionTitle"));

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);
    roomLinkEdit_ = new QLineEdit;
    roomLinkEdit_->setObjectName("RoomLargeInput");
    roomLinkEdit_->setPlaceholderText("screenshare-room-v1;room=...");
    roomLinkEdit_->setFixedHeight(40);
    connect(roomLinkEdit_, &QLineEdit::returnPressed, this, [this] {
        joinRoomLink();
    });
    row->addWidget(roomLinkEdit_, 1);
    auto* paste = iconButton("Paste", "RoomSecondaryButton", "paste");
    paste->setFixedSize(104, 40);
    connect(paste, &QPushButton::clicked, this, [this] {
        pasteRoomLink();
    });
    row->addWidget(paste);
    linkJoinButton_ = iconButton("Join", "JoinRoomButton", "watch");
    linkJoinButton_->setFixedSize(96, 40);
    connect(linkJoinButton_, &QPushButton::clicked, this, [this] {
        joinRoomLink();
    });
    row->addWidget(linkJoinButton_);
    layout->addLayout(row);
    return section;
}

QWidget* JoinRoomWindow::buildAdvancedSection()
{
    auto* panel = new QFrame;
    panel->setObjectName("JoinAdvancedPanel");
    auto* layout = new QHBoxLayout(panel);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(12);
    layout->addWidget(iconLabel("settings", 22, QStringLiteral("#b7c5c1")));
    layout->addWidget(textLabel("Advanced", "JoinSectionTitle"));
    layout->addStretch(1);
    layout->addWidget(textLabel("Listen Port", "JoinRoomMeta"));
    listenPortSpin_ = new QSpinBox;
    listenPortSpin_->setObjectName("RoomSettingsInput");
    listenPortSpin_->setRange(1, 65535);
    listenPortSpin_->setValue(5000);
    listenPortSpin_->setFixedSize(120, 40);
    layout->addWidget(listenPortSpin_);
    return panel;
}

QPushButton* JoinRoomWindow::iconButton(const QString& text, const QString& objectName, const char* iconName)
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

QLabel* JoinRoomWindow::textLabel(const QString& text, const char* objectName)
{
    auto* label = new QLabel(text);
    label->setObjectName(QString::fromUtf8(objectName));
    label->setWordWrap(true);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    return label;
}

QLabel* JoinRoomWindow::iconLabel(const char* iconName, int size, const QString& color)
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

void JoinRoomWindow::refreshRooms()
{
    if (roomNetwork_ == nullptr) {
        return;
    }
    showRoomStatus("Loading rooms...");
    if (refreshButton_ != nullptr) {
        refreshButton_->setEnabled(false);
    }

    QNetworkRequest request(QUrl(QString::fromUtf8(kDefaultSignalServer) + QStringLiteral("/rooms")));
    request.setHeader(QNetworkRequest::UserAgentHeader, "ScreenShareUi");
    QNetworkReply* reply = roomNetwork_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (refreshButton_ != nullptr) {
            refreshButton_->setEnabled(true);
        }
        if (reply->error() != QNetworkReply::NoError) {
            showRoomStatus("Could not load rooms");
            return;
        }
        updateRooms(parseRooms(reply->readAll()));
    });
}

void JoinRoomWindow::updateRooms(const QVector<JoinRoomInfo>& rooms)
{
    if (roomListLayout_ == nullptr) {
        return;
    }
    while (QLayoutItem* item = roomListLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const JoinRoomInfo& room : rooms) {
        roomListLayout_->addWidget(buildRoomRow(room));
    }
    if (rooms.isEmpty()) {
        roomStatusLabel_ = textLabel("No open rooms", "HomeEmptyState");
        roomStatusLabel_->setAlignment(Qt::AlignCenter);
        roomListLayout_->addWidget(roomStatusLabel_, 1);
    } else {
        roomStatusLabel_ = nullptr;
        roomListLayout_->addStretch(1);
    }
}

void JoinRoomWindow::showRoomStatus(const QString& message)
{
    if (roomListLayout_ == nullptr) {
        return;
    }
    while (QLayoutItem* item = roomListLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    roomStatusLabel_ = textLabel(message, "HomeEmptyState");
    roomStatusLabel_->setAlignment(Qt::AlignCenter);
    roomListLayout_->addWidget(roomStatusLabel_, 1);
}

void JoinRoomWindow::pasteRoomLink()
{
    roomLinkEdit_->setText(QGuiApplication::clipboard()->text().trimmed());
    roomLinkEdit_->setFocus();
}

void JoinRoomWindow::joinRoom(const JoinRoomInfo& room)
{
    bool accepted = true;
    const QString password = room.passwordProtected ? promptPassword(room.name, &accepted) : QString();
    if (!accepted) {
        return;
    }
    startWatch(room.roomId, room.name, room.passwordProtected, password);
}

void JoinRoomWindow::joinRoomLink()
{
    QString roomId;
    if (!parseRoomLink(roomLinkEdit_->text(), &roomId)) {
        setStatus("Paste a valid room link first", "JoinStatusError");
        roomLinkEdit_->setFocus();
        return;
    }
    startWatch(roomId, roomId, false, QString());
}

void JoinRoomWindow::startWatch(
    const QString& roomId,
    const QString& roomName,
    bool passwordProtected,
    const QString& password)
{
    if (backend_ == nullptr) {
        setStatus("Session backend unavailable", "JoinStatusError");
        return;
    }
    if (backend_->isRunning()) {
        setStatus("A session is already running.", "JoinStatusError");
        return;
    }

    const WatchSessionUiState state = currentWatchUiState(roomId, roomName, passwordProtected, password);
    if (actions_.watchStarted) {
        actions_.watchStarted(state);
    }
}

QString JoinRoomWindow::promptPassword(const QString& roomName, bool* accepted)
{
    bool ok = false;
    const QString password = QInputDialog::getText(
        this,
        "Room password",
        QStringLiteral("Enter password for %1").arg(roomName),
        QLineEdit::Password,
        QString(),
        &ok);
    if (accepted != nullptr) {
        *accepted = ok;
    }
    return ok ? password : QString();
}

void JoinRoomWindow::installBackendHandlers()
{
    if (backend_ == nullptr) {
        return;
    }
    backend_->setStartedHandler([this] {
        setStatus("Watching started", "JoinStatusLive");
    });
    backend_->setErrorHandler([this](const QString& message) {
        setStatus(message.isEmpty() ? QStringLiteral("Watching failed") : message, "JoinStatusError");
    });
    backend_->setFinishedHandler([this](const QtSessionBackend::FinishInfo& info) {
        if (info.stopRequested) {
            setStatus("Watching stopped", "JoinStatusIdle");
        } else if (info.failed) {
            setStatus("Watching failed", "JoinStatusError");
        } else {
            setStatus("Watching finished", "JoinStatusIdle");
        }
    });
    backend_->setStatusHandler([this](const screenshare::SessionEvent& event) {
        if (!event.message.empty() && event.type == screenshare::SessionEventType::Issue) {
            setStatus(QString::fromStdString(event.message), "JoinStatusError");
            return;
        }
        switch (event.status.state) {
        case screenshare::SessionState::Starting:
            setStatus("Starting watch...", "JoinStatusConnecting");
            break;
        case screenshare::SessionState::Connecting:
            setStatus("Connecting...", "JoinStatusConnecting");
            break;
        case screenshare::SessionState::Live:
            setStatus("Watching", "JoinStatusLive");
            break;
        case screenshare::SessionState::Disconnected:
            setStatus("Disconnected", "JoinStatusConnecting");
            break;
        case screenshare::SessionState::Stopping:
            setStatus("Stopping...", "JoinStatusConnecting");
            break;
        case screenshare::SessionState::Failed:
            setStatus("Watching failed", "JoinStatusError");
            break;
        default:
            break;
        }
    });
}

screenshare::WatchSessionConfig JoinRoomWindow::currentConfig(const QString& roomId, const QString& password) const
{
    screenshare::WatchSessionConfig config;
    config.connectionMode = screenshare::WatchConnectionMode::Room;
    config.listenPort = static_cast<uint16_t>(listenPortSpin_->value());
    config.roomId = toStdUtf8(roomId);
    config.roomPassword = toStdUtf8(password);
    config.signalingStunServer = kDefaultStunServer;
    config.reportPath = "receiver-report.zip";
    config.playAudio = true;
    config.previewLatencyMs = 100;
    config.audioPlaybackVolumePercent = 100;
    return config;
}

WatchSessionUiState JoinRoomWindow::currentWatchUiState(
    const QString& roomId,
    const QString& roomName,
    bool passwordProtected,
    const QString& password) const
{
    WatchSessionUiState state;
    state.config = currentConfig(roomId, password);
    state.roomId = roomId;
    state.roomName = roomName;
    state.passwordProtected = passwordProtected;
    return state;
}

void JoinRoomWindow::setStatus(const QString& text, const QString& objectName)
{
    if (statusLabel_ == nullptr) {
        return;
    }
    statusLabel_->setObjectName(objectName);
    statusLabel_->setText(text);
    statusLabel_->style()->unpolish(statusLabel_);
    statusLabel_->style()->polish(statusLabel_);
}
