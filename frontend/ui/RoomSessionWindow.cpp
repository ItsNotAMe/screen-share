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
#include <climits>
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
RoomSessionWindow::RoomSessionWindow(RoomSessionConfig config, QtRoomSession::Factory factory, bool loopback, RoomProfile* profile,
    RoomGamepadControl::Devices devices, RoomGamepadControl::Read read, FramePresentationFactory presentation)
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
    auto* controls = new QLabel("Input requires explicit consent. Window sharing permits mouse control only; source changes revoke control."); layout->addWidget(controls);
    stop_ = new QPushButton("Stop"); stop_->setObjectName("stopRoom"); layout->addWidget(stop_);
    connect(stop_, &QPushButton::clicked, this, [this] { session_.stop(); stop_->setEnabled(false); apply_->setEnabled(false); });
    connect(apply_, &QPushButton::clicked, this, [this] {
        error_->clear(); settingsState_->setText("Settings pending…"); session_.apply(ReadPreferences());
    });
    session_.statusChanged = [this, peerDiagnostics, capacityWarning, host = config.room.host](const auto& value) {
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
    session_.error = [this](const auto& message) { error_->setText(message); };
    session_.finished = [this](const auto&) { stop_->setEnabled(false); apply_->setEnabled(false); if (closing_) QTimer::singleShot(0, this, [this] { close(); }); };
    if (!session_.start(std::move(config))) { stop_->setEnabled(false); apply_->setEnabled(false); }
}
RoomSessionWindow::~RoomSessionWindow() {
    // QWidget children otherwise outlive the session_ member they reference.
    delete gamepad_; gamepad_ = nullptr;
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
