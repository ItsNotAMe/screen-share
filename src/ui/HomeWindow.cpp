#include "ui/HomeWindow.h"

#include "ui/UiStyle.h"

#include <QtCore/QFile>
#include <QtCore/QIODevice>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRectF>
#include <QtCore/QSize>
#include <QtCore/QUrl>
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
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace {

constexpr const char* kDefaultSignalServer = "https://screenshare-signaling.bit-yeet.workers.dev";
constexpr const char* kDefaultStunServer = "stun.l.google.com:19302";

std::string toStdUtf8(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<size_t>(bytes.size()));
}

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

QLabel* label(const QString& text, const char* objectName)
{
    auto* widget = new QLabel(text);
    widget->setObjectName(QString::fromUtf8(objectName));
    widget->setWordWrap(true);
    widget->setAttribute(Qt::WA_TransparentForMouseEvents);
    return widget;
}

QLabel* iconLabel(const char* iconName, int size, const QString& color = QStringLiteral("#eaf5f2"))
{
    auto* widget = new QLabel;
    widget->setFixedSize(size, size);
    widget->setAlignment(Qt::AlignCenter);
    widget->setAttribute(Qt::WA_TransparentForMouseEvents);
    const QString path = QStringLiteral(":/screenshare/ui/icons/%1.svg").arg(QString::fromUtf8(iconName));
    widget->setPixmap(renderSvgResource(path, QSize(size, size), color));
    return widget;
}

QPushButton* actionButton(const QString& text, const QString& objectName, const char* iconName)
{
    auto* button = new QPushButton(text);
    button->setObjectName(objectName);
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumHeight(42);
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

QFrame* separator()
{
    auto* line = new QFrame;
    line->setObjectName("HomeDivider");
    line->setFrameShape(QFrame::NoFrame);
    line->setFixedHeight(1);
    return line;
}

QVector<HomeActiveRoom> parseRooms(const QByteArray& payload)
{
    const QJsonDocument document = QJsonDocument::fromJson(payload);
    if (!document.isObject()) {
        return {};
    }

    const QJsonArray rooms = document.object().value(QStringLiteral("rooms")).toArray();
    QVector<HomeActiveRoom> result;
    result.reserve(rooms.size());
    for (const QJsonValue& value : rooms) {
        const QJsonObject object = value.toObject();
        HomeActiveRoom room;
        room.roomId = object.value(QStringLiteral("roomId")).toString().trimmed();
        if (room.roomId.isEmpty()) {
            continue;
        }
        room.name = object.value(QStringLiteral("name")).toString().trimmed();
        if (room.name.isEmpty()) {
            room.name = room.roomId;
        }
        room.peerCount = std::max(0, object.value(QStringLiteral("peerCount")).toInt(0));
        room.passwordProtected = object.value(QStringLiteral("passwordProtected")).toBool(false);
        room.updatedAt = static_cast<qint64>(object.value(QStringLiteral("updatedAt")).toDouble(0));
        result.push_back(std::move(room));
    }
    std::sort(result.begin(), result.end(), [](const HomeActiveRoom& lhs, const HomeActiveRoom& rhs) {
        if (lhs.updatedAt != rhs.updatedAt) {
            return lhs.updatedAt > rhs.updatedAt;
        }
        return lhs.name.localeAwareCompare(rhs.name) < 0;
    });
    return result;
}

} // namespace

HomeWindow::HomeWindow(Actions actions, QWidget* parent)
    : QWidget(parent), actions_(std::move(actions))
{
    setObjectName("HomeWindow");
    setWindowTitle("ScreenShare");
    setWindowIcon(QIcon(QStringLiteral(":/screenshare/brand/screenshare-mark.svg")));
    setStyleSheet(uiStyleSheet());
    resize(820, 640);
    setMinimumSize(740, 560);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(buildTopBar());
    root->addWidget(separator());
    root->addWidget(buildMainMenu(), 1);

    roomNetwork_ = new QNetworkAccessManager(this);
    refreshRooms();
}

QWidget* HomeWindow::buildTopBar()
{
    auto* frame = new QFrame;
    frame->setObjectName("HomeTopBar");

    auto* layout = new QHBoxLayout(frame);
    layout->setContentsMargins(30, 20, 160, 18);
    layout->setSpacing(14);

    auto* mark = new QLabel;
    mark->setFixedSize(46, 46);
    mark->setPixmap(renderSvgResource(
        QStringLiteral(":/screenshare/brand/screenshare-mark.svg"),
        QSize(46, 46)));
    layout->addWidget(mark, 0, Qt::AlignVCenter);

    layout->addWidget(label("ScreenShare", "HomeBrand"), 1, Qt::AlignVCenter);
    return frame;
}

