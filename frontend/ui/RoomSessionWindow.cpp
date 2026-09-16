#include "ui/RoomSessionWindow.h"
#include "shared/RoomLink.h"
#include <QClipboard>
#include "ui/VideoFrameWidget.h"
#include "ui/UiStyle.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/win32_socket_init.h"
#include "rtc_base/logging.h"
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCheckBox>
#include <QFile>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
using namespace screenshare::v2;
using namespace screenshare::media;
namespace {
QString Phase(RoomPhase phase) {
    switch (phase) {
    case RoomPhase::Idle: return "Ready"; case RoomPhase::Admitting: return "Entering room…";
    case RoomPhase::Connecting: return "Connecting…"; case RoomPhase::Active: return "Connected";
    case RoomPhase::Reconnecting: return "Reconnecting…"; case RoomPhase::Stopping: return "Stopping…";
    case RoomPhase::Stopped: return "Stopped"; case RoomPhase::Failed: return "Session failed";
    }
    return "Session failed";
}
}
RoomSessionWindow::RoomSessionWindow(RoomSessionConfig config, QtRoomSession::Factory factory, bool loopback)
    : session_(nullptr, std::move(factory), loopback) {
    setWindowTitle(config.room.host ? "ScreenShare — Share room" : "ScreenShare — Watch room");
    setStyleSheet(uiStyleSheet()); resize(960, 720);
    auto* layout = new QVBoxLayout(this);
    phase_ = new QLabel("Starting…"); phase_->setObjectName("roomPhase"); layout->addWidget(phase_);
    room_ = new QLabel; room_->setTextFormat(Qt::PlainText); room_->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(room_);
    roomLink_ = new QLineEdit; roomLink_->setReadOnly(true); roomLink_->setObjectName("roomLink"); layout->addWidget(roomLink_);
    roomLink_->setToolTip("Share with someone using the same service. Passwords must be shared separately.");
    copyLink_ = new QPushButton("Copy room link"); copyLink_->setObjectName("copyRoomLink"); copyLink_->setEnabled(false); layout->addWidget(copyLink_);
    connect(copyLink_, &QPushButton::clicked, this, [this] { if (!roomLink_->text().isEmpty()) QApplication::clipboard()->setText(roomLink_->text()); });
    error_ = new QLabel; error_->setObjectName("roomError"); error_->setTextFormat(Qt::PlainText); error_->setWordWrap(true); layout->addWidget(error_);
    auto* roomForm = new QFormLayout;
    nickname_ = new QLineEdit; nickname_->setObjectName("liveNickname"); nickname_->setMaxLength(128);
    name_ = new QLineEdit; name_->setObjectName("liveRoomName"); name_->setMaxLength(256);
    publicRoom_ = new QCheckBox("Public room"); publicRoom_->setObjectName("livePublicRoom");
    viewerLimit_ = new QSpinBox; viewerLimit_->setRange(1, 63); viewerLimit_->setObjectName("liveViewerLimit");
    updateNickname_ = new QPushButton("Update nickname for this session"); updateNickname_->setObjectName("updateNickname");
    updatePolicy_ = new QPushButton("Update room"); updatePolicy_->setObjectName("updateRoomPolicy");
    roomForm->addRow("Nickname", nickname_); roomForm->addRow(updateNickname_);
    if (config.room.host) {
        roomForm->addRow("Room name", name_); roomForm->addRow(publicRoom_);
        roomForm->addRow("Viewer limit", viewerLimit_); roomForm->addRow(updatePolicy_);
    } else { name_->hide(); publicRoom_->hide(); viewerLimit_->hide(); updatePolicy_->hide(); }
    // Parent even hidden host controls, so their lifetime follows this window.
    QWidget* roomFields[] = {name_, publicRoom_, viewerLimit_, updatePolicy_};
    for (auto* field : roomFields) if (!field->parent()) field->setParent(this);
    updateNickname_->setEnabled(false); updatePolicy_->setEnabled(false);
    auto* reloadRoom = new QPushButton("Reload current room values"); reloadRoom->setObjectName("reloadRoomValues"); roomForm->addRow(reloadRoom);
    layout->addLayout(roomForm);
    members_ = new QLabel; members_->setTextFormat(Qt::PlainText); members_->setWordWrap(true); members_->setObjectName("roomMembers"); layout->addWidget(members_);
    roomUpdateState_ = new QLabel; roomUpdateState_->setWordWrap(true); roomUpdateState_->setObjectName("roomUpdateState"); layout->addWidget(roomUpdateState_);
    auto edited = [this] { editingRoom_ = true; };
    connect(nickname_, &QLineEdit::textChanged, this, [this] { editingNickname_ = true; }); connect(name_, &QLineEdit::textChanged, this, edited);
    connect(publicRoom_, &QCheckBox::toggled, this, edited);
    connect(viewerLimit_, &QSpinBox::valueChanged, this, edited);
    connect(reloadRoom, &QPushButton::clicked, this, [this] {
        if (!session_.roomUpdatePending()) { editingRoom_ = editingNickname_ = false; roomUpdateState_->clear(); }
    });
    auto lockRoomFields = [this] {
        nickname_->setEnabled(false); name_->setEnabled(false); publicRoom_->setEnabled(false); viewerLimit_->setEnabled(false);
        updateNickname_->setEnabled(false); updatePolicy_->setEnabled(false);
    };
    connect(updateNickname_, &QPushButton::clicked, this, [this, lockRoomFields] {
        updatingNickname_ = true; lockRoomFields();
        roomUpdateState_->setText("Updating nickname…"); session_.updateNickname(nickname_->text().toStdString(), nicknameRevision_);
    });
    connect(updatePolicy_, &QPushButton::clicked, this, [this, lockRoomFields] {
        updatingNickname_ = false; lockRoomFields();
        roomUpdateState_->setText("Updating room…");
        session_.updatePolicy({name_->text().toStdString(), publicRoom_->isChecked(), viewerLimit_->value()}, editRevision_);
    });
    video_ = new VideoFrameWidget; video_->setMinimumSize(320, 180); video_->setVisible(!config.room.host && config.preview);
    video_->setObjectName("roomVideo");
    layout->addWidget(video_, 1);
    auto* formWidget = new QWidget; auto* form = new QFormLayout(formWidget); formWidget->setVisible(config.room.host);
    auto combo = [&](const char* label, QStringList values, int selected) { auto* field = new QComboBox; field->addItems(values); field->setCurrentIndex(selected); form->addRow(label, field); return field; };
    auto number = [&](const char* label, int minimum, int maximum, int value) { auto* field = new QSpinBox; field->setRange(minimum, maximum); field->setValue(value); form->addRow(label, field); return field; };
    const auto& p = config.media.preferences;
    preset_ = combo("Preset", {"Gaming", "Quality"}, int(p.preset));
    resolution_ = combo("Resolution", {"Auto", "Fixed", "Native"}, int(p.resolution));
    width_ = number("Width", 2, 3840, p.width); width_->setSingleStep(2); width_->setObjectName("streamWidth");
    height_ = number("Height", 2, 2160, p.height); height_->setSingleStep(2); height_->setObjectName("streamHeight");
    fpsMode_ = combo("Frame rate mode", {"Auto", "Manual"}, int(p.fpsMode)); fps_ = number("FPS", 1, 240, p.fps);
    bitrateMode_ = combo("Bitrate mode", {"Auto", "Manual"}, int(p.bitrateMode));
    bitrate_ = number("Bitrate limit (bits/s)", 1000, 100000000, p.bitrateLimitBps.value_or(12000000));
    bitrateLimit_ = new QCheckBox("Also limit Auto bitrate"); bitrateLimit_->setChecked(p.bitrateLimitBps.has_value()); form->addRow(bitrateLimit_);
    apply_ = new QPushButton("Apply settings"); apply_->setObjectName("applyStream"); form->addRow(apply_); layout->addWidget(formWidget);
    settingsState_ = new QLabel; settingsState_->setWordWrap(true); layout->addWidget(settingsState_);
    auto* controls = new QLabel("Remote control is not available in this preview."); layout->addWidget(controls);
    stop_ = new QPushButton("Stop"); stop_->setObjectName("stopRoom"); layout->addWidget(stop_);
    connect(stop_, &QPushButton::clicked, this, [this] { session_.stop(); stop_->setEnabled(false); apply_->setEnabled(false); });
    connect(apply_, &QPushButton::clicked, this, [this] {
        StreamPreferences p; p.preset = StreamPreset(preset_->currentIndex()); p.resolution = ResolutionMode(resolution_->currentIndex());
        p.width = width_->value(); p.height = height_->value(); p.fpsMode = SettingMode(fpsMode_->currentIndex()); p.fps = fps_->value();
        p.bitrateMode = SettingMode(bitrateMode_->currentIndex());
        if (p.bitrateMode == SettingMode::Manual || bitrateLimit_->isChecked()) p.bitrateLimitBps = bitrate_->value();
        error_->clear(); settingsState_->setText("Settings pending…"); session_.apply(p);
    });
    session_.statusChanged = [this, host = config.room.host](const auto& value) {
        phase_->setText(Phase(value.phase) + QString(" — %1 connected, %2 pending, %3 failed").arg(value.activePeers).arg(value.pendingPeers).arg(value.failedPeers));
        room_->setText("Room: " + QString::fromStdString(value.roomId));
        roomLink_->setText(MakeRoomLink(QString::fromStdString(value.roomId)));
        copyLink_->setEnabled(value.phase == RoomPhase::Active && !roomLink_->text().isEmpty());
        const bool editable = value.phase == RoomPhase::Active && !session_.roomUpdatePending();
        updateNickname_->setEnabled(editable); updatePolicy_->setEnabled(host && editable);
        nickname_->setEnabled(editable); name_->setEnabled(host && editable);
        publicRoom_->setEnabled(host && editable); viewerLimit_->setEnabled(host && editable);
        if (!editingRoom_ && !session_.roomUpdatePending()) {
            const QSignalBlocker nameBlock(name_), publicBlock(publicRoom_), limitBlock(viewerLimit_);
            editRevision_ = value.revision;
            name_->setText(QString::fromStdString(value.policy.name)); publicRoom_->setChecked(value.policy.publicRoom);
            viewerLimit_->setValue(value.policy.viewerLimit);
        }
        if (!editingNickname_ && !session_.roomUpdatePending()) {
            const QSignalBlocker nicknameBlock(nickname_); nicknameRevision_ = value.revision;
            for (const auto& member : value.members) if (member.peerId == value.peerId) nickname_->setText(QString::fromStdString(member.nickname));
        }
        QStringList members;
        for (const auto& member : value.members) members << QString::fromStdString(member.nickname) + (member.host ? " (host)" : " (viewer)");
        members_->setText("Members: " + members.join(", "));
        apply_->setEnabled(host && value.phase == RoomPhase::Active);
        if (host && session_.settingsPending()) settingsState_->setText("Settings pending…");
        else if (host && value.stream.requestedRevision) {
            size_t complete = 0, rejected = 0;
            for (const auto& peer : value.stream.peers) {
                complete += peer.appliedRevision == value.stream.requestedRevision && peer.observedRevision == value.stream.requestedRevision;
                rejected += peer.rejected;
            }
            settingsState_->setText(value.stream.peers.empty() ? QString("Settings saved. Waiting for viewers.") :
                QString("Stream settings: %1 applied, %2 pending, %3 rejected.")
                .arg(complete).arg(value.stream.peers.size() - complete - rejected).arg(rejected));
        }
        if (value.phase == RoomPhase::Failed) error_->setText("The room session failed. Stop and start a new session to retry.");
    };
    session_.settingsAccepted = [this](const auto& result) { if (result.error != StreamUpdateError::None) error_->setText("The settings update was rejected."); };
    session_.roomUpdated = [this](const auto& result) {
        switch (result.error) {
        case RoomUpdateError::None:
            if (updatingNickname_) editingNickname_ = false; else editingRoom_ = false;
            roomUpdateState_->setText("Server confirmed the update."); break;
        case RoomUpdateError::Conflict: roomUpdateState_->setText("The room changed while you were editing. Reload current values and review your change."); break;
        case RoomUpdateError::Unconfirmed: roomUpdateState_->setText("The server outcome is unknown. Check current room values before trying again."); break;
        case RoomUpdateError::Invalid: roomUpdateState_->setText("Invalid name or room settings."); break;
        case RoomUpdateError::Forbidden: roomUpdateState_->setText("Only the host can change room settings."); break;
        default: roomUpdateState_->setText("The room update was not accepted."); break;
        }
    };
    if (!config.room.host && config.preview) session_.frameReady = [this](screenshare::DecodedFrameInfo frame) {
        screenshare::SessionEvent::VideoFrame output;
        output.width = frame.width; output.height = frame.height; output.codedWidth = frame.codedWidth; output.codedHeight = frame.codedHeight;
        output.timestamp100ns = frame.timestamp100ns; output.duration100ns = frame.duration100ns;
        const auto* data = reinterpret_cast<const uint8_t*>(frame.data.data()); output.nv12.assign(data, data + frame.data.size());
        video_->setVideoFrame(std::move(output));
    };
    session_.error = [this](const auto& message) { error_->setText(message); };
    session_.finished = [this](const auto&) { stop_->setEnabled(false); apply_->setEnabled(false); if (closing_) QTimer::singleShot(0, this, [this] { close(); }); };
    if (!session_.start(std::move(config))) { stop_->setEnabled(false); apply_->setEnabled(false); }
}
RoomSessionWindow::~RoomSessionWindow() = default;
void RoomSessionWindow::closeEvent(QCloseEvent* event) {
    if (session_.running()) { closing_ = true; session_.stop(); event->ignore(); }
    else { event->accept(); if (closed) closed(); }
}
int RunRoomSessionWindow(const QString& path) {
    try {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) throw std::invalid_argument("Cannot open room configuration");
        const auto bytes = file.read(65537);
        if (bytes.size() > 65536) throw std::invalid_argument("Room configuration exceeds 64 KiB");
        QJsonParseError error; const auto document = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) throw std::invalid_argument("Invalid room configuration JSON");
        auto config = ParseRoomSessionConfig(document.object());
        webrtc::WinsockInitializer winsock;
        if (winsock.error() || !webrtc::InitializeSSL()) throw std::runtime_error("Media networking initialization failed");
        struct Ssl { ~Ssl() { webrtc::CleanupSSL(); } } ssl;
        webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
        webrtc::InitializeLogging(std::move(logging));
        RoomSessionWindow window(std::move(config)); window.show();
        return QApplication::exec();
    } catch (const std::exception& error) {
        QMessageBox::critical(nullptr, "Cannot start room", QString::fromUtf8(error.what())); return 1;
    }
}
