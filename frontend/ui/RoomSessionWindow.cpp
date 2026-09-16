#include "ui/RoomSessionWindow.h"
#include "shared/RoomLink.h"
#include "shared/RoomProfile.h"
#include "shared/RoomStreamDiagnostics.h"
#include <QTableWidget>
#include <QHeaderView>
#include <QJsonArray>
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
RoomSessionWindow::RoomSessionWindow(RoomSessionConfig config, QtRoomSession::Factory factory, bool loopback, RoomProfile* profile)
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
    audioKind_ = combo("Shared audio", {"System output", "Microphone", "Process output"}, int(config.media.audio.source));
    audioKind_->setObjectName("liveAudioKind");
    audioDevice_ = new QComboBox; audioDevice_->setObjectName("liveAudioDevice"); form->addRow("Audio device", audioDevice_);
    audioDevice_->addItem(config.media.audio.deviceId.empty() ? "Default device" : "Current device", QString::fromStdWString(config.media.audio.deviceId));
    audioProcess_ = number("Audio process ID", 0, INT_MAX, int(config.media.audio.processId)); audioProcess_->setObjectName("liveAudioProcess");
    refreshAudio_ = new QPushButton("Refresh audio devices"); refreshAudio_->setObjectName("refreshAudioDevices"); form->addRow(refreshAudio_);
    switchAudio_ = new QPushButton("Share selected audio"); switchAudio_->setObjectName("switchAudioSource"); switchAudio_->setEnabled(false); form->addRow(switchAudio_);
    audioState_ = new QLabel; audioState_->setWordWrap(true); audioState_->setObjectName("audioState"); form->addRow(audioState_);
    connect(audioKind_, &QComboBox::currentIndexChanged, this, [this] {
        audioDevice_->clear(); audioDevice_->addItem("Default device", QString());
        audioDevice_->setEnabled(audioKind_->currentIndex() != 2); audioProcess_->setEnabled(audioKind_->currentIndex() == 2);
    });
    connect(refreshAudio_, &QPushButton::clicked, this, [this] {
        try {
            const auto selected = audioDevice_->currentData(); audioDevice_->clear(); audioDevice_->addItem("Default device", QString());
            if (audioKind_->currentIndex() != 2) for (const auto& device : screenshare::WasapiCapture::EnumerateDevices(
                audioKind_->currentIndex() == 1 ? screenshare::AudioCaptureSource::Microphone : screenshare::AudioCaptureSource::SystemOutput))
                audioDevice_->addItem(QString::fromStdWString(device.name), QString::fromStdWString(device.id));
            const auto index = audioDevice_->findData(selected); if (index >= 0) audioDevice_->setCurrentIndex(index);
        } catch (...) { audioState_->setText("Could not enumerate audio devices."); }
    });
    connect(switchAudio_, &QPushButton::clicked, this, [this] {
        AudioSelection selection; selection.kind = AudioKind(audioKind_->currentIndex());
        if (selection.kind == AudioKind::Process) selection.processId = uint32_t(audioProcess_->value());
        else selection.deviceId = audioDevice_->currentData().toString().toStdWString();
        audioState_->setText("Waiting for the new audio source…"); switchAudio_->setEnabled(false); session_.switchAudio(std::move(selection));
    });
    session_.audioUpdated = [this](const AudioUpdateResult& result) {
        if (result.error == AudioUpdateError::None) audioState_->setText("Sharing the selected audio source.");
        else if (result.error == AudioUpdateError::Unavailable) audioState_->setText("Audio capture is not active. Connect a viewer first.");
        else if (result.error == AudioUpdateError::Cancelled) audioState_->setText("Audio change cancelled.");
        else audioState_->setText("Could not switch audio. The previous source is retained while available.");
    };
    preset_ = combo("Preset", {"Gaming", "Quality"}, int(p.preset));
    resolution_ = combo("Resolution", {"Auto", "Fixed", "Native"}, int(p.resolution));
    width_ = number("Width", 2, 3840, p.width); width_->setSingleStep(2); width_->setObjectName("streamWidth");
    height_ = number("Height", 2, 2160, p.height); height_->setSingleStep(2); height_->setObjectName("streamHeight");
    fpsMode_ = combo("Frame rate mode", {"Auto", "Manual"}, int(p.fpsMode)); fps_ = number("FPS", 1, 240, p.fps);
    bitrateMode_ = combo("Bitrate mode", {"Auto", "Manual"}, int(p.bitrateMode));
    bitrate_ = number("Bitrate limit (bits/s)", 1000, 100000000, p.bitrateLimitBps.value_or(12000000));
    bitrateLimit_ = new QCheckBox("Also limit Auto bitrate"); bitrateLimit_->setChecked(p.bitrateLimitBps.has_value()); form->addRow(bitrateLimit_);
    uploadBudgetEnabled_ = new QCheckBox("Limit total media upload allowance"); uploadBudgetEnabled_->setObjectName("uploadBudgetEnabled");
    uploadBudgetEnabled_->setChecked(p.aggregateUploadLimitBps.has_value()); form->addRow(uploadBudgetEnabled_);
    uploadBudget_ = number("Upload allowance (bits/s)", 160000, 1000000000, p.aggregateUploadLimitBps.value_or(20000000)); uploadBudget_->setObjectName("uploadBudget");
    apply_ = new QPushButton("Apply settings"); apply_->setObjectName("applyStream"); form->addRow(apply_);
    auto* sourceSettings = new QScrollArea; sourceSettings->setWidgetResizable(true); sourceSettings->setWidget(formWidget);
    sourceSettings->setVisible(config.room.host); sourceSettings->setMinimumHeight(160); layout->addWidget(sourceSettings, 1);
    settingsState_ = new QLabel; settingsState_->setWordWrap(true); layout->addWidget(settingsState_);
    uploadState_ = new QLabel; uploadState_->setWordWrap(true); uploadState_->setObjectName("uploadState"); uploadState_->setVisible(config.room.host); layout->addWidget(uploadState_);
    auto* diagnostics = new QTableWidget(0, 5, this);
    diagnostics->setObjectName("peerDiagnostics");
    diagnostics->setHorizontalHeaderLabels({"Viewer", "Settings", "Source size", "Applied cap", "Transport upload"});
    diagnostics->setEditTriggers(QAbstractItemView::NoEditTriggers);
    diagnostics->setSelectionBehavior(QAbstractItemView::SelectRows);
    diagnostics->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    diagnostics->setMaximumHeight(180); diagnostics->setVisible(config.room.host); layout->addWidget(diagnostics);
    auto* details = new QLabel(this); details->setObjectName("peerDiagnosticsDetails");
    details->setTextFormat(Qt::PlainText); details->setWordWrap(true);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse); details->setVisible(config.room.host); layout->addWidget(details);
    auto refreshDetails = [diagnostics, details] {
        auto* item = diagnostics->item(diagnostics->currentRow(), 0);
        details->setText(item ? item->toolTip() : "Select a viewer for settings details. Source observation does not confirm remote display.");
    };
    connect(diagnostics, &QTableWidget::itemSelectionChanged, this, refreshDetails);
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
    auto* controls = new QLabel("Remote control is not available in this preview."); layout->addWidget(controls);
    stop_ = new QPushButton("Stop"); stop_->setObjectName("stopRoom"); layout->addWidget(stop_);
    connect(stop_, &QPushButton::clicked, this, [this] { session_.stop(); stop_->setEnabled(false); apply_->setEnabled(false); });
    connect(apply_, &QPushButton::clicked, this, [this] {
        error_->clear(); settingsState_->setText("Settings pending…"); session_.apply(ReadPreferences());
    });
    session_.statusChanged = [this, diagnostics, refreshDetails, host = config.room.host](const auto& value) {
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
        const bool playbackEditable = !host && value.phase == RoomPhase::Active && !session_.playbackPending();
        applyPlayback_->setEnabled(playbackEditable); refreshPlayback_->setEnabled(playbackEditable);
        playbackDevice_->setEnabled(playbackEditable); playbackVolume_->setEnabled(playbackEditable); playbackMuted_->setEnabled(playbackEditable);
        switchCapture_->setEnabled(host && value.phase == RoomPhase::Active && !session_.capturePending());
        const bool audioEditable = host && value.phase == RoomPhase::Active && !session_.audioPending();
        switchAudio_->setEnabled(audioEditable && value.activePeers > 0); refreshAudio_->setEnabled(audioEditable && audioKind_->currentIndex() != 2);
        audioKind_->setEnabled(audioEditable); audioDevice_->setEnabled(audioEditable && audioKind_->currentIndex() != 2);
        audioProcess_->setEnabled(audioEditable && audioKind_->currentIndex() == 2);
        refreshCapture_->setEnabled(host && !session_.capturePending()); captureSource_->setEnabled(host && !session_.capturePending());
        if (host) {
            QJsonArray rows;
            for (const auto& peer : value.stream.peers) {
                auto row = StreamPeerJson(peer, value.stream.requestedRevision);
                for (const auto& member : value.members) if (member.peerId == peer.peerId)
                    row["nickname"] = QString::fromStdString(member.nickname);
                rows.append(row);
            }
            // Status/control ticks stay responsive, but unchanged measurements do
            // not rebuild the table. No additional service requests or timers.
            const auto snapshot = QJsonDocument(QJsonObject{{"rows", rows}, {"preferences", StreamPreferencesJson(value.stream.preferences)}}).toJson(QJsonDocument::Compact);
            if (diagnostics->property("snapshot").toByteArray() != snapshot) {
                diagnostics->setProperty("snapshot", snapshot);
                QString selected;
                if (auto* current = diagnostics->item(diagnostics->currentRow(), 0)) selected = current->data(Qt::UserRole).toString();
                const QSignalBlocker blocked(diagnostics);
                diagnostics->setRowCount(int(value.stream.peers.size()));
                diagnostics->setCurrentCell(-1, -1);
                diagnostics->clearSelection();
                for (int index = 0; index < int(value.stream.peers.size()); ++index) {
                    const auto& peer = value.stream.peers[index];
                    const auto id = QString::fromStdString(peer.peerId);
                    const auto nickname = rows[index].toObject()["nickname"].toString();
                    const auto rate = !peer.transportSampleStale && peer.transportSendBps ?
                        QString("%1 Mbps").arg(*peer.transportSendBps / 1000000.0, 0, 'f', 2) : StreamSampleState(peer);
                    const QStringList cells{nickname.isEmpty() ? id : nickname + " [" + id + "]",
                        StreamPeerState(peer, value.stream.requestedRevision),
                        peer.observedRevision ? QString("%1 × %2").arg(peer.width).arg(peer.height) : "unknown",
                        peer.appliedRevision ? QString("%1 Mbps").arg(peer.appliedVideoBitrateBps / 1000000.0, 0, 'f', 2) : "unknown", rate};
                    const auto& preferences = value.stream.preferences;
                    const auto requested = QString("%1; resolution %2 (%3 × %4); FPS %5 (%6); bitrate %7 (%8).")
                        .arg(preferences.preset == StreamPreset::Gaming ? "Gaming" : "Quality")
                        .arg(preferences.resolution == ResolutionMode::Fixed ? "Fixed" : preferences.resolution == ResolutionMode::Native ? "Native" : "Auto")
                        .arg(preferences.width).arg(preferences.height)
                        .arg(preferences.fpsMode == SettingMode::Manual ? "Manual" : "Auto").arg(preferences.fps)
                        .arg(preferences.bitrateMode == SettingMode::Manual ? "Manual target" : "Auto ceiling")
                        .arg(preferences.bitrateLimitBps ? QString::number(*preferences.bitrateLimitBps) + " bps" : "automatic allowance");
                    const auto detail = QString("Peer %1\nRequested / applied / source-observed revisions: %2 / %3 / %4\nAllocated video cap: %5 bps; applied video cap: %6 bps. Transport sample: %7 (expires after 3 seconds).\nTransport includes audio and protocol traffic, excludes IP/interface overhead. Remote display, latency and congestion reason: unknown.\nRequested settings: %8")
                        .arg(id).arg(value.stream.requestedRevision).arg(peer.appliedRevision).arg(peer.observedRevision)
                        .arg(peer.allocatedVideoBitrateBps).arg(peer.appliedVideoBitrateBps).arg(StreamSampleState(peer))
                        .arg(requested);
                    for (int column = 0; column < cells.size(); ++column) {
                        auto* item = diagnostics->item(index, column);
                        if (!item) { item = new QTableWidgetItem; diagnostics->setItem(index, column, item); }
                        item->setText(cells[column]); item->setToolTip(detail); item->setData(Qt::UserRole, id);
                    }
                    if (id == selected) { diagnostics->setCurrentCell(index, 0); diagnostics->selectRow(index); }
                }
                refreshDetails();
            }
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
            size_t complete = 0, rejected = 0;
            for (const auto& peer : value.stream.peers) {
                complete += peer.appliedRevision == value.stream.requestedRevision &&
                    (peer.observedRevision == value.stream.requestedRevision || peer.appliedVideoBitrateBps == 0);
                rejected += peer.rejected;
            }
            settingsState_->setText(value.stream.peers.empty() ? QString("Settings saved. Waiting for viewers.") :
                QString("Stream settings: %1 applied, %2 pending, %3 rejected.")
                .arg(complete).arg(value.stream.peers.size() - complete - rejected).arg(rejected));
        }
        if (value.phase == RoomPhase::Failed) error_->setText("The room session failed. Stop and start a new session to retry.");
    };
    session_.settingsAccepted = [this](const auto& result) { if (result.error != StreamUpdateError::None) error_->setText("The settings update was rejected."); };
    session_.captureUpdated = [this](const auto& result) {
        if (result.error == CaptureUpdateError::None) captureState_->setText("Sharing the new source. Waiting for viewers to display it.");
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
        auto* presentationStatus = new QTimer(this);
        presentationStatus->setInterval(250);
        connect(presentationStatus, &QTimer::timeout, this, [this, presentationStatus] {
            if (!video_->presentationStats().terminal) return;
            error_->setText("Video presentation failed. Leave and rejoin the room to retry. Audio and room controls remain available.");
            presentationStatus->stop();
        });
        presentationStatus->start();
    }
    session_.error = [this](const auto& message) { error_->setText(message); };
    session_.finished = [this](const auto&) { stop_->setEnabled(false); apply_->setEnabled(false); if (closing_) QTimer::singleShot(0, this, [this] { close(); }); };
    if (!session_.start(std::move(config))) { stop_->setEnabled(false); apply_->setEnabled(false); }
}
RoomSessionWindow::~RoomSessionWindow() = default;
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
        RoomSessionWindow window(std::move(config)); window.show();
        return QApplication::exec();
    } catch (const std::exception& error) {
        QMessageBox::critical(nullptr, "Cannot start room", QString::fromUtf8(error.what())); return 1;
    }
}
