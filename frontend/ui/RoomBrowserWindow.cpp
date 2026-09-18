#include "ui/RoomBrowserWindow.h"
#include "ui/RoomApplication.h"
#include "shared/RoomLink.h"
#include "ui/UiStyle.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/win32_socket_init.h"
#include "rtc_base/logging.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>
using namespace screenshare;
using namespace screenshare::room::qt;

RoomBrowserWindow::RoomBrowserWindow(QUrl origin, QtRoomSession::Factory factory, bool loopback, QString profileFile, bool enumerateSources)
    : origin_(std::move(origin)), factory_(std::move(factory)), loopback_(loopback), enumerateSources_(enumerateSources), profile_(profileFile), directory_(loopback) {
    setWindowTitle("ScreenShare — Rooms"); setStyleSheet(uiStyleSheet()); resize(900, 720);
    auto* layout = new QVBoxLayout(this);
    auto* service = new QLabel(origin_.toString()); service->setTextFormat(Qt::PlainText); layout->addWidget(service);
    auto* form = new QFormLayout;
    nickname_ = new QLineEdit(profile_.nickname()); nickname_->setObjectName("roomNickname"); nickname_->setMaxLength(128);
    nickname_->setToolTip("Saved for new sessions. A nickname is a display name, not an authenticated identity."); form->addRow("Nickname", nickname_);
    auto* save = new QPushButton("Save nickname"); save->setObjectName("saveNickname"); form->addRow(save);
    name_ = new QLineEdit("My room"); name_->setObjectName("roomName"); name_->setMaxLength(256); form->addRow("Room name", name_);
    public_ = new QCheckBox("Show in public room list"); public_->setObjectName("publicRoom"); public_->setChecked(true); form->addRow(public_);
    source_ = new QComboBox; source_->setObjectName("captureSource");
    form->addRow("Capture", source_);
    auto* refreshSources = new QPushButton("Refresh capture sources"); refreshSources->setObjectName("refreshCaptureSources"); form->addRow(refreshSources);
    connect(refreshSources, &QPushButton::clicked, this, [this] { RefreshSources(); });
    audio_ = new QComboBox; audio_->addItems({"System audio", "Microphone", "No shared audio"}); form->addRow("Audio", audio_);
    auto* create = new QPushButton("Create room"); create->setObjectName("createV2Room"); form->addRow(create);
    roomId_ = new QLineEdit; roomId_->setObjectName("joinRoomId"); roomId_->setMaxLength(512); form->addRow("Room ID or v2 link", roomId_);
    roomId_->setToolTip("Links use the service shown above. Enter the room password separately.");
    password_ = new QLineEdit; password_->setObjectName("roomPassword"); password_->setEchoMode(QLineEdit::Password); password_->setMaxLength(256);
    form->addRow("Room password (create or join)", password_);
    decoder_ = new QComboBox; decoder_->setObjectName("roomDecoder");
    decoder_->addItem("Automatic (prefer hardware)", "auto");
    decoder_->addItem("Software (compatibility)", "software");
    decoder_->setCurrentIndex(decoder_->findData(profile_.decoder()));
    decoder_->setToolTip("Used when joining. Software decoding avoids GPU decoder issues but uses more CPU. Saved for future joins.");
    form->addRow("Video decoding (when joining)", decoder_);
    auto* join = new QPushButton("Join room"); join->setObjectName("joinV2Room"); form->addRow(join); layout->addLayout(form);
    status_ = new QLabel; layout->addWidget(status_);
    error_ = new QLabel; error_->setObjectName("browserError"); error_->setTextFormat(Qt::PlainText); error_->setWordWrap(true); layout->addWidget(error_);
    rooms_ = new QTableWidget(0, 4); rooms_->setObjectName("publicRooms"); rooms_->setHorizontalHeaderLabels({"Room", "Viewers", "Status", "Password"});
    rooms_->setEditTriggers(QAbstractItemView::NoEditTriggers); rooms_->setSelectionBehavior(QAbstractItemView::SelectRows);
    rooms_->setSelectionMode(QAbstractItemView::SingleSelection); rooms_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(rooms_, 1);
    joinSelected_ = new QPushButton("Join selected room"); joinSelected_->setObjectName("joinSelectedRoom"); joinSelected_->setEnabled(false); layout->addWidget(joinSelected_);
    retry_ = new QPushButton("Retry room list connection"); retry_->setEnabled(false); layout->addWidget(retry_);
    connect(save, &QPushButton::clicked, this, [this] {
        if (!profile_.saveNickname(nickname_->text())) error_->setText("Nickname is invalid or could not be saved.");
        else { nickname_->setText(profile_.nickname()); error_->clear(); }
    });
    connect(create, &QPushButton::clicked, this, [this] { Launch(true); });
    connect(join, &QPushButton::clicked, this, [this] { Launch(false); });
    connect(retry_, &QPushButton::clicked, this, [this] { directory_.Start(origin_); });
    connect(rooms_, &QTableWidget::itemSelectionChanged, this, [this] {
        const auto row = rooms_->currentRow();
        const auto state = directory_.status();
        joinSelected_->setEnabled(state.phase == RoomDirectory::Phase::Ready && row >= 0 && row < rooms_->rowCount() &&
                                  rooms_->item(row, 2)->text() == "open");
    });
    connect(joinSelected_, &QPushButton::clicked, this, [this] {
        const auto row = rooms_->currentRow();
        if (directory_.status().phase != RoomDirectory::Phase::Ready || row < 0 || !joinSelected_->isEnabled()) return;
        roomId_->setText(rooms_->item(row, 0)->data(Qt::UserRole).toString()); Launch(false);
    });
    directory_.changed = [this](const auto& value) {
        Refresh(value);
        if (directoryChanged) directoryChanged(value);
        if (closing_ && !closedNotified_ && !active_ && !directory_.running()) QTimer::singleShot(0, this, [this] { close(); });
    };
    RefreshSources();
}
RoomBrowserWindow::~RoomBrowserWindow() = default;
void RoomBrowserWindow::RefreshSources() {
    const auto previous = source_->currentData();
    source_->clear();
    if (!enumerateSources_) source_->addItem("Default display", QVariantMap{{"display", 0}});
    else try {
        for (const auto& display : DesktopCapturer::EnumerateDisplays()) if (display.attachedToDesktop)
            source_->addItem(QString("Display %1 — %2").arg(display.index).arg(QString::fromStdWString(display.outputName)), QVariantMap{{"display", display.index}});
        for (const auto& window : DesktopCapturer::EnumerateWindows())
            source_->addItem(QString::fromStdWString(window.title), QVariantMap{{"window", QVariant::fromValue<qulonglong>(window.handle)}});
    } catch (...) { source_->clear(); }
    if (previous.isValid()) source_->setCurrentIndex(source_->findData(previous));
    if (!source_->count()) error_->setText("No capture sources available. Refresh to try again.");
}
void RoomBrowserWindow::ShowBackButton() {
    auto* button = new QPushButton("Back", this); button->setObjectName("roomBack");
    static_cast<QVBoxLayout*>(layout())->insertWidget(0, button);
    connect(button, &QPushButton::clicked, this, [this] { password_->clear(); if (back) back(); });
}
void RoomBrowserWindow::OpenCreate() { setWindowTitle("ScreenShare — Create room"); RefreshSources(); password_->clear(); name_->setFocus(); }
void RoomBrowserWindow::OpenJoin(const QString& roomId) {
    setWindowTitle("ScreenShare — Join room"); password_->clear(); roomId_->setText(roomId); roomId_->setFocus();
}
void RoomBrowserWindow::Refresh(const RoomDirectory::Status& state) {
    retry_->setEnabled(state.phase == RoomDirectory::Phase::Failed);
    QString selected;
    if (rooms_->currentRow() >= 0 && rooms_->item(rooms_->currentRow(), 0)) selected = rooms_->item(rooms_->currentRow(), 0)->data(Qt::UserRole).toString();
    const QSignalBlocker blocked(rooms_); rooms_->clearSelection(); rooms_->setCurrentCell(-1, -1);
    rooms_->setRowCount(int(state.rooms.size())); joinSelected_->setEnabled(false);
    for (size_t i = 0; i < state.rooms.size(); ++i) {
        const auto& room = state.rooms[i]; auto* title = new QTableWidgetItem(room.name); title->setData(Qt::UserRole, room.id);
        rooms_->setItem(int(i), 0, title); rooms_->setItem(int(i), 1, new QTableWidgetItem(QString("%1/%2").arg(room.viewers).arg(room.limit)));
        rooms_->setItem(int(i), 2, new QTableWidgetItem(room.status)); rooms_->setItem(int(i), 3, new QTableWidgetItem(room.password ? "Required" : "No"));
        if (room.id == selected) { rooms_->selectRow(int(i)); joinSelected_->setEnabled(state.phase == RoomDirectory::Phase::Ready && room.status == "open"); }
    }
    switch (state.phase) {
    case RoomDirectory::Phase::Stopped: status_->setText("Room list paused."); break;
    case RoomDirectory::Phase::Connecting: status_->setText("Connecting to room list…"); break;
    case RoomDirectory::Phase::Ready: status_->setText(QString("%1 public rooms — updates arrive automatically.").arg(state.rooms.size())); break;
    case RoomDirectory::Phase::Reconnecting: status_->setText("Reconnecting. The room list may be outdated."); break;
    case RoomDirectory::Phase::Failed: status_->setText("Room list unavailable. Reconnect to try again."); break;
    }
}
void RoomBrowserWindow::Launch(bool host) {
    if (active_ || closing_) return;
    if (host && source_->currentIndex() < 0) { error_->setText("Choose an available capture source before sharing."); return; }
    const auto nickname = RoomProfile::normalizeNickname(nickname_->text());
    const auto roomId = ParseRoomReference(roomId_->text().trimmed());
    if (!nickname || (!host && !roomId)) { error_->setText("Enter a valid nickname and room ID or v2 link when joining."); return; }
    QJsonObject input{{"origin", origin_.toString()}, {"host", host}, {"nickname", *nickname}, {"name", name_->text()},
        {"roomId", host ? QString{} : *roomId}, {"password", password_->text()}, {"public", public_->isChecked()},
        {"capture", QJsonObject::fromVariantMap(source_->currentData().toMap())},
        {"audio", QJsonObject{{"source", audio_->currentIndex() == 0 ? "system" : audio_->currentIndex() == 1 ? "microphone" : "none"}}}};
    // Window handles are strings at the shared configuration boundary.
    auto capture = input["capture"].toObject();
    if (capture.contains("window")) capture["window"] = QString::number(source_->currentData().toMap()["window"].toULongLong());
    input["capture"] = capture;
    if (!host) input["decoder"] = decoder_->currentData().toString();
    try {
        auto config = ParseRoomSessionConfig(input, loopback_);
        config.media.preferences = profile_.streamPreferences();
        const auto playback = profile_.playback();
        config.media.playbackVolume = playback.volume; config.media.playbackMuted = playback.muted;
        if (!profile_.saveNickname(*nickname)) { error_->setText("Could not save the nickname."); return; }
        if (!host && !profile_.saveDecoder(decoder_->currentData().toString())) { error_->setText("Could not save the decoding preference."); return; }
        nickname_->setText(*nickname); error_->clear();
        active_ = std::make_unique<RoomSessionWindow>(std::move(config), factory_, loopback_, &profile_);
        active_->closed = [this] { QTimer::singleShot(0, this, [this] {
            active_.reset();
            if (closing_) close();
            else if (returnFromSession) returnFromSession();
            else if (presentPage) presentPage(this);
            else show();
        }); };
        password_->clear();
        if (presentPage) presentPage(active_.get());
        else { active_->show(); hide(); }
    } catch (...) { error_->setText("Invalid room or capture settings."); }
}
void RoomBrowserWindow::showEvent(QShowEvent* event) { QWidget::showEvent(event); if (!closing_) directory_.Start(origin_); }
void RoomBrowserWindow::hideEvent(QHideEvent* event) { if (!keepDirectoryOnHide) directory_.Stop(); QWidget::hideEvent(event); }
void RoomBrowserWindow::closeEvent(QCloseEvent* event) {
    closing_ = true;
    if (active_ || directory_.running()) {
        if (active_) active_->close(); directory_.Stop(); event->ignore(); return;
    }
    event->accept(); if (!closedNotified_) { closedNotified_ = true; if (closed) closed(); }
}
int RunRoomBrowserWindow(const QUrl& origin, bool normalHome, std::function<void(AppShellWindow&)> initializeShell) {
    try { ParseRoomSessionConfig(QJsonObject{{"origin", origin.toString()}, {"host", true}}); }
    catch (...) { QMessageBox::critical(nullptr, "Invalid service", "Use an HTTPS service origin without credentials or a path."); return 1; }
    webrtc::WinsockInitializer winsock;
    if (winsock.error() || !webrtc::InitializeSSL()) return 1;
    struct Ssl { ~Ssl() { webrtc::CleanupSSL(); } } ssl;
    webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
    webrtc::InitializeLogging(std::move(logging));
    const bool previous = QApplication::quitOnLastWindowClosed(); QApplication::setQuitOnLastWindowClosed(false);
    RoomApplication window(origin, screenshare::media::WindowsRoomRuntimeFactory, false, {}, true, normalHome);
    if (initializeShell) initializeShell(window.window());
    window.closed = [] { QApplication::quit(); }; window.show();
    const int result = QApplication::exec(); QApplication::setQuitOnLastWindowClosed(previous); return result;
}
