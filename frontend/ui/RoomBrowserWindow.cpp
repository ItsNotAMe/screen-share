#include "ui/RoomBrowserWindow.h"
#include "ui/RoomApplication.h"
#include "shared/RoomLink.h"
#include "ui/UiStyle.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/win32_socket_init.h"
#include "rtc_base/logging.h"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QResizeEvent>
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
    setObjectName("RoomBrowser");
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(28, 20, 28, 24); layout->setSpacing(16);
    heading_ = new QLabel("Create a room"); heading_->setObjectName("PageHeading"); layout->addWidget(heading_);
    auto* scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget; auto* body = new QVBoxLayout(content); body->setContentsMargins(0,0,8,0); body->setSpacing(18);
    scroll->setWidget(content); layout->addWidget(scroll, 1);
    createPanel_ = new QWidget; createColumns_ = new QBoxLayout(QBoxLayout::TopToBottom, createPanel_);
    createColumns_->setContentsMargins(0,0,0,0); createColumns_->setSpacing(24);
    auto* details = new QWidget; details->setObjectName("FormCard"); auto* form = new QFormLayout(details); createColumns_->addWidget(details, 1);
    form->setContentsMargins(16,16,16,16); form->setSpacing(12); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto* detailsHeading = new QLabel("Room details"); detailsHeading->setObjectName("SectionHeading"); form->addRow(detailsHeading);
    name_ = new QLineEdit("My room"); name_->setObjectName("roomName"); name_->setMaxLength(256); form->addRow("Room name", name_);
    public_ = new QCheckBox("List this room publicly"); public_->setObjectName("publicRoom"); public_->setChecked(true); form->addRow("Visibility", public_);
    viewerLimit_ = new QSpinBox; viewerLimit_->setObjectName("createViewerLimit"); viewerLimit_->setRange(1,63); viewerLimit_->setValue(4);
    form->addRow("Viewer limit", viewerLimit_);
    auto* limitHint = new QLabel("More than four viewers increases upload and encoding work."); limitHint->setWordWrap(true);
    limitHint->setObjectName("FormHint"); form->addRow(limitHint);
    auto* stream = new QWidget; stream->setObjectName("FormCard"); form = new QFormLayout(stream); createColumns_->addWidget(stream, 1);
    form->setContentsMargins(16,16,16,16); form->setSpacing(12); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto* streamHeading = new QLabel("Stream"); streamHeading->setObjectName("SectionHeading"); form->addRow(streamHeading);
    source_ = new QComboBox; source_->setObjectName("captureSource"); source_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    source_->setMinimumContentsLength(20); form->addRow("Share source", source_);
    auto* refreshSources = new QPushButton("Refresh sources"); refreshSources->setObjectName("refreshCaptureSources"); form->addRow("", refreshSources);
    connect(refreshSources, &QPushButton::clicked, this, [this] { RefreshSources(); });
    const auto preferences = profile_.streamPreferences();
    preset_ = new QComboBox; preset_->setObjectName("createPreset"); preset_->addItems({"Gaming — responsiveness", "Quality — detail"});
    preset_->setCurrentIndex(int(preferences.preset)); form->addRow("Preset", preset_);
    resolution_ = new QComboBox; resolution_->setObjectName("createResolution");
    resolution_->addItem("Auto", "auto"); resolution_->addItem("Native source size", "native");
    for (const auto& size : {QSize(1280,720), QSize(1920,1080), QSize(2560,1440), QSize(3840,2160)})
        resolution_->addItem(QString("%1 × %2").arg(size.width()).arg(size.height()), size);
    if (preferences.resolution == media::ResolutionMode::Native) resolution_->setCurrentIndex(1);
    if (preferences.resolution == media::ResolutionMode::Fixed) {
        const QSize size(preferences.width, preferences.height);
        int index = resolution_->findData(size);
        if (index < 0) { resolution_->addItem(QString("%1 × %2").arg(size.width()).arg(size.height()), size); index = resolution_->count()-1; }
        resolution_->setCurrentIndex(index);
    }
    form->addRow("Resolution", resolution_);
    fps_ = new QComboBox; fps_->setObjectName("createFps"); fps_->addItem("Auto", 0);
    for(int value : {30,60,90,120,144,240}) fps_->addItem(QString("%1 FPS").arg(value), value);
    int frameIndex = fps_->findData(preferences.fpsMode == media::SettingMode::Auto ? 0 : preferences.fps);
    if(frameIndex < 0) { fps_->addItem(QString("%1 FPS").arg(preferences.fps), preferences.fps); frameIndex = fps_->count()-1; }
    fps_->setCurrentIndex(frameIndex); form->addRow("Frame rate", fps_);
    bitrate_ = new QComboBox; bitrate_->setObjectName("createBitrate"); bitrate_->addItem("Auto", 0);
    for(int value : {2,4,8,12,20,40,80}) bitrate_->addItem(QString("%1 Mbps limit").arg(value), value*1000000);
    const int rate = preferences.bitrateLimitBps.value_or(0);
    int rateIndex = bitrate_->findData(rate);
    if(rateIndex < 0) { bitrate_->addItem(QString("%1 Mbps limit").arg(rate/1000000.0), rate); rateIndex = bitrate_->count()-1; }
    bitrate_->setCurrentIndex(rateIndex); form->addRow("Bitrate", bitrate_);
    auto* policy = new QLabel("Auto adapts to the connection. Manual resolution and FPS set the requested output; bitrate is a limit, not a guaranteed rate.");
    policy->setWordWrap(true); policy->setObjectName("FormHint"); form->addRow(policy);
    audio_ = new QComboBox; audio_->addItems({"System audio", "Microphone", "No shared audio"}); form->addRow("Shared audio", audio_);
    body->addWidget(createPanel_);
    joinPanel_ = new QWidget; auto* joinForm = new QFormLayout(joinPanel_); joinForm->setContentsMargins(0,0,0,0); joinForm->setSpacing(12);
    joinForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); joinForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    roomId_ = new QLineEdit; roomId_->setObjectName("joinRoomId"); roomId_->setMaxLength(512);
    roomId_->setPlaceholderText("Paste a room link or room ID"); joinForm->addRow("Room link", roomId_);
    decoder_ = new QComboBox; decoder_->setObjectName("roomDecoder");
    decoder_->addItem("Automatic (prefer hardware)", "auto"); decoder_->addItem("Software (compatibility)", "software");
    decoder_->setCurrentIndex(decoder_->findData(profile_.decoder()));
    joinForm->addRow("Video decoding", decoder_); body->addWidget(joinPanel_);
    password_ = new QLineEdit; password_->setObjectName("roomPassword"); password_->setEchoMode(QLineEdit::Password); password_->setMaxLength(256);
    password_->setPlaceholderText("Leave empty if no password is needed");
    auto* passwordForm = new QFormLayout; passwordForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    passwordForm->addRow("Room password", password_); body->addLayout(passwordForm);
    status_ = new QLabel; status_->setWordWrap(true); status_->setObjectName("FormHint"); body->addWidget(status_);
    rooms_ = new QTableWidget(0, 4); rooms_->setObjectName("publicRooms"); rooms_->setHorizontalHeaderLabels({"Room", "Viewers", "Status", "Password"});
    rooms_->setEditTriggers(QAbstractItemView::NoEditTriggers); rooms_->setSelectionBehavior(QAbstractItemView::SelectRows);
    rooms_->setSelectionMode(QAbstractItemView::SingleSelection); rooms_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    rooms_->verticalHeader()->hide(); rooms_->setMinimumHeight(180); body->addWidget(rooms_, 1);
    joinSelected_ = new QPushButton("Join selected room"); joinSelected_->setObjectName("joinSelectedRoom"); joinSelected_->setEnabled(false);
    retry_ = new QPushButton("Reconnect to room list"); retry_->setEnabled(false); body->addWidget(retry_);
    body->addStretch();
    error_ = new QLabel; error_->setObjectName("browserError"); error_->setTextFormat(Qt::PlainText); error_->setWordWrap(true); layout->addWidget(error_);
    auto* actions = new QHBoxLayout; actions->addWidget(joinSelected_); actions->addStretch();
    auto* create = new QPushButton("Create && start sharing"); create->setObjectName("createV2Room"); actions->addWidget(create);
    auto* join = new QPushButton("Join room"); join->setObjectName("joinV2Room"); actions->addWidget(join); layout->addLayout(actions);
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
    OpenCreate();
}
RoomBrowserWindow::~RoomBrowserWindow() = default;
void RoomBrowserWindow::resizeEvent(QResizeEvent* event) {
    createColumns_->setDirection(event->size().width() >= 950 ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    QWidget::resizeEvent(event);
}
void RoomBrowserWindow::OpenPreferences(bool playback, QWidget* owner) {
    if (closing_) return;
    if (!owner) owner = window();
    if (auto* existing = owner->findChild<QDialog*>("ProfilePreferences")) { existing->raise(); existing->activateWindow(); return; }
    auto* dialog = new QDialog(owner); dialog->setObjectName("ProfilePreferences");
    dialog->setWindowTitle("ScreenShare settings"); dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::WindowModal); dialog->resize(480, 340);
    auto* layout = new QVBoxLayout(dialog); layout->setContentsMargins(24,24,24,24); layout->setSpacing(16);
    auto* tabs = new QTabWidget; layout->addWidget(tabs);
    auto* profilePage = new QWidget; auto* profileForm = new QFormLayout(profilePage);
    auto* nickname = new QLineEdit(profile_.nickname()); nickname->setObjectName("profileNickname"); nickname->setMaxLength(32);
    profileForm->addRow("Nickname", nickname);
    auto* hint = new QLabel("Your display name for new rooms. This is not an account or verified identity. Existing sessions keep their current name.");
    hint->setWordWrap(true); profileForm->addRow(hint); tabs->addTab(profilePage, "Profile");
    auto* playbackPage = new QWidget; auto* playbackForm = new QFormLayout(playbackPage);
    auto* decoder = new QComboBox; decoder->addItem("Automatic", "auto"); decoder->addItem("Software compatibility", "software");
    decoder->setCurrentIndex(decoder->findData(profile_.decoder())); decoder->setObjectName("profileDecoder");
    playbackForm->addRow("Video decoding", decoder);
    auto* volume = new QSpinBox; volume->setRange(0,100); volume->setSuffix(" %"); volume->setValue(profile_.playback().volume); volume->setObjectName("profileVolume");
    playbackForm->addRow("Initial volume", volume);
    auto* muted = new QCheckBox("Start muted"); muted->setChecked(profile_.playback().muted); playbackForm->addRow(muted);
    auto* playbackHint = new QLabel("Defaults apply when joining your next room. Current playback is controlled from the viewer.");
    playbackHint->setWordWrap(true); playbackForm->addRow(playbackHint); tabs->addTab(playbackPage, "Playback");
    tabs->setCurrentIndex(playback ? 1 : 0);
    auto* error = new QLabel; error->setTextFormat(Qt::PlainText); error->setWordWrap(true); error->setObjectName("profileError"); layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel); layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Save)->setObjectName("saveProfile");
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, nickname, decoder, volume, muted, error] {
        if (!RoomProfile::normalizeNickname(nickname->text())) { error->setText("Use 1–32 characters without control characters."); nickname->setFocus(); return; }
        if (!profile_.saveNickname(nickname->text()) || !profile_.saveDecoder(decoder->currentData().toString()) ||
            !profile_.savePlayback({volume->value(), muted->isChecked()})) { error->setText("Could not save all preferences. Check your settings folder is writable."); return; }
        decoder_->setCurrentIndex(decoder_->findData(profile_.decoder()));
        if (profileChanged) profileChanged(); dialog->accept();
    });
    dialog->open(); if (!playback) nickname->setFocus();
}
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
    auto* button = new QPushButton("‹ Home", this); button->setObjectName("roomBack");
    static_cast<QVBoxLayout*>(layout())->insertWidget(0, button, 0, Qt::AlignLeft);
    connect(button, &QPushButton::clicked, this, [this] { password_->clear(); if (back) back(); });
}
void RoomBrowserWindow::OpenCreate() {
    setWindowTitle("ScreenShare — Create room"); heading_->setText("Create a room");
    createPanel_->show(); joinPanel_->hide(); rooms_->hide(); joinSelected_->hide(); retry_->hide(); status_->hide();
    findChild<QPushButton*>("createV2Room")->show(); findChild<QPushButton*>("joinV2Room")->hide();
    RefreshSources(); password_->clear(); error_->clear(); name_->setFocus();
}
void RoomBrowserWindow::OpenJoin(const QString& roomId) {
    setWindowTitle("ScreenShare — Join room"); heading_->setText("Join a room");
    createPanel_->hide(); joinPanel_->show(); rooms_->show(); joinSelected_->show(); retry_->setVisible(directory_.status().phase == RoomDirectory::Phase::Failed); status_->show();
    findChild<QPushButton*>("createV2Room")->hide(); findChild<QPushButton*>("joinV2Room")->show();
    password_->clear(); error_->clear(); roomId_->setText(roomId); roomId_->setFocus();
}
void RoomBrowserWindow::Refresh(const RoomDirectory::Status& state) {
    retry_->setEnabled(state.phase == RoomDirectory::Phase::Failed);
    retry_->setVisible(!joinPanel_->isHidden() && state.phase == RoomDirectory::Phase::Failed);
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
    const auto nickname = RoomProfile::normalizeNickname(profile_.nickname());
    const auto roomId = ParseRoomReference(roomId_->text().trimmed());
    if (!nickname || (!host && !roomId)) { error_->setText("Enter a valid room ID or room link."); return; }
    QJsonObject input{{"origin", origin_.toString()}, {"host", host}, {"nickname", *nickname}, {"name", name_->text()},
        {"roomId", host ? QString{} : *roomId}, {"password", password_->text()}, {"public", public_->isChecked()}, {"viewerLimit", viewerLimit_->value()},
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
        if (host) {
            auto& p = config.media.preferences;
            p.preset = media::StreamPreset(preset_->currentIndex());
            p.resolution = resolution_->currentIndex() == 0 ? media::ResolutionMode::Auto :
                resolution_->currentIndex() == 1 ? media::ResolutionMode::Native : media::ResolutionMode::Fixed;
            if (p.resolution == media::ResolutionMode::Fixed) { const auto size = resolution_->currentData().toSize(); p.width = size.width(); p.height = size.height(); }
            p.fpsMode = fps_->currentData().toInt() ? media::SettingMode::Manual : media::SettingMode::Auto;
            if (p.fpsMode == media::SettingMode::Manual) p.fps = fps_->currentData().toInt();
            const int rate = bitrate_->currentData().toInt();
            p.bitrateMode = rate ? media::SettingMode::Manual : media::SettingMode::Auto;
            p.bitrateLimitBps = rate ? std::optional<int>(rate) : std::nullopt;
            media::ValidateStreamPreferences(p);
        }
        const auto playback = profile_.playback();
        config.media.playbackVolume = playback.volume; config.media.playbackMuted = playback.muted;
        if (!host && !profile_.saveDecoder(decoder_->currentData().toString())) { error_->setText("Could not save the decoding preference."); return; }
        error_->clear();
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