QWidget* HomeWindow::buildMainMenu()
{
    auto* host = new QWidget;
    host->setObjectName("HomeContent");
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(42, 32, 42, 32);
    layout->setSpacing(24);

    auto* actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(16);
    actions->addStretch(1);
    actions->addWidget(buildActionPanel(
        "share",
        "Start Sharing",
        "Share your screen",
        "HomePrimary",
        actions_.createRoom));
    actions->addWidget(buildActionPanel(
        "watch",
        "Join Room",
        "Watch a stream",
        "HomeSecondary",
        actions_.joinRoom));
    actions->addStretch(1);
    layout->addLayout(actions);

    layout->addWidget(buildRoomPanel(), 1);
    return host;
}

QWidget* HomeWindow::buildActionPanel(
    const char* iconName,
    const QString& title,
    const QString& detail,
    const QString& buttonObjectName,
    std::function<void()> action)
{
    auto* button = new QPushButton;
    button->setObjectName(buttonObjectName);
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumWidth(300);
    button->setMaximumWidth(360);
    button->setMinimumHeight(94);
    button->setMaximumHeight(104);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* layout = new QHBoxLayout(button);
    layout->setContentsMargins(24, 12, 24, 12);
    layout->setSpacing(16);
    layout->addStretch(1);
    layout->addWidget(iconLabel(iconName, 38, QStringLiteral("#ffffff")), 0, Qt::AlignVCenter);

    auto* textBlock = new QWidget(button);
    textBlock->setObjectName("HomeActionTextBlock");
    textBlock->setAttribute(Qt::WA_TransparentForMouseEvents);
    textBlock->setAutoFillBackground(false);
    textBlock->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    const bool primary = buttonObjectName == QStringLiteral("HomePrimary");
    auto* text = new QVBoxLayout(textBlock);
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(0);
    auto* titleLabel = label(title, primary ? "HomeActionTitlePrimary" : "HomeActionTitle");
    auto* detailLabel = label(detail, primary ? "HomeActionDetailPrimary" : "HomeActionDetail");
    titleLabel->setWordWrap(false);
    detailLabel->setWordWrap(false);
    titleLabel->setAutoFillBackground(false);
    detailLabel->setAutoFillBackground(false);
    titleLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    detailLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    text->addWidget(titleLabel, 0, Qt::AlignLeft);
    text->addWidget(detailLabel, 0, Qt::AlignLeft);
    layout->addWidget(textBlock, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    QObject::connect(button, &QPushButton::clicked, button, [action = std::move(action)] {
        if (action) {
            action();
        }
    });
    return button;
}

QWidget* HomeWindow::buildRoomPanel()
{
    auto* panel = new QFrame;
    panel->setObjectName("HomePanel");

    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    auto* heading = new QHBoxLayout;
    heading->setContentsMargins(0, 0, 0, 0);
    heading->addWidget(label("Quick Join", "HomeSectionTitle"), 1);
    refreshRoomsButton_ = actionButton("Refresh", "HomeGhost", "refresh");
    QObject::connect(refreshRoomsButton_, &QPushButton::clicked, this, [this] {
        refreshRooms();
    });
    heading->addWidget(refreshRoomsButton_);
    layout->addLayout(heading);

    auto* roomList = new QFrame;
    roomList->setObjectName("HomeRoomList");
    roomListLayout_ = new QVBoxLayout(roomList);
    roomListLayout_->setContentsMargins(0, 0, 0, 0);
    roomListLayout_->setSpacing(0);
    roomStatusLabel_ = label("Loading rooms...", "HomeEmptyState");
    roomStatusLabel_->setAlignment(Qt::AlignCenter);
    roomListLayout_->addWidget(roomStatusLabel_, 1);
    layout->addWidget(roomList, 1);

    auto* footer = new QHBoxLayout;
    footer->setSpacing(10);
    auto* roomMetric = buildMetric("-", "rooms");
    roomCountValue_ = roomMetric->findChild<QLabel*>(QStringLiteral("HomeMetricValue"));
    footer->addWidget(roomMetric);
    auto* peerMetric = buildMetric("-", "peers");
    peerCountValue_ = peerMetric->findChild<QLabel*>(QStringLiteral("HomeMetricValue"));
    footer->addWidget(peerMetric);
    layout->addLayout(footer);

    return panel;
}

QWidget* HomeWindow::buildRoomRow(const HomeActiveRoom& room)
{
    auto* row = new QFrame;
    row->setObjectName("HomeRoomRow");
    row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    row->setMinimumHeight(76);

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(14, 12, 10, 12);
    layout->setSpacing(12);
    layout->addWidget(iconLabel(
        room.passwordProtected ? "lock" : "room",
        28,
        room.passwordProtected ? QStringLiteral("#ffd56a") : QStringLiteral("#62e8dc")));

    auto* text = new QVBoxLayout;
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(1);
    text->addWidget(label(room.name, "HomeInfoPrimary"));
    const QString peerText = room.peerCount == 1 ? QStringLiteral("1 peer") : QStringLiteral("%1 peers").arg(room.peerCount);
    text->addWidget(label(peerText, "HomeInfoSecondary"));
    layout->addLayout(text, 1);

    auto* status = label(
        room.passwordProtected ? QStringLiteral("Locked") : QStringLiteral("Public"),
        room.passwordProtected ? "HomeLockedStatus" : "HomePublicStatus");
    status->setAlignment(Qt::AlignCenter);
    status->setFixedHeight(22);
    layout->addWidget(status, 0, Qt::AlignVCenter);

    auto* join = actionButton("Join", "HomeTinyButton", "watch");
    join->setMinimumWidth(64);
    QObject::connect(join, &QPushButton::clicked, this, [this, room] {
        if (!actions_.quickJoinRoom) {
            return;
        }

        QString password;
        if (room.passwordProtected) {
            bool ok = false;
            password = QInputDialog::getText(
                this,
                "Room password",
                QStringLiteral("Enter password for %1").arg(room.name),
                QLineEdit::Password,
                QString(),
                &ok);
            if (!ok) {
                return;
            }
        }

        WatchSessionUiState state;
        state.config.connectionMode = screenshare::WatchConnectionMode::Room;
        state.config.listenPort = 5000;
        state.config.roomId = toStdUtf8(room.roomId);
        state.config.roomPassword = toStdUtf8(password);
        state.config.signalingStunServer = kDefaultStunServer;
        state.config.reportPath = "receiver-report.zip";
        state.config.playAudio = true;
        state.config.previewLatencyMs = 100;
        state.config.audioPlaybackVolumePercent = 100;
        state.roomId = room.roomId;
        state.roomName = room.name;
        state.passwordProtected = room.passwordProtected;
        actions_.quickJoinRoom(state);
    });
    layout->addWidget(join, 0, Qt::AlignVCenter);
    return row;
}

void HomeWindow::refreshRooms()
{
    if (roomNetwork_ == nullptr) {
        return;
    }

    showRoomStatus("Loading rooms...");
    if (refreshRoomsButton_ != nullptr) {
        refreshRoomsButton_->setEnabled(false);
    }

    QNetworkRequest request(QUrl(QString::fromUtf8(kDefaultSignalServer) + QStringLiteral("/rooms")));
    request.setHeader(QNetworkRequest::UserAgentHeader, "ScreenShareUi");
    QNetworkReply* reply = roomNetwork_->get(request);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (refreshRoomsButton_ != nullptr) {
            refreshRoomsButton_->setEnabled(true);
        }

        if (reply->error() != QNetworkReply::NoError) {
            showRoomStatus("Could not load rooms");
            return;
        }

        updateRooms(parseRooms(reply->readAll()));
    });
}

