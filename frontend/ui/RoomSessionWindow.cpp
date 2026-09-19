#include "ui/RoomSessionWindow.h"
#include "ui/RoomGamepadControl.h"
#include "ui/RoomApplication.h"
#include "shared/RoomLink.h"
#include "shared/RoomProfile.h"
#include "ui/PeerDiagnosticsWidget.h"
#include "shared/PresentationDiagnostics.h"
#include "shared/PipelineDiagnostics.h"
#include "shared/RoomDiagnosticReport.h"
#include "shared/FrameQueueDiagnostics.h"
#include "ui/UiReportPath.h"
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
#include <QScrollArea>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QStackedWidget>
#include <QTabWidget>
#include <QSlider>
#include <QShortcut>
#include <QResizeEvent>
#include <QThread>
#include <QPainter>
#include <climits>
using namespace screenshare::v2;
using namespace screenshare::media;
namespace {
class SourcePreviewLabel final : public QLabel {
public:
    using QLabel::QLabel;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);painter.fillRect(rect(),QColor("#080e0b"));
        const auto original=pixmap();const auto target=original.width()<=80?QSize(80,80):size();
        const auto image=original.scaled(target,Qt::KeepAspectRatio,Qt::SmoothTransformation);
        painter.drawPixmap((width()-image.width())/2,(height()-image.height())/2,image);
    }
};
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
RoomSessionWindow::RoomSessionWindow(RoomSessionConfig config, QtRoomSession::Factory factory, bool loopback, RoomProfile* profile,
    RoomGamepadControl::Devices devices, RoomGamepadControl::Read read, FramePresentationFactory presentation)
    : session_(nullptr, std::move(factory), loopback) {
    setWindowTitle(config.room.host ? "ScreenShare — Share room" : "ScreenShare — Watch room");
    setStyleSheet(uiStyleSheet()); resize(960, 720);
    setObjectName("RoomSession"); setAttribute(Qt::WA_StyledBackground);
    auto* pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName("roomSessionScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    // The D3D surface intentionally does not create native ancestors. Give it
    // a scrolling native parent and a native viewport so Windows moves and
    // clips the swap-chain window with the Qt content.
    scroll->viewport()->setAttribute(Qt::WA_NativeWindow);
    auto* content = new QWidget;
    content->setAttribute(Qt::WA_NativeWindow);
    auto* layout = new QVBoxLayout(content);
    UiSpacing::applyPage(layout);
    scroll->setWidget(content);
    pageLayout->addWidget(scroll);
    phase_ = new QLabel("Starting…"); phase_->setObjectName("roomPhase"); layout->addWidget(phase_);
    room_ = new QLabel; room_->setTextFormat(Qt::PlainText); room_->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(room_);
    roomLink_ = new QLineEdit; roomLink_->setReadOnly(true); roomLink_->setObjectName("roomLink"); layout->addWidget(roomLink_);
    roomLink_->setToolTip("Share with someone using the same service. Passwords must be shared separately.");
    copyLink_ = new QPushButton("Copy room link"); copyLink_->setObjectName("copyRoomLink"); copyLink_->setEnabled(false); layout->addWidget(copyLink_);
    connect(copyLink_, &QPushButton::clicked, this, [this] { if (!roomLink_->text().isEmpty()) QApplication::clipboard()->setText(roomLink_->text()); });
    error_ = new QLabel; error_->setObjectName("roomError"); error_->setTextFormat(Qt::PlainText); error_->setWordWrap(true); layout->addWidget(error_);
    auto* exportReport = new QPushButton("Save diagnostic report", this); exportReport->setObjectName("saveRoomReport"); layout->addWidget(exportReport);
    auto* reportResult = new QLabel(this); reportResult->setObjectName("roomReportResult"); reportResult->setTextFormat(Qt::PlainText);
    reportResult->setWordWrap(true); reportResult->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(reportResult);
    connect(exportReport, &QPushButton::clicked, this, [this, reportResult, configured = config.reportFile] {
        const auto file = configured.isEmpty() ? "room-v2-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json" : configured;
        const auto path = ResolveUiReportPath(file);
        const auto input = session_.input();
        auto report = RoomDiagnosticReport(session_.status(), input ? input->Read() : std::vector<screenshare::input::Status>{});
        report["decodedFrameHandoff"] = FrameQueueDiagnostics(session_.frameStatistics());
        reportResult->setText(WriteRoomDiagnosticReport(path, report) ? "Saved diagnostic report: " + path : "Could not save diagnostic report. Check the destination is writable.");
    });
    auto* roomForm = new QFormLayout;
    roomForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    nickname_ = new QLineEdit; nickname_->setObjectName("liveNickname"); nickname_->setMaxLength(128);
    name_ = new QLineEdit; name_->setObjectName("liveRoomName"); name_->setMaxLength(256);
    publicRoom_ = new QCheckBox("Public room"); publicRoom_->setObjectName("livePublicRoom");
    viewerLimit_ = new QSpinBox; viewerLimit_->setRange(1, 63); viewerLimit_->setObjectName("liveViewerLimit");
    auto* capacityWarning = new QLabel("Above four viewers, each additional video connection adds upload and encoding work. Performance may be lower.", this);
    capacityWarning->setObjectName("capacityWarning"); capacityWarning->setWordWrap(true); capacityWarning->hide();
    connect(viewerLimit_, &QSpinBox::valueChanged, this, [capacityWarning, host = config.room.host](int value) {
        capacityWarning->setVisible(host && value > 4);
    });
    updateNickname_ = new QPushButton("Update nickname for this session"); updateNickname_->setObjectName("updateNickname");
    updatePolicy_ = new QPushButton("Update room"); updatePolicy_->setObjectName("updateRoomPolicy");
    roomForm->addRow("Nickname", nickname_); roomForm->addRow(updateNickname_);
    if (config.room.host) {
        roomForm->addRow("Room name", name_); roomForm->addRow(publicRoom_);
        roomForm->addRow("Viewer limit", viewerLimit_); roomForm->addRow(updatePolicy_);
        roomForm->addRow(capacityWarning);
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
    video_ = new VideoFrameWidget(nullptr,std::move(presentation)); video_->setMinimumSize(320, 180); video_->setVisible(!config.room.host && config.preview);
    video_->setObjectName("roomVideo");
    video_->setLowLatency(true);
    layout->addWidget(video_, 1);
    auto* playbackWidget = new QWidget; auto* playbackForm = new QFormLayout(playbackWidget);
    playbackForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    playbackWidget->setVisible(!config.room.host); layout->addWidget(playbackWidget);
    playbackDevice_ = new QComboBox; playbackDevice_->setObjectName("playbackDevice");
    playbackDevice_->addItem(config.media.playbackDeviceId.empty() ? "Default output" : "Current output", QString::fromStdWString(config.media.playbackDeviceId));
    playbackForm->addRow("Playback device", playbackDevice_);
    playbackVolume_ = new QSpinBox; playbackVolume_->setRange(0, 100); playbackVolume_->setSuffix("%");
    playbackVolume_->setValue(int(config.media.playbackVolume)); playbackVolume_->setObjectName("playbackVolume"); playbackForm->addRow("Volume", playbackVolume_);
    playbackMuted_ = new QCheckBox("Mute playback"); playbackMuted_->setChecked(config.media.playbackMuted); playbackMuted_->setObjectName("playbackMuted"); playbackForm->addRow(playbackMuted_);
    refreshPlayback_ = new QPushButton("Refresh output devices"); refreshPlayback_->setObjectName("refreshPlaybackDevices"); playbackForm->addRow(refreshPlayback_);
    applyPlayback_ = new QPushButton("Apply playback settings"); applyPlayback_->setObjectName("applyPlayback"); applyPlayback_->setEnabled(false); playbackForm->addRow(applyPlayback_);
    playbackState_ = new QLabel; playbackState_->setWordWrap(true); playbackState_->setObjectName("playbackState"); playbackForm->addRow(playbackState_);
    playbackHealth_ = new QLabel; playbackHealth_->setWordWrap(true); playbackHealth_->setObjectName("playbackHealth"); playbackForm->addRow(playbackHealth_);
    connect(refreshPlayback_, &QPushButton::clicked, this, [this] {
        try {
            const auto selected = playbackDevice_->currentData(); playbackDevice_->clear(); playbackDevice_->addItem("Default output", QString());
            for (const auto& device : screenshare::WasapiCapture::EnumerateDevices(screenshare::AudioCaptureSource::SystemOutput))
                playbackDevice_->addItem(QString::fromStdWString(device.name), QString::fromStdWString(device.id));
            const auto index = playbackDevice_->findData(selected); if (index >= 0) playbackDevice_->setCurrentIndex(index);
        } catch (...) { playbackState_->setText("Could not enumerate output devices."); }
    });
    connect(applyPlayback_, &QPushButton::clicked, this, [this] {
        applyPlayback_->setEnabled(false); playbackState_->setText("Applying playback settings…");
        session_.updatePlayback({playbackDevice_->currentData().toString().toStdWString(), unsigned(playbackVolume_->value()), playbackMuted_->isChecked()});
    });
    session_.playbackUpdated = [this](const auto& result) {
        if (result.error == AudioUpdateError::None) playbackState_->setText("Playback settings applied.");
        else if (result.error == AudioUpdateError::Cancelled) playbackState_->setText("Playback change cancelled.");
        else if (result.error == AudioUpdateError::Unavailable) playbackState_->setText("Playback is not active yet.");
        else playbackState_->setText("Could not change playback. Previous settings retained while available.");
    };
    auto* formWidget = new QWidget; auto* form = new QFormLayout(formWidget); formWidget->setVisible(config.room.host);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto combo = [&](const char* label, QStringList values, int selected) { auto* field = new QComboBox; field->addItems(values); field->setCurrentIndex(selected); form->addRow(label, field); return field; };
    auto number = [&](const char* label, int minimum, int maximum, int value) { auto* field = new QSpinBox; field->setRange(minimum, maximum); field->setValue(value); form->addRow(label, field); return field; };
    const auto& p = config.media.preferences;
    captureSource_ = new QComboBox; captureSource_->setObjectName("liveCaptureSource"); form->addRow("Capture source", captureSource_);
    auto currentSource = QVariantMap{{"display", config.media.capture.displayIndex}};
    if (config.media.capture.sourceType == screenshare::CaptureSourceType::Window) currentSource = {{"window", QVariant::fromValue<qulonglong>(config.media.capture.windowHandle)}};
    captureSource_->addItem("Current source", currentSource);
    refreshCapture_ = new QPushButton("Refresh capture sources"); refreshCapture_->setObjectName("refreshCaptureSources"); form->addRow(refreshCapture_);
    switchCapture_ = new QPushButton("Share selected source"); switchCapture_->setObjectName("switchCaptureSource"); switchCapture_->setEnabled(false); form->addRow(switchCapture_);
    captureState_ = new QLabel; captureState_->setWordWrap(true); captureState_->setObjectName("captureState"); form->addRow(captureState_);
    connect(refreshCapture_, &QPushButton::clicked, this, [this] {
        try {
            const auto selected = captureSource_->currentData(); captureSource_->clear();
            for (const auto& display : screenshare::DesktopCapturer::EnumerateDisplays())
                captureSource_->addItem(QString("Display %1").arg(display.index + 1), QVariantMap{{"display", display.index}});
            for (const auto& window : screenshare::DesktopCapturer::EnumerateWindows())
                captureSource_->addItem(QString::fromStdWString(window.title), QVariantMap{{"window", QVariant::fromValue<qulonglong>(window.handle)}});
            const auto index = captureSource_->findData(selected); if (index >= 0) captureSource_->setCurrentIndex(index);
        } catch (...) { captureState_->setText("Could not enumerate capture sources."); }
    });
    connect(switchCapture_, &QPushButton::clicked, this, [this, captureFps = config.media.capture.targetFps] {
        const auto selected = captureSource_->currentData().toMap(); if (selected.isEmpty()) return;
        CaptureSelection selection; selection.fps = captureFps;
        if (selected.contains("window")) { selection.kind = CaptureKind::Window; selection.window = selected["window"].toULongLong(); }
        else selection.display = selected["display"].toInt();
        captureState_->setText("Waiting for the new source…"); switchCapture_->setEnabled(false); session_.switchCapture(selection);
    });
    audioKind_ = combo("Shared audio", {"System output", "Microphone", "Process output", "No shared audio"}, int(config.media.audio.source));
    audioKind_->setObjectName("liveAudioKind");
    audioDevice_ = new QComboBox; audioDevice_->setObjectName("liveAudioDevice"); form->addRow("Audio device", audioDevice_);
    audioDevice_->addItem(config.media.audio.deviceId.empty() ? "Default device" : "Current device", QString::fromStdWString(config.media.audio.deviceId));
    audioProcess_ = number("Audio process ID", 0, INT_MAX, int(config.media.audio.processId)); audioProcess_->setObjectName("liveAudioProcess");
    refreshAudio_ = new QPushButton("Refresh audio devices"); refreshAudio_->setObjectName("refreshAudioDevices"); form->addRow(refreshAudio_);
    switchAudio_ = new QPushButton("Share selected audio"); switchAudio_->setObjectName("switchAudioSource"); switchAudio_->setEnabled(false); form->addRow(switchAudio_);
    audioState_ = new QLabel; audioState_->setWordWrap(true); audioState_->setObjectName("audioState"); form->addRow(audioState_);
    audioHealth_ = new QLabel; audioHealth_->setWordWrap(true); audioHealth_->setObjectName("audioHealth"); form->addRow(audioHealth_);
    connect(audioKind_, &QComboBox::currentIndexChanged, this, [this] {
        audioDevice_->clear(); audioDevice_->addItem("Default device", QString());
        audioDevice_->setEnabled(audioKind_->currentIndex() < 2); audioProcess_->setEnabled(audioKind_->currentIndex() == 2);
        refreshAudio_->setEnabled(audioKind_->currentIndex() < 2);
    });
    connect(refreshAudio_, &QPushButton::clicked, this, [this] {
        try {
            const auto selected = audioDevice_->currentData(); audioDevice_->clear(); audioDevice_->addItem("Default device", QString());
            if (audioKind_->currentIndex() < 2) for (const auto& device : screenshare::WasapiCapture::EnumerateDevices(
                audioKind_->currentIndex() == 1 ? screenshare::AudioCaptureSource::Microphone : screenshare::AudioCaptureSource::SystemOutput))
                audioDevice_->addItem(QString::fromStdWString(device.name), QString::fromStdWString(device.id));
            const auto index = audioDevice_->findData(selected); if (index >= 0) audioDevice_->setCurrentIndex(index);
        } catch (...) { audioState_->setText("Could not enumerate audio devices."); }
    });
    connect(switchAudio_, &QPushButton::clicked, this, [this] {
        AudioSelection selection; selection.kind = AudioKind(audioKind_->currentIndex());
        if (selection.kind == AudioKind::Process) selection.processId = uint32_t(audioProcess_->value());
        else if (selection.kind != AudioKind::None) selection.deviceId = audioDevice_->currentData().toString().toStdWString();
        audioState_->setText("Waiting for the new audio source…"); switchAudio_->setEnabled(false); session_.switchAudio(std::move(selection));
    });
    session_.audioUpdated = [this](const AudioUpdateResult& result) {
        if (result.error == AudioUpdateError::None) audioState_->setText(session_.status().audio.selected.kind == AudioKind::None ?
            "No audio is being shared." : "Sharing the selected audio source.");
        else if (result.error == AudioUpdateError::Unavailable) audioState_->setText("Audio capture is not active. Connect a viewer first.");
        else if (result.error == AudioUpdateError::Cancelled) audioState_->setText("Audio change cancelled.");
        else audioState_->setText("Could not switch audio. The previous source is retained while available.");
    };
    preset_ = combo("Preset", {"Gaming", "Quality"}, int(p.preset));
    preset_->setObjectName("streamPreset");
    preset_->setToolTip("Gaming favors low-delay video playback and may be less smooth on unstable networks. "
        "Quality allows adaptive buffering for smoother playback. Your resolution, FPS and bitrate selections are kept.");
    resolution_ = combo("Resolution", {"Auto", "Fixed", "Native"}, int(p.resolution));
    width_ = number("Width", 2, 3840, p.width); width_->setSingleStep(2); width_->setObjectName("streamWidth");
    height_ = number("Height", 2, 2160, p.height); height_->setSingleStep(2); height_->setObjectName("streamHeight");
    fpsMode_ = combo("Frame rate mode", {"Auto", "Manual"}, int(p.fpsMode)); fps_ = number("FPS", 1, 240, p.fps);
    bitrateMode_ = combo("Bitrate mode", {"Auto", "Manual"}, int(p.bitrateMode));
    resolution_->setObjectName("streamResolutionMode"); fpsMode_->setObjectName("streamFpsMode");
    bitrateMode_->setObjectName("streamBitrateMode"); fps_->setObjectName("streamFps");
    bitrate_ = number("Bitrate limit (bits/s)", 1000, 100000000, p.bitrateLimitBps.value_or(12000000));
    bitrate_->setObjectName("streamBitrate");
    bitrateLimit_ = new QCheckBox("Also limit Auto bitrate"); bitrateLimit_->setChecked(p.bitrateLimitBps.has_value()); form->addRow(bitrateLimit_);
    uploadBudgetEnabled_ = new QCheckBox("Limit total media upload allowance"); uploadBudgetEnabled_->setObjectName("uploadBudgetEnabled");
    uploadBudgetEnabled_->setChecked(p.aggregateUploadLimitBps.has_value()); form->addRow(uploadBudgetEnabled_);
    uploadBudget_ = number("Upload allowance (bits/s)", 160000, 1000000000, p.aggregateUploadLimitBps.value_or(20000000)); uploadBudget_->setObjectName("uploadBudget");
    apply_ = new QPushButton("Apply settings"); apply_->setObjectName("applyStream"); form->addRow(apply_);
    auto* sourceSettings = new QScrollArea; sourceSettings->setWidgetResizable(true); sourceSettings->setWidget(formWidget);
    sourceSettings->setVisible(config.room.host); sourceSettings->setMinimumHeight(160); layout->addWidget(sourceSettings, 1);
    settingsState_ = new QLabel; settingsState_->setObjectName("streamSettingsState"); settingsState_->setWordWrap(true); layout->addWidget(settingsState_);
    uploadState_ = new QLabel; uploadState_->setWordWrap(true); uploadState_->setObjectName("uploadState"); uploadState_->setVisible(config.room.host); layout->addWidget(uploadState_);
    auto* peerDiagnostics = new PeerDiagnosticsWidget(this);
    peerDiagnostics->setVisible(config.room.host); layout->addWidget(peerDiagnostics);
    if (profile) {
        auto* saveDefaults = new QPushButton(config.room.host ? "Save stream settings for new rooms" : "Save playback settings for new sessions");
        saveDefaults->setObjectName("saveSessionDefaults"); layout->addWidget(saveDefaults);
        auto* savedState = new QLabel; savedState->setObjectName("profileSaveState"); savedState->setWordWrap(true); layout->addWidget(savedState);
        connect(saveDefaults, &QPushButton::clicked, this, [this, profile, savedState, host = config.room.host] {
            const bool saved = host ? profile->saveStreamPreferences(ReadPreferences()) :
                profile->savePlayback({playbackVolume_->value(), playbackMuted_->isChecked()});
            savedState->setText(saved ? "Saved for new sessions. Use Apply to change this session." :
                "Settings are invalid or could not be saved. Previous defaults were not replaced by invalid values.");
        });
    }
    gamepad_ = new RoomGamepadControl(config.room.host, [this] { return session_.input(); },
        [this] { return session_.status(); }, this, std::move(devices), std::move(read));
    layout->addWidget(gamepad_);
    gamepad_->SetVideo(video_);
    gamepad_->prepareGrant=[this](uint8_t caps){return session_.prepareInputGrant(caps);};
    stop_ = new QPushButton("Stop"); stop_->setObjectName("stopRoom"); layout->addWidget(stop_);
    connect(stop_, &QPushButton::clicked, this, [this] { close(); stop_->setEnabled(false); apply_->setEnabled(false); });
    connect(apply_, &QPushButton::clicked, this, [this] {
        error_->clear(); settingsState_->setText("Settings pending…"); session_.apply(ReadPreferences());
    });
    session_.statusChanged = [this, peerDiagnostics, capacityWarning, host = config.room.host](const auto& value) {
        phase_->setText(Phase(value.phase));
        phase_->setToolTip(QString("%1 connected, %2 pending, %3 failed").arg(value.activePeers).arg(value.pendingPeers).arg(value.failedPeers));
        room_->setText((host ? "Hosting: " : "") + QString::fromStdString(value.policy.name.empty()?value.roomId:value.policy.name));
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
        capacityWarning->setVisible(host && viewerLimit_->value() > 4);
        if (!editingNickname_ && !session_.roomUpdatePending()) {
            const QSignalBlocker nicknameBlock(nickname_); nicknameRevision_ = value.revision;
            for (const auto& member : value.members) if (member.peerId == value.peerId) nickname_->setText(QString::fromStdString(member.nickname));
        }
        QStringList members;
        for (const auto& member : value.members) {
            const bool duplicate = std::count_if(value.members.begin(), value.members.end(), [&](const auto& other) { return other.nickname == member.nickname; }) > 1;
            members << QString::fromStdString(member.nickname) + (duplicate ? " [" + QString::fromStdString(member.peerId) + "]" : "") +
                (member.host ? " (host)" : " (viewer)");
        }
        members_->setText("Members: " + members.join(", "));
        apply_->setEnabled(host && value.phase == RoomPhase::Active);
        const bool playbackEditable = !host && value.phase == RoomPhase::Active && !session_.playbackPending();
        const bool playbackFailed = value.playback.health.state == AudioEndpointState::Failed;
        playbackHealth_->setText(playbackFailed ? "Audio output failed. Video continues. Retry playback or select another output." :
            value.playback.health.state == AudioEndpointState::Running ? "Audio output active." : "Audio output inactive.");
        applyPlayback_->setText(playbackFailed ? "Retry playback" : "Apply playback settings");
        applyPlayback_->setEnabled(playbackEditable); refreshPlayback_->setEnabled(playbackEditable);
        playbackDevice_->setEnabled(playbackEditable); playbackVolume_->setEnabled(playbackEditable); playbackMuted_->setEnabled(playbackEditable);
        switchCapture_->setEnabled(host && value.phase == RoomPhase::Active && !session_.capturePending());
        const bool audioEditable = host && value.phase == RoomPhase::Active && !session_.audioPending();
        const bool audioFailed = value.audio.health.state == AudioEndpointState::Failed;
        audioHealth_->setText(audioFailed ? "Audio capture failed. Video continues without shared audio. Retry or select another source." :
            value.audio.health.state == AudioEndpointState::Running ? (value.audio.microphoneProcessing ?
                "Microphone active: noise suppression and digital gain. Echo cancellation is unavailable." : "Audio capture active; speech processing bypassed.") :
            value.audio.health.state == AudioEndpointState::Silent ? "Audio capture disabled." : "Audio capture inactive.");
        switchAudio_->setText(audioFailed ? "Retry selected audio" : "Share selected audio");
        switchAudio_->setEnabled(audioEditable && value.activePeers > 0); refreshAudio_->setEnabled(audioEditable && audioKind_->currentIndex() < 2);
        audioKind_->setEnabled(audioEditable); audioDevice_->setEnabled(audioEditable && audioKind_->currentIndex() < 2);
        audioProcess_->setEnabled(audioEditable && audioKind_->currentIndex() == 2);
        refreshCapture_->setEnabled(host && !session_.capturePending()); captureSource_->setEnabled(host && !session_.capturePending());
        if (host) {
            peerDiagnostics->Update(value);
            qint64 allocated = 0, applied = 0; uint64_t measured = 0; size_t paused = 0, measuredPeers = 0;
            for (const auto& peer : value.stream.peers) {
                allocated += peer.allocatedVideoBitrateBps; applied += peer.appliedVideoBitrateBps;
                paused += peer.appliedRevision && peer.appliedVideoBitrateBps == 0;
                if (peer.transportSendBps) { measured += *peer.transportSendBps; ++measuredPeers; }
            }
            const auto measuredText = measuredPeers && measuredPeers == value.stream.peers.size() ?
                QString("Measured WebRTC transport upload: %1 Mbps (excludes IP/interface overhead).").arg(measured / 1000000.0, 0, 'f', 2) :
                QString("Measured transport upload: waiting for fresh samples from every viewer.");
            uploadState_->setText(QString("Video caps: %1 Mbps allocated, %2 Mbps applied; %3 viewer(s) paused by the upload allowance. ")
                .arg(allocated / 1000000.0, 0, 'f', 2).arg(applied / 1000000.0, 0, 'f', 2).arg(paused) + measuredText);
        }
        if (host && session_.settingsPending()) settingsState_->setText("Settings pending…");
        else if (host && value.stream.requestedRevision) {
            const auto application = StreamApplicationJson(value.stream);
            settingsState_->setText(value.stream.peers.empty() ? QString("Settings saved. Waiting for viewers.") :
                QString("Stream settings: %1 applied, %2 pending, %3 rejected. %4")
                .arg(application["applied"].toInt()).arg(application["pending"].toInt()).arg(application["rejected"].toInt())
                .arg(application["rejected"].toInt() ? "Some viewers retain earlier settings. Review viewer details, then Apply to retry." : ""));
        }
        if (value.phase == RoomPhase::Failed) error_->setText("The room session failed. Stop and start a new session to retry.");
        error_->setVisible(!error_->text().isEmpty());
    };
    session_.settingsAccepted = [this](const auto& result) { if (result.error != StreamUpdateError::None) error_->setText("The settings update was rejected."); };
    session_.captureUpdated = [this](const auto& result) {
        if (result.error == CaptureUpdateError::None) captureState_->setText("Sharing the new source. Receiver reports appear in viewer details.");
        else if (result.error == CaptureUpdateError::Cancelled) captureState_->setText("Source change cancelled.");
        else captureState_->setText("Could not change source. The previous source remains selected if it is still available.");
    };
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
    if (!config.room.host && config.preview) session_.frameReady = [this](screenshare::Nv12VideoFrame frame) {
        video_->setVideoFrame(std::move(frame));
    };
    if (!config.room.host && config.preview) {
        auto* diagnostics = new QLabel(this); diagnostics->setObjectName("viewerPresentationDiagnostics");
        diagnostics->setTextFormat(Qt::PlainText); diagnostics->setWordWrap(true); layout->addWidget(diagnostics);
        auto* presentationStatus = new QTimer(this);
        presentationStatus->setInterval(1000);
        connect(presentationStatus, &QTimer::timeout, this, [this, diagnostics] {
            const auto stats = video_->presentationStats();
            const auto& renderer = stats.renderer;
            session_.reportPresentation({stats.presentedFrames, stats.droppedFrames,
                uint8_t(stats.queuedFrames), uint8_t(renderer.outcome)});
            const auto fields = PresentationDiagnosticsJson(renderer);
            const auto frames = session_.frameStatistics();
            diagnostics->setText(QString("Local preview: %1. Presented %2; dropped %3; pending %4.\nDrops: busy %5, occluded %6, minimized %7, unavailable %8, recovery backoff %9.\nGraphics errors %10; rebuilds %11; last error %12. GPU frames %13; CPU readbacks %14. These counters do not measure end-to-end latency.")
                .arg(fields["outcome"].toString()).arg(stats.presentedFrames).arg(stats.droppedFrames).arg(stats.queuedFrames)
                .arg(renderer.busyDrops).arg(renderer.occludedDrops).arg(renderer.minimizedDrops).arg(renderer.unavailableDrops)
                .arg(renderer.backoffDrops).arg(renderer.errors).arg(renderer.recoveries)
                .arg(fields["lastErrorCode"].isNull() ? "none" : fields["lastErrorCode"].toString())
                .arg(frames.gpuRetained).arg(frames.gpuReadbacks) +
                QString("\nDecoded-frame handoff: pending age %1 ms; last wait %2 ms; maximum wait %3 ms; conversion failures %4.")
                    .arg(frames.pendingAgeUs ? QString::number(double(*frames.pendingAgeUs) / 1000, 'f', 1) : "none")
                    .arg(frames.lastWaitUs ? QString::number(double(*frames.lastWaitUs) / 1000, 'f', 1) : "unknown")
                    .arg(double(frames.maxWaitUs) / 1000, 0, 'f', 1).arg(frames.failed));
            if (!stats.terminal) return;
            error_->setText("Video presentation failed. Leave and rejoin the room to retry. Audio and room controls remain available.");
        });
        presentationStatus->start();
    }
    for (auto* optionForm : findChildren<QFormLayout*>()) alignOptionRows(optionForm);
    // Compose the session dashboard from the existing, connected controls. The
    // backend/session remains the single owner of settings and permissions.
    auto* pages = new QStackedWidget;
    pages->setObjectName("SessionPages");
    pageLayout->removeWidget(scroll); pageLayout->addWidget(pages);
    auto* dashboard = new QWidget; dashboard->setObjectName("SessionDashboard"); dashboard->setAttribute(Qt::WA_StyledBackground);
    auto* dashboardLayout = new QVBoxLayout(dashboard); UiSpacing::applyPage(dashboardLayout);
    pages->addWidget(dashboard);
    auto* settingsPage = new QWidget; settingsPage->setObjectName("SessionSettings");settingsPage->setAttribute(Qt::WA_StyledBackground);
    auto* settingsLayout = new QVBoxLayout(settingsPage); UiSpacing::applyPage(settingsLayout);
    auto* backToSession = new QPushButton("Back to session"); backToSession->setObjectName("sessionSettingsBack");
    backToSession->setIcon(uiIcon("back")); settingsLayout->addWidget(backToSession,0,Qt::AlignLeft);
    auto* settingsTitle = new QLabel(config.room.host ? "Room settings" : "Playback settings"); settingsTitle->setObjectName("PageHeading"); settingsLayout->addWidget(settingsTitle);
    auto* settingsTabs = new QTabWidget; settingsTabs->setObjectName("SessionSettingsTabs"); settingsLayout->addWidget(settingsTabs,1);
    pages->addWidget(settingsPage);
    connect(backToSession,&QPushButton::clicked,this,[pages,dashboard]{pages->setCurrentWidget(dashboard);});
    auto move = [layout](QWidget* widget,QBoxLayout* destination,int stretch=0) { layout->removeWidget(widget); destination->addWidget(widget,stretch); };
    auto* sessionHeader = new QHBoxLayout;
    room_->setObjectName("SessionTitle"); room_->setWordWrap(true); move(room_,sessionHeader,1); move(phase_,sessionHeader);
    dashboardLayout->addLayout(sessionHeader);
    sessionColumns_ = new QBoxLayout(QBoxLayout::LeftToRight); sessionColumns_->setSpacing(UiSpacing::SectionGap);
    dashboardLayout->addLayout(sessionColumns_,1);
    auto card = [](QBoxLayout* parent) {
        auto* widget=new QWidget; widget->setObjectName("SessionCard");
        auto* body=new QVBoxLayout(widget); body->setContentsMargins(20,20,20,20); body->setSpacing(12);
        parent->addWidget(widget,1); return body;
    };
    auto* primary=card(sessionColumns_);
    auto* controlsScroll=new QScrollArea; controlsScroll->setObjectName("SessionControlsScroll"); controlsScroll->setWidgetResizable(true); controlsScroll->setFrameShape(QFrame::NoFrame);
    auto* controlsCard=new QWidget; controlsCard->setObjectName("SessionCard"); auto* controlsBody=new QVBoxLayout(controlsCard);
    controlsBody->setContentsMargins(20,20,20,20); controlsBody->setSpacing(16);
    controlsScroll->setWidget(controlsCard); controlsScroll->setMinimumWidth(280);controlsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); sessionColumns_->addWidget(controlsScroll,1);
    sessionColumns_->setStretch(0,config.room.host?3:5); sessionColumns_->setStretch(1,2);
    auto* controlsTitle=new QLabel(config.room.host?"Viewers":"Controls"); controlsTitle->setObjectName("SectionHeading"); controlsBody->addWidget(controlsTitle);
    move(gamepad_,controlsBody); controlsBody->addStretch();
    for(const auto* name:{"showInputDiagnostics","inputDiagnostics"}) if(auto* widget=gamepad_->findChild<QWidget*>(name)) {
        gamepad_->layout()->removeWidget(widget);layout->addWidget(widget);
    }
    if(config.room.host) {
        auto* sourceTitle=new QLabel("You’re sharing"); sourceTitle->setObjectName("SectionHeading"); primary->addWidget(sourceTitle);
        hostPreview_=new SourcePreviewLabel;hostPreview_->setObjectName("HostSourcePreview");hostPreview_->setAlignment(Qt::AlignCenter);
        hostPreview_->setMinimumHeight(80);hostPreview_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Expanding);
        hostPreview_->setPixmap(uiIcon("display").pixmap(80,80));primary->addWidget(hostPreview_,1);
        auto* sourceSummary=new QLabel; sourceSummary->setObjectName("SessionSourceSummary"); sourceSummary->setWordWrap(true); primary->addWidget(sourceSummary);
        auto* sourceActions=new QHBoxLayout;primary->addLayout(sourceActions);
        auto* changeSource=new QPushButton("Change source"); changeSource->setObjectName("sessionChangeSource"); changeSource->setIcon(uiIcon("display")); sourceActions->addWidget(changeSource);
        connect(changeSource,&QPushButton::clicked,this,[pages,settingsPage,settingsTabs]{settingsTabs->setCurrentIndex(0);pages->setCurrentWidget(settingsPage);});
        auto* muteAudio=new QPushButton("Mute audio");muteAudio->setObjectName("muteSharedAudio");muteAudio->setIcon(uiIcon("volume"));sourceActions->addWidget(muteAudio);
        auto previousAudio=std::make_shared<AudioSelection>();
        connect(muteAudio,&QPushButton::clicked,this,[this,previousAudio] {
            const auto current=session_.status().audio.selected;
            if(current.kind==AudioKind::None)session_.switchAudio(*previousAudio);
            else {*previousAudio=current;AudioSelection muted;muted.kind=AudioKind::None;session_.switchAudio(muted);}
        });
        auto* healthTitle=new QLabel("Stream health"); healthTitle->setObjectName("SectionHeading"); primary->addWidget(healthTitle);
        auto* health=new QLabel; health->setObjectName("SessionHealth"); health->setWordWrap(true); primary->addWidget(health);
        auto previous=session_.statusChanged;
        session_.statusChanged=[this,previous,sourceSummary,health,controlsTitle,muteAudio,loopback](const auto& status) {
            previous(status);
            muteAudio->setEnabled(status.phase==RoomPhase::Active && status.activePeers>0 && !session_.audioPending());
            muteAudio->setText(status.audio.selected.kind==AudioKind::None?"Share audio":"Mute audio");
            const auto& selected=status.capture.selected;
            if(!loopback && (!previewRevision_ || selected.kind!=previewSource_.kind || selected.display!=previewSource_.display || selected.window!=previewSource_.window)) {
                previewSource_=selected;RefreshHostPreview();
            }
            const auto sourceName=status.capture.selected.kind==CaptureKind::Window ? QString("Sharing a window") : QString("Display %1").arg(status.capture.selected.display+1);
            sourceSummary->setText(sourceName+QString(" · %1\nTarget frame rate: %2").arg(status.stream.preferences.preset==StreamPreset::Gaming?"Gaming":"Quality",
                status.stream.preferences.fpsMode==SettingMode::Auto?QString("Auto"):QString("%1 FPS").arg(status.stream.preferences.fps)));
            controlsTitle->setText(QString("Viewers · %1").arg(status.activePeers));
            uint64_t upload=0; bool sampled=false;
            for(const auto& peer:status.stream.peers) if(peer.transportSendBps){upload+=*peer.transportSendBps;sampled=true;}
            health->setText(status.activePeers ? (sampled ? QString("Transport upload  %1 Mbps").arg(upload/1000000.0,0,'f',2):"Waiting for stream measurements…") : "Waiting for viewers");
            if(status.stream.peers.size()==1) {
                const auto& sender=status.stream.peers.front().sender;
                if(sender.encodedFps)health->setText(health->text()+QString("\nEncoded video  %1 FPS").arg(*sender.encodedFps,0,'f',0));
                if(sender.rttMs)health->setText(health->text()+QString("\nNetwork RTT  %1 ms").arg(*sender.rttMs,0,'f',0));
            }
        };
    } else {
        move(video_,primary,1);
        auto* connection=new QLabel; connection->setObjectName("SessionConnection"); connection->setWordWrap(true); controlsBody->insertWidget(1,connection);
        auto previous=session_.statusChanged;
        session_.statusChanged=[previous,connection](const auto& status){previous(status);connection->setText(Phase(status.phase));};
        auto* videoInfo=new QLabel("Waiting for video…");videoInfo->setObjectName("SessionVideoInfo");controlsBody->insertWidget(2,videoInfo);
        auto present=session_.frameReady;
        session_.frameReady=[present,videoInfo](auto frame){videoInfo->setText(QString("Video  %1 × %2").arg(frame.width).arg(frame.height));if(present)present(std::move(frame));};
    }
    layout->removeWidget(sourceSettings); layout->removeWidget(playbackWidget);
    // Compact source actions and progressive disclosure keep the common stream
    // settings visible; the existing apply/acknowledgement behavior is unchanged.
    auto detachField=[form](QWidget* field) {
        auto row=form->takeRow(field);
        if(row.labelItem){delete row.labelItem->widget();delete row.labelItem;}
        delete row.fieldItem;
    };
    auto inlineActions=[&](const QString& caption,QWidget* selector,QPushButton* refresh,QPushButton* apply) {
        int row=0;QFormLayout::ItemRole role;form->getWidgetPosition(selector,&row,&role);
        detachField(selector);detachField(refresh);detachField(apply);
        auto* widget=new QWidget;auto* line=new QHBoxLayout(widget);line->setContentsMargins(0,0,0,0);line->setSpacing(8);
        line->addWidget(selector,1);line->addWidget(refresh);line->addWidget(apply);
        refresh->setToolTip(refresh->text());refresh->setAccessibleName(refresh->text());refresh->setText({});refresh->setIcon(uiIcon("refresh"));refresh->setFixedWidth(38);
        apply->setText("Share");form->insertRow(row,caption,widget);
    };
    inlineActions("Source",captureSource_,refreshCapture_,switchCapture_);
    inlineActions("Audio device",audioDevice_,refreshAudio_,switchAudio_);
    detachField(audioHealth_);layout->addWidget(audioHealth_);
    auto* advancedStream=new QPushButton("Advanced settings");advancedStream->setObjectName("sessionStreamAdvanced");advancedStream->setCheckable(true);form->insertRow(form->rowCount()-1,advancedStream);
    auto refreshAdvanced=[this,form,advancedStream] {
        const bool advanced=advancedStream->isChecked();
        form->setRowVisible(audioProcess_,audioKind_->currentIndex()==2);
        form->setRowVisible(width_,resolution_->currentIndex()==int(ResolutionMode::Fixed));
        form->setRowVisible(height_,resolution_->currentIndex()==int(ResolutionMode::Fixed));
        form->setRowVisible(fps_,fpsMode_->currentIndex()==int(SettingMode::Manual));
        form->setRowVisible(bitrateLimit_,advanced);
        form->setRowVisible(bitrate_,bitrateMode_->currentIndex()==int(SettingMode::Manual)||bitrateLimit_->isChecked());
        form->setRowVisible(uploadBudgetEnabled_,advanced);
        form->setRowVisible(uploadBudget_,advanced&&uploadBudgetEnabled_->isChecked());
    };
    connect(advancedStream,&QPushButton::toggled,this,refreshAdvanced);
    for(auto* combo:{audioKind_,resolution_,fpsMode_,bitrateMode_})connect(combo,&QComboBox::currentIndexChanged,this,refreshAdvanced);
    connect(bitrateLimit_,&QCheckBox::toggled,this,refreshAdvanced);connect(uploadBudgetEnabled_,&QCheckBox::toggled,this,refreshAdvanced);refreshAdvanced();
    form->setContentsMargins(20,20,20,20);form->setVerticalSpacing(12);
    detachField(apply_);settingsLayout->addWidget(apply_,0,Qt::AlignRight);apply_->setVisible(config.room.host);
    if(config.room.host) { settingsTabs->addTab(sourceSettings,"Stream"); sourceSettings->show(); playbackWidget->hide(); }
    else { settingsTabs->addTab(playbackWidget,"Audio"); playbackWidget->show(); sourceSettings->hide(); }
    layout->removeItem(roomForm);
    auto* roomSettings=new QWidget; roomSettings->setLayout(roomForm);
    if(config.room.host) settingsTabs->addTab(roomSettings,"Room"); else {roomSettings->setParent(settingsPage);roomSettings->hide();}
    // Profile editing belongs to the application's top-bar menu.
    roomForm->setRowVisible(nickname_,false); roomForm->setRowVisible(updateNickname_,false);
    layout->removeWidget(roomUpdateState_); roomForm->addRow(roomUpdateState_);
    layout->removeWidget(settingsState_); form->addRow(settingsState_);
    settingsTabs->addTab(scroll,"Details");
    connect(settingsTabs,&QTabWidget::currentChanged,this,[this,settingsTabs,sourceSettings,host=config.room.host]{apply_->setVisible(host&&settingsTabs->currentWidget()==sourceSettings);});
    auto* detailsButton=new QPushButton("Details");detailsButton->setObjectName("sessionDetails");
    (config.room.host?primary:controlsBody)->addWidget(detailsButton);
    connect(detailsButton,&QPushButton::clicked,this,[pages,settingsPage,settingsTabs,scroll]{settingsTabs->setCurrentWidget(scroll);pages->setCurrentWidget(settingsPage);});
    auto* footer=new QHBoxLayout; footer->setSpacing(12); dashboardLayout->addLayout(footer);
    auto* openSettings=new QPushButton(config.room.host?"Room settings":"Playback settings"); openSettings->setObjectName("openSessionSettings");
    openSettings->setIcon(uiIcon("settings")); footer->addWidget(openSettings);
    connect(openSettings,&QPushButton::clicked,this,[pages,settingsPage,settingsTabs]{settingsTabs->setCurrentIndex(0);pages->setCurrentWidget(settingsPage);});
    if(config.room.host) {copyLink_->setText("Copy invite");copyLink_->setIcon(uiIcon("copy"));move(copyLink_,footer);}
    else {
        auto* mute=new QPushButton("Mute"); mute->setObjectName("sessionMute"); mute->setCheckable(true); mute->setChecked(playbackMuted_->isChecked());
        mute->setIcon(uiIcon("volume")); footer->addWidget(mute);
        auto* volume=new QSlider(Qt::Horizontal); volume->setObjectName("sessionVolume"); volume->setRange(0,100);volume->setValue(playbackVolume_->value());volume->setMaximumWidth(120);footer->addWidget(volume);
        auto* volumeDelay=new QTimer(this); volumeDelay->setSingleShot(true); volumeDelay->setInterval(100);
        connect(volume,&QSlider::valueChanged,this,[this,volumeDelay](int value){playbackVolume_->setValue(value);volumeDelay->start();});
        connect(volumeDelay,&QTimer::timeout,this,[this,volumeDelay]{
            if(session_.playbackPending())volumeDelay->start();
            else if(session_.status().phase==RoomPhase::Active)applyPlayback_->click();
        });
        connect(mute,&QPushButton::toggled,this,[this,volumeDelay](bool checked){playbackMuted_->setChecked(checked);volumeDelay->start(100);});
        connect(playbackMuted_,&QCheckBox::toggled,mute,&QPushButton::setChecked);
        connect(playbackVolume_,&QSpinBox::valueChanged,volume,&QSlider::setValue);
        auto* fullscreen=new QPushButton("Fullscreen"); fullscreen->setObjectName("sessionFullscreen");fullscreen->setIcon(uiIcon("fullscreen"));footer->addWidget(fullscreen);
        connect(fullscreen,&QPushButton::clicked,this,[this,fullscreen]{auto* shell=window();if(shell->isFullScreen()){shell->showNormal();fullscreen->setText("Fullscreen");}else{shell->showFullScreen();fullscreen->setText("Exit fullscreen");}});
        auto* escape=new QShortcut(QKeySequence(Qt::Key_Escape),this);connect(escape,&QShortcut::activated,this,[this,fullscreen]{if(window()->isFullScreen()){window()->showNormal();fullscreen->setText("Fullscreen");}});
        auto* toggle=new QPushButton("Controls"); toggle->setObjectName("toggleSessionControls");toggle->setCheckable(true);toggle->setChecked(true);footer->addWidget(toggle);
        connect(toggle,&QPushButton::toggled,controlsScroll,&QWidget::setVisible);
    }
    footer->addStretch(); stop_->setText(config.room.host?"Stop sharing":"Leave room");stop_->setIcon(uiIcon("stop"));move(stop_,footer);
    move(error_,dashboardLayout); error_->hide();
    for(auto* combo:findChildren<QComboBox*>()) styleComboPopup(combo);
    pages->setCurrentWidget(dashboard);
    session_.error = [this](const auto& message) { error_->setText(message);error_->setVisible(!message.isEmpty()); };
    session_.finished = [this](const auto&) { stop_->setEnabled(false); apply_->setEnabled(false); if (closing_) QTimer::singleShot(0, this, [this] { close(); }); };
    if (!session_.start(std::move(config))) { stop_->setEnabled(false); apply_->setEnabled(false); }
}
void RoomSessionWindow::resizeEvent(QResizeEvent* event) {
    if(sessionColumns_)sessionColumns_->setDirection(event->size().width()<720?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight);
    QWidget::resizeEvent(event);
}
RoomSessionWindow::~RoomSessionWindow() {
    if(previewWorker_) {previewWorker_->requestInterruption();previewWorker_->wait();delete previewWorker_;}
    // QWidget children otherwise outlive the session_ member they reference.
    delete gamepad_; gamepad_ = nullptr;
}
void RoomSessionWindow::RefreshHostPreview() {
    ++previewRevision_;
    hostPreview_->setPixmap(uiIcon("display").pixmap(80,80));
    hostPreview_->setToolTip("Loading source preview…");
    if(previewWorker_) {previewWorker_->requestInterruption();return;}
    const auto source=previewSource_;const auto revision=previewRevision_;
    auto result=std::make_shared<QImage>();
    previewWorker_=QThread::create([source,result] {
        try {
            screenshare::CaptureConfig capture;capture.targetFps=15;capture.allowDisplayFallback=false;
            capture.backend=screenshare::CaptureBackend::WindowsGraphicsCapture;
            capture.sourceType=source.kind==CaptureKind::Window?screenshare::CaptureSourceType::Window:screenshare::CaptureSourceType::Display;
            capture.windowHandle=source.window;capture.displayIndex=source.display;
            screenshare::DesktopCapturer capturer;capturer.Start(capture);
            const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(1);
            while(!QThread::currentThread()->isInterruptionRequested()&&std::chrono::steady_clock::now()<until) {
                auto frame=capturer.TryCaptureFrame(std::chrono::milliseconds(30));
                if(frame&&!frame->pixels.empty()) {*result=QImage(reinterpret_cast<const uchar*>(frame->pixels.data()),frame->width,frame->height,frame->rowPitch,QImage::Format_RGB32).scaled(480,270,Qt::KeepAspectRatio,Qt::SmoothTransformation);break;}
            }
        } catch(...) {}
    });
    connect(previewWorker_,&QThread::finished,this,[this,result,revision] {
        auto* worker=previewWorker_;previewWorker_=nullptr;worker->deleteLater();
        if(revision!=previewRevision_) {if(!closing_)RefreshHostPreview();return;}
        if(!result->isNull())hostPreview_->setPixmap(QPixmap::fromImage(*result));
        hostPreview_->setToolTip(result->isNull()?"Source preview unavailable":"Source preview captured when this source was selected");
    });
    previewWorker_->start();
}
void RoomSessionWindow::revokeControl() { if (gamepad_) gamepad_->Revoke(); }
StreamPreferences RoomSessionWindow::ReadPreferences() const {
    StreamPreferences p; p.preset = StreamPreset(preset_->currentIndex()); p.resolution = ResolutionMode(resolution_->currentIndex());
    p.width = width_->value(); p.height = height_->value(); p.fpsMode = SettingMode(fpsMode_->currentIndex()); p.fps = fps_->value();
    p.bitrateMode = SettingMode(bitrateMode_->currentIndex());
    if (p.bitrateMode == SettingMode::Manual || bitrateLimit_->isChecked()) p.bitrateLimitBps = bitrate_->value();
    if (uploadBudgetEnabled_->isChecked()) p.aggregateUploadLimitBps = uploadBudget_->value();
    return p;
}
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
        const bool previous = QApplication::quitOnLastWindowClosed();
        QApplication::setQuitOnLastWindowClosed(false);
        RoomApplication window(std::move(config));
        window.closed = [] { QApplication::quit(); }; window.show();
        const int result = QApplication::exec();
        QApplication::setQuitOnLastWindowClosed(previous); return result;
    } catch (const std::exception& error) {
        QMessageBox::critical(nullptr, "Cannot start room", QString::fromUtf8(error.what())); return 1;
    }
}
