#include "ui/RoomSessionWindow.h"
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
    error_ = new QLabel; error_->setObjectName("roomError"); error_->setTextFormat(Qt::PlainText); error_->setWordWrap(true); layout->addWidget(error_);
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
    else event->accept();
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
