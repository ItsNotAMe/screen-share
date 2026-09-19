#include "ui/HomeWindow.h"

#include "ui/UiStyle.h"

#include <QtCore/QFile>
#include <QtCore/QIODevice>
#include <QtCore/QRectF>
#include <QtCore/QSize>
#include <QtGui/QIcon>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtSvg/QSvgRenderer>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QLineEdit>
#include <QtCore/QSet>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
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

QLabel* label(const QString& text, const char* objectName)
{
    auto* widget = new QLabel(text);
    widget->setObjectName(QString::fromUtf8(objectName));
    widget->setTextFormat(Qt::PlainText);
    widget->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
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

} // namespace

HomeWindow::HomeWindow(Actions actions, QWidget* parent)
    : QWidget(parent), actions_(std::move(actions))
{
    setObjectName("HomeWindow");
    setWindowTitle("ScreenShare");
    setWindowIcon(QIcon(QStringLiteral(":/screenshare/brand/screenshare-mark.svg")));
    setStyleSheet(uiStyleSheet());
    resize(820, 640);
    setMinimumSize(700, 500);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(buildMainMenu(), 1);

}

RoomDirectoryWidget::RoomDirectoryWidget(std::function<void()> refresh, std::function<void(const QString&)> join, QWidget* parent)
    : QWidget(parent), request_(std::move(refresh)), join_(std::move(join)) {
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0,0,0,0);
    layout->addWidget(buildRoomPanel());
    showRoomStatus("Connecting to room list…");
}

QWidget* HomeWindow::buildRoomPanel() {
    rooms_ = new RoomDirectoryWidget(actions_.requestRooms, actions_.openRoom, this);
    return rooms_;
}
void HomeWindow::refreshRooms() { rooms_->refreshRooms(); }
void HomeWindow::setPushedRooms(const QVector<HomeActiveRoom>& rooms, const QString& unavailable) { rooms_->setPushedRooms(rooms, unavailable); }

QWidget* HomeWindow::buildMainMenu()
{
    auto* host = new QWidget;
    host->setObjectName("HomeContent");
    auto* layout = new QVBoxLayout(host);
    UiSpacing::applyPage(layout);

    auto* actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(UiSpacing::SectionGap);
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
    button->setAccessibleName(title);
    button->setObjectName(buttonObjectName);
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumWidth(300);
    button->setMinimumHeight(94);
    button->setMaximumHeight(104);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* layout = new QHBoxLayout(button);
    layout->setContentsMargins(24, 12, 24, 12);
    layout->setSpacing(16);
    layout->addWidget(iconLabel(iconName, 38, buttonObjectName == "HomePrimary" ? QStringLiteral("#38d8c8") : QStringLiteral("#a3b5af")), 0, Qt::AlignVCenter);

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
    layout->addWidget(iconLabel("chevron-right", 30), 0, Qt::AlignVCenter);

    QObject::connect(button, &QPushButton::clicked, button, [action = std::move(action)] {
        if (action) {
            action();
        }
    });
    return button;
}

QWidget* RoomDirectoryWidget::buildRoomPanel()
{
    auto* panel = new QFrame;
    panel->setObjectName("HomePanel");

    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* heading = new QHBoxLayout;
    heading->setContentsMargins(0, 0, 0, 0);
    heading->addWidget(label("Available rooms", "HomeSectionTitle"), 1);
    search_ = new QLineEdit; search_->setObjectName("roomSearch"); search_->setPlaceholderText("Search rooms");
    search_->setAccessibleName("Search available rooms"); search_->setClearButtonEnabled(true);
    search_->setMinimumWidth(220); search_->setMinimumHeight(28);
    search_->addAction(QIcon(":/screenshare/ui/icons/search.svg"), QLineEdit::LeadingPosition);
    heading->addWidget(search_);
    connect(search_, &QLineEdit::textChanged, this, [this] { filterRooms(); });
    refreshRoomsButton_ = actionButton("", "HomeGhost", "refresh");
    refreshRoomsButton_->setFixedWidth(44);
    refreshRoomsButton_->setToolTip("Refresh rooms");
    refreshRoomsButton_->setAccessibleName("Refresh rooms");
    QObject::connect(refreshRoomsButton_, &QPushButton::clicked, this, [this] {
        refreshRooms();
    });
    heading->addWidget(refreshRoomsButton_);
    layout->addLayout(heading);
    directoryStatus_ = label("Connecting…", "HomeInfoSecondary");
    layout->addWidget(directoryStatus_);

    auto* columns = new QWidget;
    columns->setObjectName("HomeRoomHeader");
    auto* columnLayout = new QHBoxLayout(columns);
    columnLayout->setContentsMargins(54, 6, 10, 6);
    columnLayout->addWidget(label("Room", "HomeInfoTitle"), 1);
    columnLayout->setSpacing(12);
    auto* viewers = label("Viewers", "HomeInfoTitle"); viewers->setFixedWidth(70);
    viewers->setAlignment(Qt::AlignLeft | Qt::AlignVCenter); columnLayout->addWidget(viewers);
    auto* access = label("Access", "HomeInfoTitle"); access->setFixedWidth(130);
    columnLayout->addWidget(access);
    auto* action = label("Join", "HomeInfoTitle"); action->setFixedWidth(84);
    columnLayout->addWidget(action);
    auto* listFrame = new QFrame; listFrame->setObjectName("HomeRoomList");
    auto* listLayout = new QVBoxLayout(listFrame); listLayout->setContentsMargins(0, 0, 0, 0); listLayout->setSpacing(0);
    listLayout->addWidget(columns);
    auto* roomList = new QWidget;
    roomListLayout_ = new QVBoxLayout(roomList);
    roomListLayout_->setContentsMargins(0, 0, 0, 0);
    roomListLayout_->setSpacing(0);
    roomStatusLabel_ = label("Loading rooms...", "HomeEmptyState");
    roomStatusLabel_->setAlignment(Qt::AlignCenter);
    roomListLayout_->addWidget(roomStatusLabel_, 1);
    auto* scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(roomList); listLayout->addWidget(scroll, 1); layout->addWidget(listFrame, 1);

    return panel;
}

