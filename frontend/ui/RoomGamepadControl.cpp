#include "RoomGamepadControl.h"
#include "VideoFrameWidget.h"
#include "shared/MappedInput.h"
#include "shared/RoomInputStatus.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

using namespace screenshare;
RoomGamepadControl::RoomGamepadControl(bool host, std::function<std::shared_ptr<input::Port>()> port,
    std::function<v2::RoomStatus()> room, QWidget* parent, Devices devices, Read read)
    : QWidget(parent), host_(host), port_(std::move(port)), room_(std::move(room)), read_(std::move(read)) {
    setObjectName("roomGamepadControl");
    auto* layout = new QVBoxLayout(this);
    status_ = new QLabel("Remote input is off.", this);
    status_->setObjectName("controllerStatus"); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* showDiagnostics = new QCheckBox("Show input diagnostics", this);
    showDiagnostics->setObjectName("showInputDiagnostics"); layout->addWidget(showDiagnostics);
    diagnostics_ = new QLabel(this); diagnostics_->setObjectName("inputDiagnostics");
    diagnostics_->setTextFormat(Qt::PlainText); diagnostics_->setWordWrap(true);
    diagnostics_->setTextInteractionFlags(Qt::TextSelectableByMouse); diagnostics_->hide(); layout->addWidget(diagnostics_);
    connect(showDiagnostics, &QCheckBox::toggled, diagnostics_, &QWidget::setVisible);
    peers_ = new QComboBox(this); peers_->setObjectName("controllerPeer"); layout->addWidget(peers_);
    capabilities_ = new QComboBox(this); capabilities_->setObjectName("inputCapabilities");
    capabilities_->addItem("Controller",input::Gamepad); capabilities_->addItem("Mouse",input::Mouse);
    capabilities_->addItem("Keyboard (display sharing only)",input::Keyboard); capabilities_->addItem("Mouse and keyboard (display sharing only)",input::Mouse|input::Keyboard);
    layout->addWidget(capabilities_);
    devices_ = new QComboBox(this); devices_->setObjectName("controllerDevice");
    devices_->setVisible(!host); layout->addWidget(devices_);
    auto* refresh = new QPushButton("Refresh controllers", this); refresh->setObjectName("refreshControllers");
    refresh->setVisible(!host); layout->addWidget(refresh);
    consent_ = new QCheckBox(host ? "I allow the selected viewer to use the selected input controls." :
        "Use my selected input controls in this room.", this);
    consent_->setObjectName("controllerConsent"); layout->addWidget(consent_);
    action_ = new QPushButton(host ? "Grant selected control" : "Request selected control", this);
    action_->setObjectName("controllerAction"); action_->setEnabled(false); layout->addWidget(action_);
    auto* revoke = new QPushButton(host ? "Revoke all control" : "Release control", this);
    revoke->setObjectName("revokeController"); layout->addWidget(revoke);
    connect(revoke, &QPushButton::clicked, this, [this] { Revoke(); });
    if (host) {
        auto* selected = new QPushButton("Revoke selected controller", this);
        selected->setObjectName("revokeSelectedController"); layout->addWidget(selected);
        connect(selected, &QPushButton::clicked, this, [this] {
            const auto peer = peers_->currentData().toString().toStdString();
            if (auto port = port_(); port && !peer.empty()) port->Revoke(peer);
            consent_->setChecked(false);
        });
    }
    connect(refresh, &QPushButton::clicked, this, [this, devices = std::move(devices)] {
        Revoke(); const QSignalBlocker blocker(devices_); devices_->clear();
        try { for (const auto& device : devices()) devices_->addItem(QString::fromStdString(device.name), QString::fromStdString(device.id)); }
        catch (...) { status_->setText("Controllers could not be listed."); }
        Tick();
    });
    connect(peers_, &QComboBox::currentIndexChanged, this, [this] { if (!host_) Revoke(); else consent_->setChecked(false); });
    connect(devices_, &QComboBox::currentIndexChanged, this, [this] { Revoke(); });
    connect(capabilities_, &QComboBox::currentIndexChanged, this, [this] { if(!host_)Revoke();else consent_->setChecked(false);Tick(); });
    connect(consent_, &QCheckBox::toggled, this, [this](bool checked) { if(checked)actionError_.clear(); if (!checked && !host_) Revoke(); Tick(); });
    connect(action_, &QPushButton::clicked, this, [this] {
        auto port = port_(); const auto peer = peers_->currentData().toString().toStdString();
        if (!port || peer.empty() || !consent_->isChecked()) return;
        const auto caps=uint8_t(capabilities_->currentData().toUInt());
        if (host_) {
            if ((prepareGrant && !prepareGrant(caps)) || !port->Grant(peer, caps)) {
                port->Revoke(peer);actionError_="Selected input is unavailable. Keyboard requires display sharing; restore the shared window before granting mouse control.";
            }
            consent_->setChecked(false); // Consent applies only to this action/peer.
        } else if (!(caps&input::Gamepad) || !devices_->currentData().toString().isEmpty()) {
            for (const auto& state : port->Read()) if (state.peer == peer) requestPermission_ = state.permission;
            armed_ = port->Request(peer, caps); requestedPeer_ = armed_ ? peer : "";
        }
        Tick();
    });
    auto* timer = new QTimer(this); timer->setInterval(50);
    connect(timer, &QTimer::timeout, this, [this] { Tick(); }); timer->start();
    qApp->installEventFilter(this);
}
RoomGamepadControl::~RoomGamepadControl() { qApp->removeEventFilter(this); Revoke(); }
void RoomGamepadControl::SetVideo(VideoFrameWidget* video) {
    video_=video;
    video_->setRemoteInputHandler([this](const auto& value) {
        if(value.kind==RemoteInputKind::ReleaseControl) {Revoke();return;}
        const auto event=MappedInput(value);const auto port=port_();
        if(!event) {if(value.kind==RemoteInputKind::MouseButton && !value.pressed)Revoke();return;}
        if(!port || !armed_ || requestedPeer_.empty())return;
        if(!port->Submit(requestedPeer_,*event))Revoke();
    });
}
void RoomGamepadControl::Revoke(const QString& explanation) {
    actionError_ = explanation;
    armed_ = false; requestedPeer_.clear(); poller_.reset();
    if(video_)video_->setControlCapture(false,false,false);
    if (auto port = port_()) port->Revoke();
    const QSignalBlocker blocker(consent_); consent_->setChecked(false);
}
bool RoomGamepadControl::eventFilter(QObject* watched, QEvent* event) {
    if (!host_ && watched == window() && (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide))
        Revoke("Input stopped because the viewer was hidden or lost focus. Request fresh permission to resume.");
    if(!host_ && video_ && event->type()==QEvent::FocusOut && (watched==video_ || video_->isAncestorOf(qobject_cast<QWidget*>(watched))))
        QTimer::singleShot(0,this,[this] {auto* focus=QApplication::focusWidget();if(video_ && focus!=video_ && !video_->isAncestorOf(focus))
            Revoke("Input stopped because focus left the video. Request fresh permission to resume.");});
    return QWidget::eventFilter(watched, event);
}
void RoomGamepadControl::Tick() {
    const auto port = port_(); const auto states = port ? port->Read() : std::vector<input::Status>{};
    const auto room = room_(); const auto selected = peers_->currentData().toString();
    QStringList ids;
    for (const auto& state : states) if (state.ready) ids.push_back(QString::fromStdString(state.peer));
    QStringList existing; for (int i = 0; i < peers_->count(); ++i) existing.push_back(peers_->itemData(i).toString());
    if (ids != existing) {
        const QSignalBlocker blocker(peers_); peers_->clear();
        for (const auto& id : ids) {
            QString name;
            for (const auto& member : room.members) if (member.peerId == id.toStdString()) name = QString::fromStdString(member.nickname);
            peers_->addItem(name + " [" + id + "]", id);
        }
        const auto index = peers_->findData(selected); if (index >= 0) peers_->setCurrentIndex(index);
        if (selected != peers_->currentData().toString()) { if (!host_) Revoke(); else consent_->setChecked(false); }
    }
    const auto peer = peers_->currentData().toString().toStdString();
    const auto caps=uint8_t(capabilities_->currentData().toUInt());
    uint8_t granted = 0, requested = 0; uint64_t permission = 0; input::Reason reason = input::Reason::Unavailable;
    bool pending = false, revoking = false;
    QStringList active;
    diagnostics_->setText("No selected peer observations.");
    for (const auto& state : states) {
        if (state.granted) active << QString::fromStdString(state.peer);
        if (state.peer == peer) {
            granted = state.granted; requested = state.requested; permission = state.permission; reason = state.reason; pending = state.grantPending; revoking = state.revokePending;
            auto time = [](auto value) { return value ? QString::number(*value) + " us" : QString("unknown"); };
            diagnostics_->setText(QString("Selected peer: %1\nReason: %2; transport blocked: %3\nQueued transitions: %4; states: %5\nApplied: %6; rejected: %7; coalesced: %8\nLocal queue wait: %9; backend apply: %10\nLocal timings only; not network or input-to-display latency.")
                .arg(QString::fromStdString(state.peer), frontend::InputReason(state.reason), state.transportBlocked ? "yes" : "no")
                .arg(state.reliableQueued).arg(state.stateQueued).arg(state.applied).arg(state.rejected).arg(state.coalesced)
                .arg(time(state.queueWaitUs), time(state.backendApplyUs)));
        }
    }
    action_->setEnabled(room.phase == v2::RoomPhase::Active && consent_->isChecked() && !peer.empty() &&
        (host_ ? (requested&caps)==caps : ((caps&input::Gamepad)?devices_->count()>0:video_ && video_->presentedInputMapping().Valid()) && permission && !armed_ && !revoking));
    if (!host_) {
        if (armed_ && !granted && permission > requestPermission_)
            Revoke("Host permission ended. Request fresh permission to resume.");
        if (granted && (!armed_ || requestedPeer_ != peer || granted!=caps)) { port->Revoke(peer); granted = 0; }
        if (poller_ && poller_->permission() != permission) poller_.reset();
        if ((granted & input::Gamepad) && !poller_) {
            const auto device = devices_->currentData().toString().toStdString();
            poller_ = std::make_unique<input::GamepadPoller>(port, peer, [read = read_, device]() -> std::optional<input::Event> {
                const auto value = read(device); if (!value) return {};
                input::Event event; event.kind = input::Kind::Pad; event.buttons = value->buttons;
                event.leftTrigger = value->leftTrigger; event.rightTrigger = value->rightTrigger;
                event.axes = {value->thumbLX, value->thumbLY, value->thumbRX, value->thumbRY}; return event;
            });
        }
        if (!granted && poller_) Revoke("Controller input stopped. Check the selected device and request fresh permission.");
        if(video_)video_->setControlCapture(bool(granted&3),granted&input::Mouse,granted&input::Keyboard);
    }
    status_->setText(!actionError_.isEmpty()?actionError_:pending ? "Starting selected input…" : granted ? "Remote input active: " + active.join(", ") :
        reason == input::Reason::Backend ? "Input unavailable. Check the shared source, foreground window, or controller driver and slots." :
        reason == input::Reason::Ownership ? "Selected input is already in use." :
        reason == input::Reason::Backpressure ? "Input stopped because the connection is congested. Request fresh permission to resume." :
        reason == input::Reason::Watchdog ? "Input stopped because updates timed out. Request fresh permission to resume." :
        reason == input::Reason::SourceChanged ? "The shared source changed. Request fresh permission to resume." :
        revoking ? "Waiting for release acknowledgement…" : armed_ || requested ? "Waiting for explicit host permission." : "Remote input is off.");
    if (!granted && !active.empty()) status_->setText(status_->text() + "\nOther active peers: " + active.join(", "));
}