void HomeWindow::updateRooms(const QVector<HomeActiveRoom>& rooms)
{
    if (roomListLayout_ == nullptr) {
        return;
    }

    while (QLayoutItem* item = roomListLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    int peerCount = 0;
    for (const HomeActiveRoom& room : rooms) {
        peerCount += room.peerCount;
        roomListLayout_->addWidget(buildRoomRow(room));
    }

    if (rooms.isEmpty()) {
        roomStatusLabel_ = label("No open rooms", "HomeEmptyState");
        roomStatusLabel_->setAlignment(Qt::AlignCenter);
        roomListLayout_->addWidget(roomStatusLabel_, 1);
    } else {
        roomStatusLabel_ = nullptr;
        roomListLayout_->addStretch(1);
    }

    if (roomCountValue_ != nullptr) {
        roomCountValue_->setText(QString::number(rooms.size()));
    }
    if (peerCountValue_ != nullptr) {
        peerCountValue_->setText(QString::number(peerCount));
    }
}

void HomeWindow::showRoomStatus(const QString& message)
{
    if (roomListLayout_ == nullptr) {
        return;
    }

    while (QLayoutItem* item = roomListLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    roomStatusLabel_ = label(message, "HomeEmptyState");
    roomStatusLabel_->setAlignment(Qt::AlignCenter);
    roomListLayout_->addWidget(roomStatusLabel_, 1);
    if (roomCountValue_ != nullptr) {
        roomCountValue_->setText("-");
    }
    if (peerCountValue_ != nullptr) {
        peerCountValue_->setText("-");
    }
}

QWidget* HomeWindow::buildMetric(const QString& value, const QString& labelText)
{
    auto* panel = new QFrame;
    panel->setObjectName("HomeMetric");
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(2);
    layout->addWidget(label(value, "HomeMetricValue"));
    layout->addWidget(label(labelText, "HomeMetricLabel"));
    return panel;
}