QWidget* RoomDirectoryWidget::buildRoomRow(const HomeActiveRoom& room)
{
    auto* row = new QFrame;
    row->setObjectName("HomeRoomRow");
    row->setProperty("roomId", room.roomId);
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
    layout->addLayout(text, 1);
    auto* viewers = label(QString::number(room.peerCount), "HomeViewerCount");
    viewers->setFixedWidth(70); viewers->setAlignment(Qt::AlignLeft | Qt::AlignVCenter); layout->addWidget(viewers);

    auto* status = label(
        room.passwordProtected ? QStringLiteral("Locked") : QStringLiteral("Public"),
        room.passwordProtected ? "HomeLockedStatus" : "HomePublicStatus");
    status->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    status->setFixedHeight(22);
    status->setFixedWidth(130);
    layout->addWidget(status, 0, Qt::AlignVCenter);

    auto* join = actionButton("Join", "HomeTinyButton", "watch");
    join->setFixedWidth(84);
    join->setEnabled(room.joinable);
    QObject::connect(join, &QPushButton::clicked, this, [this, id = room.roomId] {
        if (join_) join_(id);
    });
    layout->addWidget(join, 0, Qt::AlignVCenter);
    return row;
}

void RoomDirectoryWidget::refreshRooms()
{
    if (request_) request_();
}

void RoomDirectoryWidget::setPushedRooms(const QVector<HomeActiveRoom>& rooms, const QString& unavailable)
{
    if (unavailable.isEmpty() || !rooms.empty()) updateRooms(rooms);
    directoryStatus_->setText(unavailable);
    directoryStatus_->setVisible(!unavailable.isEmpty());
    if (!unavailable.isEmpty()) for (auto* button : findChildren<QPushButton*>("HomeTinyButton")) button->setEnabled(false);
    refreshRoomsButton_->setEnabled(true);
}

void RoomDirectoryWidget::updateRooms(const QVector<HomeActiveRoom>& rooms)
{
    if (roomListLayout_ == nullptr) {
        return;
    }

    QSet<QString> ids;
    for (const auto& room : rooms) ids.insert(room.roomId);
    // Keep existing row widgets and their order so pushes preserve focus/scroll.
    for (int i = roomListLayout_->count()-1; i >= 0; --i) {
        auto* item = roomListLayout_->itemAt(i); auto* widget = item->widget();
        if (!widget || !ids.contains(widget->property("roomId").toString())) {
            delete roomListLayout_->takeAt(i); delete widget;
        }
    }
    roomStatusLabel_ = nullptr;
    for (const HomeActiveRoom& room : rooms) {
        QWidget* row = nullptr;
        for (int i = 0; i < roomListLayout_->count(); ++i) {
            auto* candidate = roomListLayout_->itemAt(i)->widget();
            if (candidate && candidate->property("roomId").toString() == room.roomId) { row = candidate; break; }
        }
        if (!row) { row = buildRoomRow(room); roomListLayout_->addWidget(row); }
        row->setProperty("searchName", room.name);
        row->findChild<QLabel*>("HomeInfoPrimary")->setText(room.name);
        row->findChild<QLabel*>("HomeViewerCount")->setText(QString::number(room.peerCount));
        row->findChild<QPushButton*>("HomeTinyButton")->setEnabled(room.joinable);
        auto* badge = row->findChild<QLabel*>("HomeLockedStatus");
        if (!badge) badge = row->findChild<QLabel*>("HomePublicStatus");
        badge->setText(room.passwordProtected ? "Password required" : "Public");
    }

    if (rooms.isEmpty()) {
        roomStatusLabel_ = label("No open rooms", "HomeEmptyState");
        roomStatusLabel_->setAlignment(Qt::AlignCenter);
        roomListLayout_->addWidget(roomStatusLabel_, 1);
    } else {
        roomStatusLabel_ = nullptr;
        roomListLayout_->addStretch(1);
    }

    filterRooms();
}

void RoomDirectoryWidget::filterRooms()
{
    for (auto* row : findChildren<QFrame*>("HomeRoomRow"))
        row->setVisible(row->property("searchName").toString().contains(search_->text().trimmed(), Qt::CaseInsensitive));
}

void RoomDirectoryWidget::showRoomStatus(const QString& message)
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
}
