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
#include "ui/SourcePickerDialog.h"
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
#include <QMouseEvent>
#include <QElapsedTimer>
#include <QProcess>
#include <QDir>
#include <QFileInfo>
#include <QScrollBar>
#include <QTabBar>
#include <QVariantAnimation>
#include <QKeyEvent>
#include "ui/AppShellWindow.h"
#include <thread>
#include <climits>
using namespace screenshare::v2;
using namespace screenshare::media;
namespace {
class SettingsBackdrop final : public QWidget {
public:
    SettingsBackdrop(QWidget* parent):QWidget(parent) {
        qApp->installEventFilter(this);
        animation_.setDuration(180);animation_.setEasingCurve(QEasingCurve::OutCubic);
        connect(&animation_,&QVariantAnimation::valueChanged,this,[this](const QVariant& value){progress_=value.toReal();positionPanel();});
        connect(&animation_,&QVariantAnimation::finished,this,[this]{if(!opening_){hide();if(didClose)didClose();}});
    }
    ~SettingsBackdrop() override {qApp->removeEventFilter(this);}
    QWidget* panel=nullptr;
    std::function<void()> dismiss,didClose;
    std::function<void(int)> panelMoved;
    void slide(bool open){
        animation_.stop();opening_=open;
        if(open){show();raise();}
        animation_.setStartValue(progress_);animation_.setEndValue(open?1.0:0.0);animation_.start();
    }
protected:
    bool eventFilter(QObject* watched,QEvent* event) override {
        auto* target=qobject_cast<QWidget*>(watched);
        if(isVisible() && target && parentWidget()->isAncestorOf(target) && !isAncestorOf(target) && target!=this) {
            switch(event->type()) {
            case QEvent::MouseButtonPress:
                if(static_cast<QMouseEvent*>(event)->button()==Qt::LeftButton && dismiss)dismiss();
                return true;
            case QEvent::MouseButtonRelease:case QEvent::MouseButtonDblClick:
            case QEvent::MouseMove:case QEvent::Wheel:case QEvent::KeyPress:case QEvent::KeyRelease:
                return true;
            default:break;
            }
        }
        return QWidget::eventFilter(watched,event);
    }
    void resizeEvent(QResizeEvent* event) override{QWidget::resizeEvent(event);positionPanel();}
    void mousePressEvent(QMouseEvent* event) override {
        if(event->button()==Qt::LeftButton && panel && !panel->geometry().contains(event->position().toPoint())) {
            if(dismiss)dismiss();event->accept();return;
        }
        QWidget::mousePressEvent(event);
    }
private:
    void positionPanel(){if(panel){const int w=qMin(520,width());panel->setGeometry(width()-qRound(w*progress_),0,w,height());if(panelMoved)panelMoved(panel->x());}}
    QVariantAnimation animation_;
    qreal progress_=0;
    bool opening_=false;
};
class AdvancedDisclosure final : public QPushButton {
public:
    AdvancedDisclosure() {
        setCheckable(true);setAccessibleName("Advanced settings");
        auto* row=new QHBoxLayout(this);row->setContentsMargins(6,6,6,6);row->setSpacing(8);
        auto* gear=new QLabel;gear->setPixmap(uiIcon("settings").pixmap(18,18));
        auto* label=new QLabel("Advanced settings");auto* arrow=new QLabel;arrow->setObjectName("AdvancedArrow");
        arrow->setPixmap(uiIcon("chevron-down").pixmap(14,14));
        for(auto* part:{gear,label,arrow}){part->setAttribute(Qt::WA_TransparentForMouseEvents);row->addWidget(part);}
        connect(this,&QPushButton::toggled,this,[arrow](bool open){arrow->setPixmap(uiIcon(open?"chevron-up":"chevron-down").pixmap(14,14));});
        setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);
    }
    QSize sizeHint() const override{return (layout()->sizeHint()+QSize(12,8)).expandedTo(QSize(0,36));}
    QSize minimumSizeHint() const override{return sizeHint();}
};
class OutputDeviceCombo final : public QComboBox {
public:
    std::function<void()> refresh;
    void showPopup() override {if(refresh)refresh();QComboBox::showPopup();}
};
class SettingsTabs final : public QTabWidget {
protected:
    void resizeEvent(QResizeEvent* event) override {QTabWidget::resizeEvent(event);tabBar()->setFixedWidth(width());}
};
class SessionVolumeSlider final : public QSlider {
public:
    SessionVolumeSlider() : QSlider(Qt::Horizontal) {setFixedHeight(36);setAccessibleName("Playback volume");}
protected:
    void mousePressEvent(QMouseEvent* event) override {
        if(event->button()!=Qt::LeftButton){QSlider::mousePressEvent(event);return;}
        setFocus(Qt::MouseFocusReason);setSliderDown(true);Move(event);event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {if(isSliderDown()){Move(event);event->accept();}else QSlider::mouseMoveEvent(event);}
    void mouseReleaseEvent(QMouseEvent* event) override {
        if(event->button()==Qt::LeftButton&&isSliderDown()){Move(event);setSliderDown(false);event->accept();}else QSlider::mouseReleaseEvent(event);
    }
private:
    void Move(QMouseEvent* event){setValue(QStyle::sliderValueFromPosition(minimum(),maximum(),qBound(0,int(event->position().x())-7,width()-14),qMax(1,width()-14)));}
};
class SourcePreviewLabel final : public QLabel {
public:
    using QLabel::QLabel;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);painter.fillRect(rect(),QColor("#080e0b"));
        if(pixmap().isNull()) {painter.setPen(QColor("#a3b5af"));painter.drawText(rect().adjusted(20,20,-20,-20),Qt::AlignCenter|Qt::TextWordWrap,text());return;}
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
    // Diagnostics do not own the native video surface. Avoid promoting hidden
    // forms into native windows while constructing the embedded session page.
    auto* content = new QWidget;
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
    auto* showReport=new QPushButton("Show in Explorer");showReport->setObjectName("showRoomReport");showReport->setEnabled(false);layout->addWidget(showReport);
    connect(showReport,&QPushButton::clicked,this,[showReport,reportResult]{
        const auto path=showReport->property("reportPath").toString();
        if(!QFileInfo::exists(path)){reportResult->setText("The saved report is no longer at: "+path);showReport->setEnabled(false);return;}
        if(!QProcess::startDetached("explorer.exe",{QStringLiteral("/select,"),QDir::toNativeSeparators(path)}))reportResult->setText("Could not open Explorer. Report saved at: "+path);
    });
    connect(exportReport, &QPushButton::clicked, this, [this, reportResult, showReport, configured = config.reportFile] {
        const auto file = configured.isEmpty() ? "room-v2-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json" : configured;
        const auto path = ResolveUiReportPath(file);
        const auto input = session_.input();
        auto report = RoomDiagnosticReport(session_.status(), input ? input->Read() : std::vector<screenshare::input::Status>{});
        report["decodedFrameHandoff"] = FrameQueueDiagnostics(session_.frameStatistics());
        const bool saved=WriteRoomDiagnosticReport(path, report);
        reportResult->setText(saved ? "Saved diagnostic report: " + QDir::toNativeSeparators(path) : "Could not save diagnostic report. Check the destination is writable.");
        showReport->setProperty("reportPath",saved?path:QString());showReport->setEnabled(saved);
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
    auto edited = [this] { editingRoom_ = true; ++roomEditSequence_; };
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
        autoRoomUpdate_=false;updatingNickname_ = false; lockRoomFields();
        roomUpdateState_->setText("Updating room…");
        session_.updatePolicy({name_->text().toStdString(), publicRoom_->isChecked(), viewerLimit_->value()}, editRevision_);
    });
    video_ = new VideoFrameWidget(this,std::move(presentation)); video_->setMinimumSize(320, 180); video_->setVisible(!config.room.host && config.preview);
    video_->setObjectName("roomVideo");
    video_->setLowLatency(true);
    layout->addWidget(video_, 1);
    auto* playbackWidget = new QWidget(this); auto* playbackForm = new QFormLayout(playbackWidget);
    playbackForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    playbackWidget->setVisible(!config.room.host); layout->addWidget(playbackWidget);
    playbackDevice_ = new OutputDeviceCombo; playbackDevice_->setObjectName("playbackDevice");
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
            const auto devices = screenshare::WasapiCapture::EnumerateDevices(screenshare::AudioCaptureSource::SystemOutput);
            const QSignalBlocker blocker(playbackDevice_);
            const auto selected = playbackDevice_->currentData(); const auto label=playbackDevice_->currentText(); playbackDevice_->clear(); playbackDevice_->addItem("Default output", QString());
            for (const auto& device : devices)
                playbackDevice_->addItem(QString::fromStdWString(device.name), QString::fromStdWString(device.id));
            auto index = playbackDevice_->findData(selected); if(index<0){playbackDevice_->addItem(label,selected);index=playbackDevice_->count()-1;} playbackDevice_->setCurrentIndex(index);
        } catch (...) { playbackState_->setText("Could not enumerate output devices."); }
    });
    static_cast<OutputDeviceCombo*>(playbackDevice_)->refresh=[this]{refreshPlayback_->click();};
    playbackForm->setRowVisible(refreshPlayback_,false);
    playbackForm->setRowVisible(applyPlayback_,false);
    connect(applyPlayback_, &QPushButton::clicked, this, [this] {
        applyPlayback_->setEnabled(false); playbackState_->setText("Applying playback settings…");
        session_.updatePlayback({playbackDevice_->currentData().toString().toStdWString(), unsigned(playbackVolume_->value()), playbackMuted_->isChecked()});
    });
    session_.playbackUpdated = [this,profile](const auto& result) {
        if (result.error == AudioUpdateError::None) {playbackState_->clear();if(profile)profile->savePlayback({playbackVolume_->value(),playbackMuted_->isChecked()});}
        else if (result.error == AudioUpdateError::Cancelled) playbackState_->setText("Playback change cancelled.");
        else if (result.error == AudioUpdateError::Unavailable) playbackState_->setText("Playback is not active yet.");
        else playbackState_->setText("Could not change playback. Previous settings retained while available.");
    };
    auto* formWidget = new QWidget(this); auto* form = new QFormLayout(formWidget); formWidget->setVisible(config.room.host);
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
    auto* sourceSettings = new QScrollArea(this); sourceSettings->setWidgetResizable(true); sourceSettings->setWidget(formWidget);
    sourceSettings->setVisible(config.room.host); sourceSettings->setMinimumHeight(160); layout->addWidget(sourceSettings, 1);
    settingsState_ = new QLabel; settingsState_->setObjectName("streamSettingsState"); settingsState_->setWordWrap(true); layout->addWidget(settingsState_);
    uploadState_ = new QLabel; uploadState_->setWordWrap(true); uploadState_->setObjectName("uploadState"); uploadState_->setVisible(config.room.host); layout->addWidget(uploadState_);
    auto* peerDiagnostics = new PeerDiagnosticsWidget(this);
    peerDiagnostics->setVisible(config.room.host); layout->addWidget(peerDiagnostics);
    gamepad_ = new RoomGamepadControl(config.room.host, [this] { return session_.input(); },
        [this] { return session_.status(); }, this, std::move(devices), std::move(read));
    layout->addWidget(gamepad_);
    gamepad_->SetVideo(video_);
    gamepad_->prepareGrant=[this](uint8_t caps){return session_.prepareInputGrant(caps);};
    stop_ = new QPushButton("Stop"); stop_->setObjectName("stopRoom"); layout->addWidget(stop_);
    connect(stop_, &QPushButton::clicked, this, [this] { close(); stop_->setEnabled(false); apply_->setEnabled(false); });
    connect(apply_, &QPushButton::clicked, this, [this] {
        error_->clear();streamSaveError_.clear(); settingsState_->setText("Settings pending…"); session_.apply(ReadPreferences());
    });
    session_.statusChanged = [this, peerDiagnostics, capacityWarning, host = config.room.host](const auto& value) {
        phase_->setText(Phase(value.phase));
        phase_->setToolTip(QString("%1 connected, %2 pending, %3 failed").arg(value.activePeers).arg(value.pendingPeers).arg(value.failedPeers));
        room_->setText((host ? "Hosting: " : "") + QString::fromStdString(value.policy.name.empty()?value.roomId:value.policy.name));
        roomLink_->setText(MakeRoomLink(QString::fromStdString(value.roomId)));
        copyLink_->setEnabled(value.phase == RoomPhase::Active && !roomLink_->text().isEmpty());
        const bool editable = value.phase == RoomPhase::Active && !session_.roomUpdatePending();
        updateNickname_->setEnabled(editable); updatePolicy_->setEnabled(host && editable);
        nickname_->setEnabled(editable); name_->setEnabled(host && (editable||autoRoomUpdate_));
        publicRoom_->setEnabled(host && (editable||autoRoomUpdate_)); viewerLimit_->setEnabled(host && (editable||autoRoomUpdate_));
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
        applyPlayback_->setVisible(playbackFailed);
        applyPlayback_->setEnabled(playbackEditable); refreshPlayback_->setEnabled(playbackEditable);
        playbackDevice_->setEnabled(playbackEditable); playbackVolume_->setEnabled(playbackEditable); playbackMuted_->setEnabled(playbackEditable);
        switchCapture_->setEnabled(host && value.phase == RoomPhase::Active && !session_.capturePending());
        const bool audioEditable = host && value.phase == RoomPhase::Active && !session_.audioPending();
        const bool audioFailed = value.audio.health.state == AudioEndpointState::Failed;
        audioHealth_->setText(audioFailed ? "Audio capture failed. Video continues without shared audio. Retry or select another source." :
            value.audio.health.state == AudioEndpointState::Running ? (value.audio.microphoneProcessing ?
                "Microphone active: noise suppression and digital gain. Echo cancellation is unavailable." : "Audio capture active; speech processing bypassed.") :
            value.audio.health.state == AudioEndpointState::Silent ? "Audio capture disabled." : "Audio capture inactive.");
        switchAudio_->setText(audioFailed ? "Retry" : "Share");
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
            uploadState_->setText(QString("Video caps: %1 Mbps allocated, %2 Mbps applied; %3 viewer(s) paused. ")
                .arg(allocated / 1000000.0, 0, 'f', 2).arg(applied / 1000000.0, 0, 'f', 2).arg(paused) + measuredText);
        }
        if(host&&!streamSaveError_.isEmpty())settingsState_->setText(streamSaveError_);
        else if (host && session_.settingsPending()) settingsState_->setText("Settings pending…");
        else if (host && value.stream.requestedRevision) {
            const auto application = StreamApplicationJson(value.stream);
            settingsState_->setText(application["rejected"].toInt()?"Some viewers could not apply these settings. Review Details, then change the setting to retry.":application["pending"].toInt()?"Applying settings…":QString());
        }
        if (value.phase == RoomPhase::Failed) error_->setText("The room session failed. Stop and start a new session to retry.");
        error_->setVisible(!error_->text().isEmpty());
    };
    session_.settingsAccepted = [this](const auto& result) { if (result.error != StreamUpdateError::None) {streamSaveError_="These settings could not be applied. Change an option to retry.";settingsState_->setText(streamSaveError_);} };
    session_.captureUpdated = [this](const auto& result) {
        if (result.error == CaptureUpdateError::None) captureState_->setText("Sharing the new source. Receiver reports appear in viewer details.");
        else if (result.error == CaptureUpdateError::Cancelled) captureState_->setText("Source change cancelled.");
        else captureState_->setText("Could not change source. The previous source remains selected if it is still available.");
    };
    session_.roomUpdated = [this](const auto& result) {
        switch (result.error) {
        case RoomUpdateError::None:
            if (updatingNickname_) {editingNickname_ = false;nicknameRevision_=result.currentRevision;} else editingRoom_ = autoRoomUpdate_&&roomSubmittedSequence_!=roomEditSequence_;
            roomUpdateState_->clear(); break;
        case RoomUpdateError::Conflict: roomUpdateState_->setText("The room changed while you were editing. Your changes were not applied; edit again to retry."); break;
        case RoomUpdateError::Unconfirmed: roomUpdateState_->setText("The server outcome is unknown. Check current room values before trying again."); break;
        case RoomUpdateError::Invalid: roomUpdateState_->setText("Invalid name or room settings."); break;
        case RoomUpdateError::Forbidden: roomUpdateState_->setText("Only the host can change room settings."); break;
        default: roomUpdateState_->setText("The room update was not accepted."); break;
        }
        autoRoomUpdate_=false;
        if(applyingProfileNickname_) {
            applyingProfileNickname_=false;
            if(profileNicknameResult)profileNicknameResult(result.error==RoomUpdateError::None?QString():"Saved for future rooms, but this room's nickname was not confirmed. "+roomUpdateState_->text());
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
    auto* settingsPage = new SettingsBackdrop(dashboard); settingsDrawer_=settingsPage;settingsPage->setObjectName("SessionSettingsOverlay");settingsPage->setAttribute(Qt::WA_StyledBackground);

    auto* drawer=new QWidget(settingsPage);drawer->setObjectName("SessionSettings");drawer->setAttribute(Qt::WA_StyledBackground);settingsPage->panel=drawer;
    auto* settingsLayout = new QVBoxLayout(drawer); settingsLayout->setContentsMargins(20,20,20,20);settingsLayout->setSpacing(16);
    auto* settingsHeading=new QHBoxLayout;
    auto* settingsTitle = new QLabel(config.room.host ? "Room settings" : "Playback settings"); settingsTitle->setObjectName("SectionHeading");settingsHeading->addWidget(settingsTitle,1);
    auto* backToSession = new QPushButton; backToSession->setObjectName("sessionSettingsBack");backToSession->setAccessibleName("Close settings");backToSession->setToolTip("Close settings");backToSession->setFixedSize(36,36);settingsHeading->addWidget(backToSession);settingsLayout->addLayout(settingsHeading);
    auto* settingsTabs = new SettingsTabs; settingsTabs->setObjectName("SessionSettingsTabs"); settingsLayout->addWidget(settingsTabs,1);
    settingsPage->hide();backToSession->setIcon(uiIcon("window-close"));
    if(!config.room.host) {
        settingsPage->panelMoved=[this,settingsPage,drawer](int){
            if(settingsPage->isVisible())video_->setOverlayExclusion(QRect(video_->mapFromGlobal(drawer->mapToGlobal(QPoint())),drawer->size()));
        };
        settingsPage->didClose=[this]{video_->setOverlayExclusion({});};
    }
    auto hideSettings=[settingsPage]{settingsPage->slide(false);};
    auto showSettings=[this,settingsPage,dashboard,backToSession,drawer]{
        settingsPage->setGeometry(dashboard->rect());
        settingsPage->slide(true);backToSession->setFocus();
    };
    settingsPage->dismiss=hideSettings;
    connect(backToSession,&QPushButton::clicked,this,hideSettings);
    if(config.room.host){auto* escape=new QShortcut(QKeySequence(Qt::Key_Escape),this);connect(escape,&QShortcut::activated,this,hideSettings);}
    auto move = [layout](QWidget* widget,QBoxLayout* destination,int stretch=0) { layout->removeWidget(widget); destination->addWidget(widget,stretch); };
    auto* sessionHeader = new QHBoxLayout;
    room_->setObjectName("SessionTitle"); room_->setWordWrap(true); move(room_,sessionHeader,1); move(phase_,sessionHeader);
    auto* elapsed=new QLabel("Elapsed  00:00");elapsed->setObjectName("SessionElapsed");elapsed->setToolTip("Session duration");sessionHeader->addWidget(elapsed);
    auto* viewerIcon=new QLabel;viewerIcon->setPixmap(uiIcon("viewers").pixmap(20,20));sessionHeader->addWidget(viewerIcon);
    auto* viewerCount=new QLabel("0 viewers");viewerCount->setObjectName("SessionViewerCount");sessionHeader->addWidget(viewerCount);
    auto previousHeader=session_.statusChanged;
    session_.statusChanged=[previousHeader,viewerCount](const auto& status){previousHeader(status);const auto count=std::count_if(status.members.begin(),status.members.end(),[](const auto& member){return !member.host;});viewerCount->setText(QString("%1 %2").arg(count).arg(count==1?"viewer":"viewers"));};
    auto elapsedClock=std::make_shared<QElapsedTimer>();elapsedClock->start();auto* elapsedTimer=new QTimer(this);elapsedTimer->setInterval(1000);
    connect(elapsedTimer,&QTimer::timeout,this,[elapsed,elapsedClock]{const auto seconds=elapsedClock->elapsed()/1000;elapsed->setText(QString("Elapsed  %1:%2").arg(seconds/60,2,10,QChar('0')).arg(seconds%60,2,10,QChar('0')));});elapsedTimer->start();
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
    auto* detailsButton=new QPushButton("Details");detailsButton->setObjectName("sessionDetails");detailsButton->setIcon(uiIcon("chevron-down"));detailsButton->setLayoutDirection(Qt::RightToLeft);
    for(const auto* name:{"showInputDiagnostics","inputDiagnostics"}) if(auto* widget=gamepad_->findChild<QWidget*>(name)) {
        gamepad_->layout()->removeWidget(widget);layout->addWidget(widget);
    }
    if(config.room.host) {
        auto* sourceTitle=new QLabel("You’re sharing"); sourceTitle->setObjectName("SectionHeading"); primary->addWidget(sourceTitle);
        hostPreview_=new SourcePreviewLabel;hostPreview_->setObjectName("HostSourcePreview");hostPreview_->setAlignment(Qt::AlignCenter);
        hostPreview_->setMinimumHeight(80);hostPreview_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Expanding);
        hostPreview_->setPixmap(uiIcon("display").pixmap(80,80));primary->addWidget(hostPreview_,1);
        auto* previewActivity=new QTimer(this);previewActivity->setInterval(100);
        connect(previewActivity,&QTimer::timeout,this,[this,loopback] {
            const bool active=!loopback&&!closing_&&hostPreview_->isVisible()&&window()->isActiveWindow()&&!window()->isMinimized()&&!session_.status().stream.preferences.videoPaused;
            if(active!=previewActive_){previewActive_=active;RefreshHostPreview();}
        });previewActivity->start();
        auto* sourceSummary=new QLabel; sourceSummary->setObjectName("SessionSourceSummary"); sourceSummary->setWordWrap(true); primary->addWidget(sourceSummary);
        auto* sourceActions=new QHBoxLayout;primary->addLayout(sourceActions);
        auto* changeSource=new QPushButton("Change source"); changeSource->setObjectName("sessionChangeSource"); changeSource->setIcon(uiIcon("display")); sourceActions->addWidget(changeSource);
        connect(changeSource,&QPushButton::clicked,this,[this,loopback]{
            if(auto* existing=findChild<QDialog*>("SourcePickerDialog")){existing->raise();return;}
            auto* picker=new SourcePickerDialog(session_.status().capture.selected,!loopback,this);picker->setAttribute(Qt::WA_DeleteOnClose);
            picker->body->insertWidget(picker->body->count()-1,sharedAudioSettings_);sharedAudioSettings_->show();
            connect(picker,&QDialog::finished,this,[this]{sharedAudioSettings_->setParent(this);sharedAudioSettings_->hide();});
            picker->chosen=[this](CaptureSelection selection){captureState_->setText("Waiting for the new source…");session_.switchCapture(selection);};
            picker->open();
        });
        auto* pauseVideo=new QPushButton("Pause video");pauseVideo->setObjectName("pauseSharedVideo");pauseVideo->setIcon(uiIcon("pause"));sourceActions->addWidget(pauseVideo);
        connect(pauseVideo,&QPushButton::clicked,this,[this] {
            auto preferences=session_.status().stream.preferences;
            preferences.videoPaused=!preferences.videoPaused;
            session_.apply(preferences);
        });
        auto* muteAudio=new QPushButton("Mute audio");muteAudio->setObjectName("muteSharedAudio");muteAudio->setIcon(uiIcon("volume"));sourceActions->addWidget(muteAudio);
        for(auto* button:{changeSource,pauseVideo,muteAudio}){button->setMinimumHeight(42);button->setIconSize(QSize(22,22));}
        auto previousAudio=std::make_shared<AudioSelection>();
        connect(muteAudio,&QPushButton::clicked,this,[this,previousAudio] {
            const auto current=session_.status().audio.selected;
            if(current.kind==AudioKind::None)session_.switchAudio(*previousAudio);
            else {*previousAudio=current;AudioSelection muted;muted.kind=AudioKind::None;session_.switchAudio(muted);}
        });
        auto* healthRow=new QHBoxLayout;healthRow->setSpacing(8);primary->addLayout(healthRow);
        auto metric=[healthRow](const QString& title){auto* column=new QVBoxLayout;auto* caption=new QLabel(title);caption->setObjectName("MetricCaption");column->addWidget(caption);auto* value=new QLabel("—");value->setObjectName("MetricValue");column->addWidget(value);healthRow->addLayout(column,1);return value;};
        auto* health=metric("Stream health");health->setObjectName("SessionHealth");
        auto* bitrate=metric("Actual bitrate");auto* rtt=metric("Network RTT");auto* sentFps=metric("Encoded FPS");healthRow->addWidget(detailsButton);
        auto previous=session_.statusChanged;
        session_.statusChanged=[this,previous,sourceTitle,sourceSummary,health,bitrate,rtt,sentFps,controlsTitle,muteAudio,pauseVideo,loopback](const auto& status) {
            previous(status);
            const bool pausePending=session_.settingsPending()||std::any_of(status.stream.peers.begin(),status.stream.peers.end(),[&](const auto& peer){return !peer.rejected&&peer.appliedRevision<status.stream.requestedRevision;});
            pauseVideo->setEnabled(status.phase==RoomPhase::Active&&!pausePending);
            pauseVideo->setText(pausePending?"Applying…":status.stream.preferences.videoPaused?"Resume video":"Pause video");
            pauseVideo->setIcon(uiIcon(status.stream.preferences.videoPaused?"play":"pause"));
            muteAudio->setEnabled(status.phase==RoomPhase::Active && status.activePeers>0 && !session_.audioPending());
            muteAudio->setText(status.audio.selected.kind==AudioKind::None?"Share audio":"Mute audio");
            const auto& selected=status.capture.selected;
            if(!loopback && (!previewRevision_ || selected.kind!=previewSource_.kind || selected.display!=previewSource_.display || selected.window!=previewSource_.window)) {
                previewSource_=selected;previewSourceSize_={};RefreshHostPreview();
            }
            const auto sourceName=status.capture.selected.kind==CaptureKind::Window ? QString("Sharing a window") : QString("Display %1").arg(status.capture.selected.display+1);
            sourceTitle->setText("You’re sharing "+sourceName);
            sourceSummary->setText(QString("%1  ·  %2").arg(status.stream.preferences.preset==StreamPreset::Gaming?"Gaming":"Quality",
                status.stream.preferences.fpsMode==SettingMode::Auto?QString("Auto"):QString("%1 FPS").arg(status.stream.preferences.fps)));
            QSize resolution=previewSourceSize_;bool streamResolution=false;
            if(!status.stream.peers.empty()&&status.stream.peers.front().width>0){resolution=QSize(status.stream.peers.front().width,status.stream.peers.front().height);streamResolution=true;}
            sourceSummary->setText((resolution.isValid()?QString("%1 × %2  ·  ").arg(resolution.width()).arg(resolution.height()):QString("Resolution pending  ·  "))+sourceSummary->text());
            sourceSummary->setToolTip(streamResolution?"Observed stream resolution for the first viewer; per-viewer measurements are in Details.":"Captured source resolution; stream resolution appears when a viewer connects.");
            controlsTitle->setText(QString("Viewers · %1").arg(status.activePeers));
            uint64_t upload=0; bool sampled=false;
            for(const auto& peer:status.stream.peers) if(peer.transportSendBps){upload+=*peer.transportSendBps;sampled=true;}
            health->setText(status.stream.preferences.videoPaused?"Paused":!status.activePeers?"Waiting":status.failedPeers?"Degraded":"Connected");
            bitrate->setText(sampled?QString("%1 Mbps").arg(upload/1000000.0,0,'f',1):"—");bitrate->setToolTip("Total measured transport upload across viewers");rtt->setText("—");sentFps->setText("—");
            if(status.stream.peers.size()==1) {
                const auto& sender=status.stream.peers.front().sender;
                if(sender.encodedFps)sentFps->setText(QString("%1 FPS").arg(*sender.encodedFps,0,'f',0));
                if(sender.rttMs)rtt->setText(QString("%1 ms").arg(*sender.rttMs,0,'f',0));
            }
        };
    } else {
        primary->setContentsMargins(0,0,0,0);
        move(video_,primary,1);video_->setCornerRadius(8);
        auto* connectionTitle=new QLabel("Connection");connectionTitle->setObjectName("SectionHeading");controlsBody->addWidget(connectionTitle);
        auto* metrics=new QFormLayout;metrics->setContentsMargins(0,8,0,8);metrics->setVerticalSpacing(12);
        auto metric=[metrics](const char* caption,const char* name){
            auto* line=new QFrame;line->setObjectName("SettingsDivider");line->setFixedHeight(1);metrics->addRow(line);
            auto* value=new QLabel(QString(QChar(0x2014)));value->setObjectName(name);value->setAlignment(Qt::AlignRight|Qt::AlignVCenter);metrics->addRow(caption,value);return value;
        };
        auto* rtt=metric("RTT","SessionRtt");rtt->setToolTip("Network round-trip time");
        auto* videoInfo=metric("Resolution","SessionVideoInfo");
        auto* fpsInfo=metric("FPS","SessionFps");
        auto* bitrate=metric("Bitrate","SessionBitrate");bitrate->setToolTip("Received transport traffic, including audio and video");
        controlsBody->addLayout(metrics);
        auto* connection=new QLabel(this);connection->setObjectName("SessionConnection");connection->hide();
        auto previous=session_.statusChanged;
        session_.statusChanged=[previous,connection,rtt,bitrate](const auto& status){
            previous(status);connection->setText(Phase(status.phase));
            rtt->setText(status.stream.receiveRttMs?QString("%1 ms").arg(*status.stream.receiveRttMs,0,'f',0):QString(QChar(0x2014)));
            bitrate->setText(status.stream.receiveBps?QString("%1 Mbps").arg(*status.stream.receiveBps/1000000.0,0,'f',1):QString(QChar(0x2014)));
        };
        controlsBody->addWidget(detailsButton);
        auto decoded=std::make_shared<unsigned>(0);auto* fpsTimer=new QTimer(this);fpsTimer->setInterval(1000);
        auto sampleClock=std::make_shared<QElapsedTimer>();sampleClock->start();
        connect(fpsTimer,&QTimer::timeout,this,[decoded,fpsInfo,sampleClock]{const auto ms=sampleClock->restart();fpsInfo->setText(QString::number(ms?1000.0*(*decoded)/ms:0,'f',0));*decoded=0;});fpsTimer->start();
        auto present=session_.frameReady;
        session_.frameReady=[present,videoInfo,decoded](auto frame){++*decoded;videoInfo->setText(QString("%1 %2 %3").arg(frame.width).arg(QChar(0x00d7)).arg(frame.height));if(present)present(std::move(frame));};
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
    form->setRowVisible(captureSource_->parentWidget(),false);
    inlineActions("Audio device",audioDevice_,refreshAudio_,switchAudio_);
    // Keep video, audio and advanced settings in separate, predictable groups.
    auto presetRow=form->takeRow(preset_);delete presetRow.labelItem->widget();delete presetRow.labelItem;delete presetRow.fieldItem;form->insertRow(0,"Preset",preset_);
    for(auto* field:{static_cast<QWidget*>(audioKind_),audioDevice_->parentWidget(),static_cast<QWidget*>(audioState_),static_cast<QWidget*>(captureState_)}) {
        auto row=form->takeRow(field);if(row.labelItem){form->addRow(row.labelItem->widget(),field);delete row.labelItem;}else form->addRow(field);delete row.fieldItem;
    }
    detachField(audioHealth_);layout->addWidget(audioHealth_);
    auto* advancedStream=new AdvancedDisclosure;advancedStream->setObjectName("sessionStreamAdvanced");form->addRow(advancedStream);
    auto* advancedContent=new QWidget;advancedContent->setObjectName("SessionAdvancedContent");auto* advancedForm=new QFormLayout(advancedContent);advancedForm->setContentsMargins(0,8,0,8);advancedForm->setVerticalSpacing(12);advancedForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    for(auto* field:{static_cast<QWidget*>(bitrateLimit_),static_cast<QWidget*>(uploadBudgetEnabled_),static_cast<QWidget*>(uploadBudget_)}) {
        auto row=form->takeRow(field);if(row.labelItem){advancedForm->addRow(row.labelItem->widget(),field);delete row.labelItem;}else advancedForm->addRow(field);delete row.fieldItem;
    }
    auto* automaticCap=new QSpinBox;automaticCap->setRange(bitrate_->minimum(),bitrate_->maximum());automaticCap->setValue(bitrate_->value());automaticCap->setSingleStep(bitrate_->singleStep());advancedForm->insertRow(1,"Video ceiling (bps)",automaticCap);
    connect(automaticCap,&QSpinBox::valueChanged,bitrate_,&QSpinBox::setValue);connect(bitrate_,&QSpinBox::valueChanged,automaticCap,&QSpinBox::setValue);
    form->addRow(advancedContent);advancedContent->hide();
    connect(advancedStream,&QPushButton::toggled,this,[sourceSettings]{const auto top=sourceSettings->verticalScrollBar()->value();QTimer::singleShot(0,sourceSettings,[sourceSettings,top]{sourceSettings->verticalScrollBar()->setValue(top);});});
    auto refreshAdvanced=[this,form,advancedStream,advancedContent,advancedForm,automaticCap] {
        const bool advanced=advancedStream->isChecked();
        advancedContent->setVisible(advanced);
        form->setRowVisible(width_,false);
        form->setRowVisible(height_,false);
        form->setRowVisible(fps_,false);
        const bool automatic=bitrateMode_->currentIndex()==int(SettingMode::Auto);
        form->setRowVisible(bitrate_,false);
        advancedForm->setRowVisible(bitrateLimit_,automatic);
        advancedForm->setRowVisible(automaticCap,automatic&&bitrateLimit_->isChecked());
        advancedForm->setRowVisible(uploadBudget_,uploadBudgetEnabled_->isChecked());
    };
    captureState_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);audioState_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    // Use one frame-rate chooser; keep the existing model fields as the source
    // of truth for configuration and acknowledgements.
    auto* frameRate=new QComboBox;frameRate->setObjectName("sessionFrameRate");frameRate->addItem("Auto",0);
    for(int fps:{30,60,120,144,240})frameRate->addItem(QString("%1 FPS").arg(fps),fps);
    if(frameRate->findData(p.fps)<0)frameRate->addItem(QString("%1 FPS").arg(p.fps),p.fps);
    frameRate->setCurrentIndex(p.fpsMode==SettingMode::Auto?0:frameRate->findData(p.fps));
    int fpsRow=0;QFormLayout::ItemRole fpsRole;form->getWidgetPosition(fpsMode_,&fpsRow,&fpsRole);form->insertRow(fpsRow,"Frame rate",frameRate);
    form->setRowVisible(fpsMode_,false);form->setRowVisible(fps_,false);
    connect(frameRate,&QComboBox::currentIndexChanged,this,[this,frameRate]{const auto value=frameRate->currentData().toInt();fpsMode_->setCurrentIndex(int(value?SettingMode::Manual:SettingMode::Auto));if(value)fps_->setValue(value);});
    auto* resolutionChoice=new QComboBox;resolutionChoice->setObjectName("sessionResolutionPreset");resolutionChoice->addItem("Auto");resolutionChoice->addItem("Native");
    for(const auto size:{QSize(854,480),QSize(1280,720),QSize(1920,1080),QSize(2560,1440),QSize(3840,2160)})resolutionChoice->addItem(QString("%1p · %2 × %1").arg(size.height()).arg(size.width()),size);
    if(p.resolution==ResolutionMode::Fixed&&resolutionChoice->findData(QSize(p.width,p.height))<0)resolutionChoice->addItem(QString("%1 × %2").arg(p.width).arg(p.height),QSize(p.width,p.height));
    resolutionChoice->setCurrentIndex(p.resolution==ResolutionMode::Auto?0:p.resolution==ResolutionMode::Native?1:resolutionChoice->findData(QSize(p.width,p.height)));
    int resolutionRow=0;QFormLayout::ItemRole resolutionRole;form->getWidgetPosition(resolution_,&resolutionRow,&resolutionRole);form->insertRow(resolutionRow,"Resolution",resolutionChoice);form->setRowVisible(resolution_,false);
    connect(resolutionChoice,&QComboBox::currentIndexChanged,this,[this,resolutionChoice](int index){resolution_->setCurrentIndex(int(index==0?ResolutionMode::Auto:index==1?ResolutionMode::Native:ResolutionMode::Fixed));if(index>1){const auto size=resolutionChoice->currentData().toSize();width_->setValue(size.width());height_->setValue(size.height());}});
    auto* bitrateChoice=new QComboBox;bitrateChoice->setObjectName("sessionBitratePreset");bitrateChoice->addItem("Auto",0);
    for(int mbps:{5,10,15,20,30,50,80,100})bitrateChoice->addItem(QString("%1 Mbps").arg(mbps),mbps*1000000);
    const int currentBitrate=p.bitrateMode==SettingMode::Manual?p.bitrateLimitBps.value_or(0):0;
    if(bitrateChoice->findData(currentBitrate)<0)bitrateChoice->addItem(QString("%1 Mbps").arg(currentBitrate/1000000.0),currentBitrate);
    bitrateChoice->setCurrentIndex(bitrateChoice->findData(currentBitrate));
    int bitrateRow=0;QFormLayout::ItemRole bitrateRole;form->getWidgetPosition(bitrateMode_,&bitrateRow,&bitrateRole);form->insertRow(bitrateRow,"Bitrate",bitrateChoice);form->setRowVisible(bitrateMode_,false);
    connect(bitrateChoice,&QComboBox::currentIndexChanged,this,[this,bitrateChoice]{const auto value=bitrateChoice->currentData().toInt();bitrateMode_->setCurrentIndex(int(value?SettingMode::Manual:SettingMode::Auto));if(value)bitrate_->setValue(value);});
    connect(advancedStream,&QPushButton::toggled,this,refreshAdvanced);
    for(auto* combo:{audioKind_,resolution_,fpsMode_,bitrateMode_})connect(combo,&QComboBox::currentIndexChanged,this,refreshAdvanced);
    connect(bitrateLimit_,&QCheckBox::toggled,this,refreshAdvanced);connect(uploadBudgetEnabled_,&QCheckBox::toggled,this,refreshAdvanced);refreshAdvanced();
    form->setContentsMargins(12,20,12,20);form->setVerticalSpacing(12);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    for(auto* field:formWidget->findChildren<QComboBox*>()) {
        field->setMinimumWidth(0);field->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    }
    auto decorate=[](QComboBox* box,const QString& icon){for(int index=0;index<box->count();++index)box->setItemIcon(index,uiIcon(icon));box->setIconSize(QSize(22,22));};
    decorate(preset_,"preset-gaming");preset_->setItemIcon(1,uiIcon("preset-quality"));decorate(captureSource_,"display");decorate(resolutionChoice,"display");decorate(frameRate,"fps");decorate(bitrateChoice,"quality");decorate(audioKind_,"volume");decorate(audioDevice_,"volume");
    if(auto* label=qobject_cast<QLabel*>(form->labelForField(bitrateMode_)))label->setText("Bitrate");
    auto separator=[form](QWidget* before){int row=0;QFormLayout::ItemRole role;form->getWidgetPosition(before,&row,&role);auto* line=new QFrame;line->setObjectName("SettingsDivider");line->setFixedHeight(1);form->insertRow(row,line);};separator(advancedStream);
    alignOptionRows(form);alignOptionRows(advancedForm);alignOptionRows(roomForm);
    detachField(apply_);apply_->setParent(this);apply_->hide();
    sharedAudioSettings_=new QWidget(this);auto* shareAudioForm=new QFormLayout(sharedAudioSettings_);shareAudioForm->setContentsMargins(0,8,0,0);shareAudioForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    for(auto* field:{static_cast<QWidget*>(audioKind_),audioDevice_->parentWidget(),static_cast<QWidget*>(audioProcess_),static_cast<QWidget*>(audioState_)}) {
        auto row=form->takeRow(field);if(row.labelItem){shareAudioForm->addRow(row.labelItem->widget(),field);delete row.labelItem;}else shareAudioForm->addRow(field);delete row.fieldItem;
    }
    auto refreshAudioFields=[this,shareAudioForm]{shareAudioForm->setRowVisible(audioProcess_,audioKind_->currentIndex()==2);shareAudioForm->setRowVisible(audioDevice_->parentWidget(),audioKind_->currentIndex()<2);};
    connect(audioKind_,&QComboBox::currentIndexChanged,this,refreshAudioFields);refreshAudioFields();sharedAudioSettings_->hide();alignOptionRows(shareAudioForm);
    detachField(captureState_);layout->addWidget(captureState_);
    if(config.room.host) { settingsTabs->addTab(sourceSettings,"Stream"); sourceSettings->show(); playbackWidget->hide(); }
    else { settingsTabs->addTab(playbackWidget,"Playback"); playbackWidget->show(); sourceSettings->hide();
        if(auto* controller=gamepad_->findChild<QComboBox*>("controllerDevice"))playbackForm->addRow("Controller",controller);
    }
    layout->removeItem(roomForm);
    auto* roomSettings=new QWidget; roomSettings->setLayout(roomForm);
    if(config.room.host) settingsTabs->addTab(roomSettings,"Room"); else {roomSettings->setParent(settingsPage);roomSettings->hide();}
    // Profile editing belongs to the application's top-bar menu.
    roomForm->setRowVisible(nickname_,false); roomForm->setRowVisible(updateNickname_,false);
    if(config.room.host)roomForm->setRowVisible(updatePolicy_,false);roomForm->setRowVisible(reloadRoom,false);
    layout->removeWidget(roomUpdateState_); roomForm->addRow(roomUpdateState_);
    layout->removeWidget(settingsState_); form->addRow(settingsState_);
    layout->removeWidget(roomLink_);roomLink_->setParent(this);roomLink_->hide();
    settingsTabs->setTabIcon(0,uiIcon(config.room.host?"play":"volume"));if(config.room.host)settingsTabs->setTabIcon(1,uiIcon("room"));settingsTabs->setIconSize(QSize(22,22));settingsTabs->tabBar()->setExpanding(true);
    if(config.room.host) {
        auto* streamSave=new QTimer(this);streamSave->setSingleShot(true);streamSave->setInterval(350);
        auto queueStream=[streamSave]{streamSave->start();};
        for(auto* choice:{preset_,frameRate,resolutionChoice,bitrateChoice})connect(choice,&QComboBox::activated,this,queueStream);
        for(auto* toggle:{bitrateLimit_,uploadBudgetEnabled_})connect(toggle,&QCheckBox::clicked,this,queueStream);
        for(auto* value:{automaticCap,uploadBudget_})connect(value,&QSpinBox::valueChanged,this,[value,queueStream]{if(value->hasFocus())queueStream();});
        connect(streamSave,&QTimer::timeout,this,[this,streamSave]{if(session_.settingsPending()){streamSave->start();return;}if(session_.status().phase==RoomPhase::Active)apply_->click();});
        connect(apply_,&QPushButton::clicked,streamSave,&QTimer::stop);
        auto* roomSave=new QTimer(this);roomSave->setSingleShot(true);roomSave->setInterval(500);
        connect(name_,&QLineEdit::textEdited,this,[roomSave]{roomSave->start();});
        connect(publicRoom_,&QCheckBox::clicked,this,[roomSave]{roomSave->start();});
        connect(viewerLimit_,&QSpinBox::valueChanged,this,[roomSave,this]{if(viewerLimit_->hasFocus())roomSave->start();});
        connect(updatePolicy_,&QPushButton::clicked,roomSave,&QTimer::stop);
        connect(roomSave,&QTimer::timeout,this,[this,roomSave]{
            if(closing_)return;
            if(session_.roomUpdatePending()){roomSave->start();return;}
            const auto status=session_.status();if(status.phase!=RoomPhase::Active)return;
            if(name_->text().trimmed().isEmpty()){roomUpdateState_->setText("Enter a room name.");return;}
            const RoomPolicy policy{name_->text().toStdString(),publicRoom_->isChecked(),viewerLimit_->value()};
            if(policy.name==status.policy.name&&policy.publicRoom==status.policy.publicRoom&&policy.viewerLimit==status.policy.viewerLimit){editingRoom_=false;return;}
            updatingNickname_=false;autoRoomUpdate_=true;roomSubmittedSequence_=roomEditSequence_;
            roomUpdateState_->setText("Saving…");session_.updatePolicy(policy,status.revision);
        });
        auto* audioSave=new QTimer(this);audioSave->setSingleShot(true);audioSave->setInterval(300);
        for(auto* choice:{audioKind_,audioDevice_})connect(choice,&QComboBox::activated,this,[audioSave]{audioSave->start();});
        connect(audioProcess_,&QSpinBox::editingFinished,this,[audioSave]{audioSave->start();});
        connect(audioSave,&QTimer::timeout,this,[this,audioSave]{
            if(closing_||session_.status().phase!=RoomPhase::Active)return;
            if(session_.audioPending()){audioSave->start();return;}
            if(!session_.status().activePeers){audioState_->setText("Audio change pending until a viewer connects.");audioSave->start();return;}
            switchAudio_->click();
        });
        connect(switchAudio_,&QPushButton::clicked,audioSave,&QTimer::stop);switchAudio_->hide();
    }
    auto* detailsPopup=new SessionPopup("Stream details",this);detailsPopup->setObjectName("SessionDetailsPopup");detailsPopup->body->addWidget(scroll,1);scroll->show();
    connect(detailsPopup,&QDialog::finished,this,[this,showVideo=!config.room.host&&config.preview]{video_->setVisible(showVideo);});
    connect(detailsButton,&QPushButton::clicked,this,[this,detailsPopup]{video_->hide();detailsPopup->open();});
    auto* footer=new QHBoxLayout; footer->setSpacing(12); dashboardLayout->addLayout(footer);
    auto* openSettings=new QPushButton(config.room.host?"Room settings":"Playback settings"); openSettings->setObjectName("openSessionSettings");
    openSettings->setIcon(uiIcon("settings"));if(config.room.host)footer->addWidget(openSettings);
    connect(openSettings,&QPushButton::clicked,this,[showSettings,settingsTabs]{settingsTabs->setCurrentIndex(0);showSettings();});
    if(config.room.host) {copyLink_->setText("Copy invite");copyLink_->setIcon(uiIcon("copy"));move(copyLink_,footer);}
    else {
        auto* mute=new QPushButton; mute->setObjectName("sessionMute"); mute->setCheckable(true); mute->setChecked(playbackMuted_->isChecked());mute->setFixedSize(42,42);mute->setIconSize(QSize(20,20));
        auto syncMute=[mute](bool muted){mute->setIcon(uiIcon(muted?"mute":"volume"));mute->setToolTip(muted?"Unmute":"Mute");mute->setAccessibleName(muted?"Unmute audio":"Mute audio");};syncMute(mute->isChecked());
        footer->addWidget(mute);
        auto* volume=new SessionVolumeSlider; volume->setObjectName("sessionVolume"); volume->setRange(0,100);volume->setValue(playbackVolume_->value());volume->setMinimumWidth(90);volume->setMaximumWidth(160);footer->addWidget(volume);
        auto* volumeText=new QLabel(QString("%1%").arg(volume->value()));volumeText->setMinimumWidth(36);footer->addWidget(volumeText);footer->addStretch();
        auto* volumeDelay=new QTimer(this); volumeDelay->setSingleShot(true); volumeDelay->setInterval(100);
        connect(volume,&QSlider::valueChanged,this,[this,volumeDelay,volumeText](int value){playbackVolume_->setValue(value);volumeText->setText(QString("%1%").arg(value));volumeDelay->start();});
        connect(volumeDelay,&QTimer::timeout,this,[this,volumeDelay,volume,mute]{
            if(session_.playbackPending())volumeDelay->start();
            else if(session_.status().phase==RoomPhase::Active) {
                playbackState_->setText("Applying playback settings…");
                session_.updatePlayback({playbackDevice_->currentData().toString().toStdWString(),unsigned(volume->value()),mute->isChecked()});
            }
        });
        connect(mute,&QPushButton::toggled,this,[this,volumeDelay,syncMute](bool checked){syncMute(checked);playbackMuted_->setChecked(checked);volumeDelay->start(0);});
        connect(playbackMuted_,&QCheckBox::toggled,mute,[mute,syncMute](bool checked){const QSignalBlocker block(mute);mute->setChecked(checked);syncMute(checked);});
        connect(playbackVolume_,&QSpinBox::valueChanged,volume,[volume,volumeText](int value){const QSignalBlocker block(volume);volume->setValue(value);volumeText->setText(QString("%1%").arg(value));});
        connect(applyPlayback_,&QPushButton::clicked,volumeDelay,&QTimer::stop);
        connect(playbackDevice_,&QComboBox::activated,this,[volumeDelay]{volumeDelay->start();});
        connect(playbackVolume_,&QSpinBox::valueChanged,this,[this,volumeDelay]{if(playbackVolume_->hasFocus())volumeDelay->start();});
        connect(playbackMuted_,&QCheckBox::clicked,this,[volumeDelay]{volumeDelay->start();});
        auto* fullscreen=new QPushButton("Fullscreen"); fullscreen->setObjectName("sessionFullscreen");fullscreen->setIcon(uiIcon("fullscreen"));fullscreen->setFixedHeight(42);fullscreen->setIconSize(QSize(20,20));footer->addWidget(fullscreen);
        auto* cinema=new QWidget;cinema->setObjectName("StreamFullscreen");auto* cinemaLayout=new QVBoxLayout(cinema);cinemaLayout->setContentsMargins(0,0,0,0);cinemaLayout->setSpacing(0);pages->addWidget(cinema);
        auto previousState=std::make_shared<Qt::WindowStates>();
        qApp->installEventFilter(this);
        exitFullscreen_=[this,pages,dashboard,primary,previousState]{
            if(!streamFullscreen_)return;streamFullscreen_=false;
            primary->addWidget(video_,1);video_->setCornerRadius(8);pages->setCurrentWidget(dashboard);video_->show();
            auto* shell=window();if(auto* app=dynamic_cast<AppShellWindow*>(shell))app->setChromeVisible(true);
            shell->setWindowState(*previousState);video_->setFocus();
        };
        connect(fullscreen,&QPushButton::clicked,this,[this,pages,cinema,cinemaLayout,previousState]{
            if(streamFullscreen_){exitFullscreen_();return;}
            *previousState=window()->windowState();streamFullscreen_=true;video_->setCornerRadius(0);cinemaLayout->addWidget(video_);pages->setCurrentWidget(cinema);video_->show();
            if(auto* app=dynamic_cast<AppShellWindow*>(window()))app->setChromeVisible(false);
            window()->showFullScreen();video_->setFocus();
        });
        auto* escape=new QShortcut(QKeySequence(Qt::Key_Escape),this);connect(escape,&QShortcut::activated,this,[this,settingsPage,hideSettings]{if(streamFullscreen_){exitFullscreen_();return;}if(settingsPage->isVisible())hideSettings();});
        auto* toggle=new QPushButton("Controls"); toggle->setObjectName("toggleSessionControls");toggle->setIcon(uiIcon("controls"));toggle->setIconSize(QSize(20,20));toggle->setFixedHeight(42);toggle->setCheckable(true);toggle->setChecked(true);footer->addWidget(toggle);
        connect(toggle,&QPushButton::toggled,controlsScroll,&QWidget::setVisible);
        openSettings->setText({});openSettings->setAccessibleName("Playback settings");openSettings->setToolTip("Playback settings");openSettings->setFixedSize(42,42);openSettings->setIconSize(QSize(20,20));footer->addWidget(openSettings);
    }
    if(config.room.host)footer->addStretch(); stop_->setText(config.room.host?"Stop sharing":"Leave room");stop_->setIcon(uiIcon("stop","#ffffff"));stop_->setIconSize(QSize(20,20));stop_->setMinimumHeight(42);move(stop_,footer);
    if(config.room.host)for(auto* button:{openSettings,copyLink_,stop_}) {
        button->setFixedHeight(42);button->setIconSize(QSize(20,20));footer->setAlignment(button,Qt::AlignVCenter);
    }
    move(error_,dashboardLayout); error_->hide();
    for(auto* combo:findChildren<QComboBox*>()) styleComboPopup(combo);
    pages->setCurrentWidget(dashboard);
    session_.error = [this](const auto& message) { error_->setText(message);error_->setVisible(!message.isEmpty()); };
    session_.finished = [this,host=config.room.host](const auto& status) {
        apply_->setEnabled(false);
        // Leave remains usable after failures; a normal remote room shutdown
        // follows the same cleanup/navigation path as a local Leave.
        stop_->setEnabled(!closing_);
        if(closing_ || (!host && status.phase==RoomPhase::Stopped))
            QTimer::singleShot(0,this,[this]{close();});
    };
    if (!session_.start(std::move(config))) { stop_->setEnabled(true); apply_->setEnabled(false); }
}
bool RoomSessionWindow::eventFilter(QObject* watched,QEvent* event) {
    auto* widget=qobject_cast<QWidget*>(watched);
    if(widget && (widget==this || isAncestorOf(widget)) && (event->type()==QEvent::ShortcutOverride || event->type()==QEvent::KeyPress || event->type()==QEvent::KeyRelease)) {
        auto* key=static_cast<QKeyEvent*>(event);
        if(key->key()==Qt::Key_Escape) {
            if(event->type()==QEvent::KeyRelease && swallowEscapeRelease_){swallowEscapeRelease_=false;event->accept();return true;}
            if(streamFullscreen_){
                if(event->type()==QEvent::KeyPress){swallowEscapeRelease_=true;exitFullscreen_();}
                event->accept();return true;
            }
        }
    }
    return QWidget::eventFilter(watched,event);
}
void RoomSessionWindow::resizeEvent(QResizeEvent* event) {
    if(sessionColumns_)sessionColumns_->setDirection(event->size().width()<720?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight);
    if(settingsDrawer_)QTimer::singleShot(0,this,[this]{settingsDrawer_->setGeometry(settingsDrawer_->parentWidget()->rect());});
    QWidget::resizeEvent(event);
}
RoomSessionWindow::~RoomSessionWindow() {
    qApp->removeEventFilter(this);
    if(previewWorker_) {previewWorker_->requestInterruption();previewWorker_->wait();delete previewWorker_;}
    // QWidget children otherwise outlive the session_ member they reference.
    delete gamepad_; gamepad_ = nullptr;
}
void RoomSessionWindow::RefreshHostPreview() {
    ++previewRevision_;
    hostPreview_->setText(previewActive_?"Starting live preview…":session_.status().stream.preferences.videoPaused?"Video paused":"Preview suspended to save resources.\nSelect this window to resume the preview.");
    hostPreview_->setToolTip("Live preview runs only while this app is selected. Sharing continues when the preview is suspended.");
    if(previewWorker_) {previewWorker_->requestInterruption();return;}
    if(!previewActive_)return;
    const auto source=previewSource_;const auto revision=previewRevision_;
    auto pending=std::make_shared<std::atomic_bool>(false);
    previewWorker_=QThread::create([this,source,revision,pending] {
        try {
            screenshare::CaptureConfig capture;capture.targetFps=30;capture.allowDisplayFallback=false;
            capture.backend=screenshare::CaptureBackend::WindowsGraphicsCapture;
            capture.sourceType=source.kind==CaptureKind::Window?screenshare::CaptureSourceType::Window:screenshare::CaptureSourceType::Display;
            capture.windowHandle=source.window;capture.displayIndex=source.display;
            screenshare::DesktopCapturer capturer;capturer.Start(capture);
            while(!QThread::currentThread()->isInterruptionRequested()) {
                const auto nextFrame=std::chrono::steady_clock::now()+std::chrono::milliseconds(33);
                auto frame=capturer.TryCaptureFrame(std::chrono::milliseconds(30));
                if(frame&&!frame->pixels.empty()&&!pending->exchange(true)) {
                    auto image=QImage(reinterpret_cast<const uchar*>(frame->pixels.data()),frame->width,frame->height,frame->rowPitch,QImage::Format_RGB32).scaled(640,360,Qt::KeepAspectRatio,Qt::SmoothTransformation);
                    const QSize sourceSize(frame->sourceWidth,frame->sourceHeight);
                    QMetaObject::invokeMethod(this,[this,image,sourceSize,revision,pending]{pending->store(false);if(revision==previewRevision_&&previewActive_){previewSourceSize_=sourceSize;hostPreview_->setPixmap(QPixmap::fromImage(image));}},Qt::QueuedConnection);
                }
                std::this_thread::sleep_until(nextFrame);
            }
        } catch(...) {}
    });
    connect(previewWorker_,&QThread::finished,this,[this,revision] {
        auto* worker=previewWorker_;previewWorker_=nullptr;worker->deleteLater();
        if(revision!=previewRevision_) {if(!closing_&&previewActive_)RefreshHostPreview();return;}
        if(previewActive_)hostPreview_->setText("Live preview unavailable");
    });
    previewWorker_->start();
}
void RoomSessionWindow::revokeControl() { if (gamepad_) gamepad_->Revoke(); }
void RoomSessionWindow::setProfileNickname(const QString& nickname) {
    const auto edit=++profileNicknameEdit_;
    QTimer::singleShot(500,this,[this,nickname,edit]{applyProfileNickname(nickname,edit);});
}
void RoomSessionWindow::applyProfileNickname(QString nickname,uint64_t edit) {
    if(edit!=profileNicknameEdit_||closing_)return;
    if(session_.roomUpdatePending()) {QTimer::singleShot(100,this,[this,nickname,edit]{applyProfileNickname(nickname,edit);});return;}
    const auto status=session_.status();
    if(status.phase!=RoomPhase::Active) {if(profileNicknameResult)profileNicknameResult("Saved for future rooms. The current room is not connected.");return;}
    for(const auto& member:status.members)if(member.peerId==status.peerId&&member.nickname==nickname.toStdString())return;
    nicknameRevision_=status.revision;nickname_->setText(nickname);applyingProfileNickname_=true;updatingNickname_=true;
    updateNickname_->setEnabled(false);updatePolicy_->setEnabled(false);
    roomUpdateState_->setText("Updating nickname…");session_.updateNickname(nickname.toStdString(),status.revision);
}
StreamPreferences RoomSessionWindow::ReadPreferences() const {
    StreamPreferences p; p.preset = StreamPreset(preset_->currentIndex()); p.resolution = ResolutionMode(resolution_->currentIndex());
    p.width = width_->value(); p.height = height_->value(); p.fpsMode = SettingMode(fpsMode_->currentIndex()); p.fps = fps_->value();
    p.bitrateMode = SettingMode(bitrateMode_->currentIndex());
    if (p.bitrateMode == SettingMode::Manual || bitrateLimit_->isChecked()) p.bitrateLimitBps = bitrate_->value();
    if (uploadBudgetEnabled_->isChecked()) p.aggregateUploadLimitBps = uploadBudget_->value();
    p.videoPaused=session_.status().stream.preferences.videoPaused;
    return p;
}
void RoomSessionWindow::closeEvent(QCloseEvent* event) {
    if(exitFullscreen_)exitFullscreen_();
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
