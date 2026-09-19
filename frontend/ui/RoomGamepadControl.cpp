#include "RoomGamepadControl.h"
#include "VideoFrameWidget.h"
#include "UiStyle.h"
#include "Toast.h"
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
#include <QHBoxLayout>
#include <QIcon>

using namespace screenshare;
RoomGamepadControl::RoomGamepadControl(bool host, std::function<std::shared_ptr<input::Port>()> port,
    std::function<v2::RoomStatus()> room, QWidget* parent, Devices devices, Read read)
    : QWidget(parent), host_(host), port_(std::move(port)), room_(std::move(room)), read_(std::move(read)) {
    setObjectName("roomGamepadControl");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0); layout->setSpacing(12);
    if(host) {peerRows_=new QVBoxLayout;layout->addLayout(peerRows_);}
    status_ = new QLabel("Remote input is off.", this);
    status_->setObjectName("controllerStatus"); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true);
    status_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
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
        "Allow selected input", this);
    consent_->setToolTip("Allow the host to grant the selected controls. Uncheck or release control to stop input.");
    consent_->setObjectName("controllerConsent"); layout->addWidget(consent_);
    action_ = new QPushButton(host ? "Grant selected control" : "Request selected control", this);
    action_->setObjectName("controllerAction"); action_->setEnabled(false); layout->addWidget(action_);
    auto* revoke = new QPushButton(host ? "Revoke all control" : "Release control", this);
    revoke->setObjectName("revokeController"); layout->addWidget(revoke);
    if(!host){auto policy=revoke->sizePolicy();policy.setRetainSizeWhenHidden(true);revoke->setSizePolicy(policy);}
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
    auto refreshDevices = [this, devices = std::move(devices)] {
        std::vector<ViewerGamepadDevice> available;
        try { available = devices(); }
        catch (...) { status_->setText("Controllers could not be listed."); return; }
        bool unchanged = devices_->count() == int(available.size());
        for(int index=0;unchanged && index<devices_->count();++index)
            unchanged = devices_->itemData(index).toString()==QString::fromStdString(available[index].id) &&
                devices_->itemText(index)==QString::fromStdString(available[index].name);
        if(unchanged)return;
        const auto selected = devices_->currentData().toString();
        const QSignalBlocker blocker(devices_); devices_->clear();
        for(const auto& device:available)devices_->addItem(QString::fromStdString(device.name),QString::fromStdString(device.id));
        const auto previous=devices_->findData(selected);
        devices_->setCurrentIndex(previous>=0?previous:devices_->count()?0:-1);
        // Keep permission independent of hardware availability. The poller
        // sends neutral state while no selected controller can be read.
        selectedDevice_->store(std::make_shared<const std::string>(devices_->currentData().toString().toStdString()));
        Tick();
    };
    connect(refresh, &QPushButton::clicked, this, refreshDevices);
    if(!host) {
        auto* discovery=new QTimer(this);discovery->setInterval(1500);
        connect(discovery,&QTimer::timeout,this,refreshDevices);discovery->start();
    }
    connect(peers_, &QComboBox::currentIndexChanged, this, [this] { if (!host_) Revoke(); else consent_->setChecked(false); });
    connect(devices_, &QComboBox::currentIndexChanged, this, [this] { selectedDevice_->store(std::make_shared<const std::string>(devices_->currentData().toString().toStdString())); });
    connect(capabilities_, &QComboBox::currentIndexChanged, this, [this] { if(!host_)Revoke();else consent_->setChecked(false);Tick(); });
    connect(consent_, &QCheckBox::toggled, this, [this](bool checked) { if(checked)actionError_.clear(); if (!checked && !host_) Revoke(); Tick(); });
    connect(action_, &QPushButton::clicked, this, [this] {
        auto port = port_(); const auto peer = peers_->currentData().toString().toStdString();
        if (!port || peer.empty() || (host_ && !consent_->isChecked())) return;
        if(!host_) { const QSignalBlocker blocker(consent_); consent_->setChecked(true); actionError_.clear(); }
        const auto caps=uint8_t(capabilities_->currentData().toUInt());
        if (host_) {
            if ((prepareGrant && !prepareGrant(caps)) || !port->Grant(peer, caps)) {
                port->Revoke(peer);actionError_="Selected input is unavailable. Keyboard requires display sharing; restore the shared window before granting mouse control.";
            }
            consent_->setChecked(false); // Consent applies only to this action/peer.
        } else {
            for (const auto& state : port->Read()) if (state.peer == peer) requestPermission_ = state.permission;
            armed_ = port->Request(peer, caps); requestedPeer_ = armed_ ? peer : "";
            if(!armed_)actionError_="Could not send the control request. Try again when the connection is ready.";
        }
        Tick();
    });
    auto* timer = new QTimer(this); timer->setInterval(50);
    connect(timer, &QTimer::timeout, this, [this] { Tick(); }); timer->start();
    for(auto* combo:{peers_,devices_,capabilities_}) {combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);combo->setMinimumContentsLength(12);combo->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);}
    qApp->installEventFilter(this);
    if(host) {
        peers_->hide(); capabilities_->hide(); consent_->hide(); action_->hide();
        revoke->hide();
        findChild<QPushButton*>("revokeSelectedController")->hide();
    } else {
        peers_->hide();
        consent_->hide();
        refresh->hide();

        capabilities_->addItem("Mouse and controller",input::Mouse|input::Gamepad);
        capabilities_->addItem("Keyboard and controller",input::Keyboard|input::Gamepad);
        capabilities_->addItem("Mouse, keyboard and controller",7);
        capabilities_->addItem("No input",0);
        capabilities_->hide();
        devices_->setPlaceholderText("No controller detected");
        auto* selection=new QWidget;auto* choices=new QVBoxLayout(selection);choices->setContentsMargins(0,0,0,0);
        std::vector<QCheckBox*> checks;
        for(const auto& entry:{std::pair{"Mouse",input::Mouse},std::pair{"Keyboard",input::Keyboard},std::pair{"Controller",input::Gamepad}}) {
            auto* check=new QCheckBox(entry.first);check->setProperty("capability",int(entry.second));check->setChecked(entry.second==input::Gamepad);
            check->setObjectName(QString("select%1Input").arg(entry.first));auto* row=new QHBoxLayout;auto* icon=new QLabel;icon->setObjectName(QString("permission%1").arg(entry.first));icon->setProperty("capability",int(entry.second));icon->setFixedSize(24,24);
            row->addWidget(check,1);row->addWidget(icon);choices->addLayout(row);checks.push_back(check);
        }
        layout->insertWidget(layout->indexOf(consent_),selection);
        for(auto* check:checks)connect(check,&QCheckBox::toggled,this,[this,checks] {
            int caps=0;for(auto* selected:checks)if(selected->isChecked())caps|=selected->property("capability").toInt();
            capabilities_->setCurrentIndex(capabilities_->findData(caps));
        });
        for(auto* check:checks)connect(check,&QCheckBox::clicked,this,[this]{consent_->setChecked(capabilities_->currentData().toUInt()!=0);});
        connect(capabilities_,&QComboBox::currentIndexChanged,this,[this,checks] {
            const auto caps=capabilities_->currentData().toUInt();
            for(auto* check:checks){QSignalBlocker block(check);check->setChecked(caps&check->property("capability").toUInt());}
        });
        refresh->click();
    }
}
RoomGamepadControl::~RoomGamepadControl() { qApp->removeEventFilter(this); Revoke(); }
void RoomGamepadControl::SetVideo(VideoFrameWidget* video) {
    video_=video;
    video_->setRemoteInputHandler([this](const auto& value) {
        if(value.kind==RemoteInputKind::ReleaseControl) {Revoke();return;}
        const auto event=MappedInput(value);const auto port=port_();
        if(!event) {if(value.kind==RemoteInputKind::MouseButton && !value.pressed)Revoke();return;}
        if(!port || !armed_ || inputPaused_->load() || requestedPeer_.empty())return;
        uint64_t permission=0;
        for(const auto& state:port->Read())if(state.peer==requestedPeer_)permission=state.permission;
        if(!port->SubmitIfCurrent(requestedPeer_,permission,*event)){Revoke();return;}
        if(event->kind==input::Kind::Key || event->kind==input::Kind::Button) {
            const int id=event->kind==input::Kind::Key?event->key:256+event->button;
            if(event->down)heldInput_[id]={*event,permission};else heldInput_.erase(id);
        }
        if(event->kind==input::Kind::Pointer)for(auto& [id,held]:heldInput_)if(held.first.kind==input::Kind::Button){held.first.x=event->x;held.first.y=event->y;}

    });
}
void RoomGamepadControl::Revoke(const QString& explanation) {
    actionError_ = explanation;
    heldInput_.clear();
    armed_ = false; capturingDesktop_ = false; requestedPeer_.clear(); poller_.reset();
    if(video_)video_->setControlCapture(false,false,false);
    if (auto port = port_()) {for(const auto& state:port->Read())blockedPermission_=std::max(blockedPermission_,state.permission);port->Revoke();}
    const QSignalBlocker blocker(consent_); consent_->setChecked(false);
}
void RoomGamepadControl::PauseInput() {
    inputPaused_->store(true);
    // Release only input this viewer actually held, under its original grant.
    // A focus change is not a request to withdraw the host's permission.
    if(auto port=port_())for(auto& [id,held]:heldInput_) {
        held.first.down=false;
        if(!port->SubmitIfCurrent(requestedPeer_,held.second,held.first)) {
            port->RevokeIfCurrent(requestedPeer_,held.second);
        }
    }
    heldInput_.clear();
    capturingDesktop_=false;
    if(video_)video_->setControlCapture(false,false,false);
}
bool RoomGamepadControl::eventFilter(QObject* watched, QEvent* event) {
    if(host_)return QWidget::eventFilter(watched,event);
    if(watched==window()) {
        if(event->type()==QEvent::WindowDeactivate || event->type()==QEvent::Hide)PauseInput();
        else if(event->type()==QEvent::WindowActivate)inputPaused_->store(false);
    }
    const bool videoFocus=video_ && (watched==video_ || video_->isAncestorOf(qobject_cast<QWidget*>(watched)));
    if(videoFocus && event->type()==QEvent::FocusIn)inputPaused_->store(false);
    if(videoFocus && event->type()==QEvent::MouseButtonPress && inputPaused_->load()){inputPaused_->store(false);Tick();}
    if(videoFocus && capturingDesktop_ && event->type()==QEvent::FocusOut)
        QTimer::singleShot(0,this,[this] {auto* focus=QApplication::focusWidget();if(video_ && focus!=video_ && !video_->isAncestorOf(focus))PauseInput();});
    return QWidget::eventFilter(watched, event);
}
void RoomGamepadControl::Tick() {
    const auto port = port_(); const auto states = port ? port->Read() : std::vector<input::Status>{};
    const auto room = room_(); const auto selected = peers_->currentData().toString();
    if(host_) {
        QStringList present;
        for(const auto& member:room.members) if(!member.host) {
            const auto id=QString::fromStdString(member.peerId);present<<id;
            if(!peerCards_.contains(id)) {
                auto* card=new QWidget;card->setObjectName("HostViewerRow");
                auto* body=new QVBoxLayout(card);body->setContentsMargins(0,8,0,12);
                auto* identity=new QHBoxLayout;auto* avatar=new QLabel;avatar->setObjectName("PeerAvatar");avatar->setAlignment(Qt::AlignCenter);avatar->setFixedSize(40,40);identity->addWidget(avatar);
                auto* text=new QVBoxLayout; text->setSpacing(2);
                auto* name=new QLabel;name->setObjectName("PeerName");name->setTextFormat(Qt::PlainText);name->setWordWrap(true);text->addWidget(name);
                auto* status=new QLabel;status->setObjectName("PeerControlState");status->setWordWrap(true);text->addWidget(status);identity->addLayout(text,1);body->addLayout(identity);
                auto* buttons=new QHBoxLayout;buttons->setSpacing(4);identity->addLayout(buttons);
                for(const auto& entry: {std::pair{"mouse",input::Mouse},std::pair{"keyboard",input::Keyboard},std::pair{"gamepad",input::Gamepad}}) {
                    auto* button=new QPushButton;button->setObjectName("PeerCapability");button->setCheckable(true);
                    button->setProperty("capability",int(entry.second));button->setProperty("peerId",id);
                    button->setIcon(uiIcon(entry.first));button->setIconSize(QSize(18,18));button->setFixedSize(36,36);buttons->addWidget(button);
                    connect(button,&QPushButton::clicked,this,[this,id,cap=uint8_t(entry.second)] {
                        auto port=port_();if(!port)return;
                        for(const auto& state:port->Read()) if(state.peer==id.toStdString()) {
                            const auto next=uint8_t(state.granted^cap);
                            if(!next)port->Revoke(state.peer);
                            else if((prepareGrant&&!prepareGrant(next))||!port->Grant(state.peer,next))actionError_="This input is unavailable for the shared source.";
                            else actionError_.clear();
                            break;
                        }
                        Tick();
                    });
                }
                auto* requestActions=new QHBoxLayout;body->addLayout(requestActions);
                auto* accept=new QPushButton("Accept");accept->setObjectName("acceptPeerRequest");requestActions->addWidget(accept);
                connect(accept,&QPushButton::clicked,this,[this,id]{if(auto port=port_())for(const auto& state:port->Read())if(state.peer==id.toStdString()&&state.requested){
                    const auto next=uint8_t(state.granted|state.requested);
                    if((prepareGrant&&!prepareGrant(next))||!port->Grant(state.peer,next))actionError_="Requested input is unavailable for this source.";break;
                }});
                auto* deny=new QPushButton("Deny");deny->setObjectName("denyPeerRequest");requestActions->addWidget(deny);
                connect(deny,&QPushButton::clicked,this,[this,id]{if(auto port=port_())port->Revoke(id.toStdString());});
                peerRows_->addWidget(card);peerCards_.insert(id,card);
            }
            auto* card=peerCards_.value(id);const auto name=QString::fromStdString(member.nickname);
            card->findChild<QLabel*>("PeerName")->setText(name);
            card->findChild<QLabel*>("PeerAvatar")->setText(name.left(1).toUpper());
            auto state=std::find_if(states.begin(),states.end(),[&](const auto& s){return s.peer==member.peerId;});
            const bool ready=state!=states.end()&&state->ready;
            card->findChild<QLabel*>("PeerControlState")->setText(!ready?"Connecting…":state->grantPending?"Applying control…":state->revokePending?"Releasing control…":state->requested?"Requests control":state->granted?"Control granted":"Watching");
            QStringList requestedNames;
            if(ready)for(const auto& entry:{std::pair{input::Mouse,"mouse"},std::pair{input::Keyboard,"keyboard"},std::pair{input::Gamepad,"controller"}})if(state->requested&entry.first)requestedNames<<entry.second;
            if(!requestedNames.empty())card->findChild<QLabel*>("PeerControlState")->setText("Requests " + requestedNames.join(", "));
            card->findChild<QPushButton*>("denyPeerRequest")->setVisible(!requestedNames.empty());
            auto* accept=card->findChild<QPushButton*>("acceptPeerRequest");accept->setVisible(!requestedNames.empty());accept->setEnabled(ready&&!state->grantPending&&!state->revokePending);
            for(auto* button:card->findChildren<QPushButton*>("PeerCapability")) {
                const auto cap=button->property("capability").toUInt();const bool on=ready&&(state->granted&cap);
                const bool supported=!(cap==input::Keyboard&&room.capture.selected.kind==media::CaptureKind::Window);
                button->setChecked(on);button->setEnabled(ready&&supported&&!state->grantPending&&!state->revokePending);
                const auto control=cap==input::Mouse?"mouse":cap==input::Keyboard?"keyboard":"controller";
                const auto label=QString("%1 %2 %3 %4").arg(on?"Revoke":"Grant",control,on?"from":"to",name);
                button->setToolTip(supported?label:"Keyboard control requires display sharing");button->setAccessibleName(label);
            }
        }
        for(const auto& id:peerCards_.keys())if(!present.contains(id))delete peerCards_.take(id);
    }
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
    if (!host_) {
        if (armed_ && !granted && permission > requestPermission_)
            Revoke("Host permission ended. Request fresh permission to resume.");
        // The host's explicit grant is authoritative; request checkboxes only
        // describe what the viewer asks for, not which host grants it accepts.
        if(granted && !armed_ && !revoking && permission>blockedPermission_) {
            armed_=true;requestedPeer_=peer;requestPermission_=permission;
            const QSignalBlocker block(consent_);consent_->setChecked(true);actionError_.clear();
        }
        if(granted && (!armed_ || requestedPeer_!=peer)){granted=0;}
        if (poller_ && poller_->permission() != permission) poller_.reset();
        if ((granted & input::Gamepad) && !poller_) {
            poller_ = std::make_unique<input::GamepadPoller>(port, peer, [read = read_, selected=selectedDevice_, paused=inputPaused_]() -> std::optional<input::Event> {
                input::Event event;event.kind=input::Kind::Pad;
                if(paused->load())return event;
                const auto device=selected->load();
                const auto value=device&&!device->empty()?read(*device):std::optional<RemoteGamepadState>{};
                if(!value)return event; // Keep permission, release held state, and wait for a device.
                event.buttons = value->buttons;
                event.leftTrigger = value->leftTrigger; event.rightTrigger = value->rightTrigger;
                event.axes = {value->thumbLX, value->thumbLY, value->thumbRX, value->thumbRY}; return event;
            });
        }
        if (!granted && poller_) Revoke("Controller input stopped. Check the selected device and request fresh permission.");
        capturingDesktop_=!inputPaused_->load() && bool(granted&(input::Mouse|input::Keyboard));
        if(video_)video_->setControlCapture(capturingDesktop_,granted&input::Mouse,granted&input::Keyboard);
    }
    action_->setEnabled(room.phase == v2::RoomPhase::Active && caps && (!host_ || consent_->isChecked()) && !peer.empty() &&
        (host_ ? true : permission && !armed_ && !requested && !pending && !revoking));
    status_->setText(!actionError_.isEmpty()?actionError_:pending ? "Starting selected input…" : granted ? (host_?"Control active":"Host granted control") :
        reason == input::Reason::Backend ? "Input unavailable. Check the shared source, foreground window, or controller driver and slots." :
        reason == input::Reason::Ownership ? "Selected input is already in use." :
        reason == input::Reason::Backpressure ? "Input stopped because the connection is congested. Request fresh permission to resume." :
        reason == input::Reason::Watchdog ? "Input stopped because updates timed out. Request fresh permission to resume." :
        reason == input::Reason::SourceChanged ? "The shared source changed. Request fresh permission to resume." :
        revoking ? "Waiting for release acknowledgement…" : armed_ || requested ? "Waiting for explicit host permission." :
        !host_ && (caps&input::Gamepad) && devices_->currentData().toString().isEmpty() ? "Controller input will start when a controller is connected." :
        !host_ && !caps ? "Select the controls you want to request." : "Remote input is off.");
    if (!granted && !active.empty()) status_->setText(QString("%1 viewer(s) have control").arg(active.size()));
    if(host_)status_->setVisible(!actionError_.isEmpty());
    else {
        status_->hide();
        action_->setToolTip(status_->text());
        for(const auto& entry:{std::pair{"Mouse","mouse"},std::pair{"Keyboard","keyboard"},std::pair{"Controller","gamepad"}}) {
            auto* icon=findChild<QLabel*>(QString("permission%1").arg(entry.first));
            const bool allowed=granted & icon->property("capability").toUInt();
            if(!icon->property("initialized").toBool() || icon->property("granted").toBool()!=allowed) {
                icon->setPixmap(uiIcon(entry.second,allowed?"#65db98":"#83958d").pixmap(22,22));
                icon->setProperty("initialized",true);icon->setProperty("granted",allowed);
                const auto description=QString("%1: %2").arg(entry.first,allowed?"Permission granted":"No permission");
                icon->setAccessibleName(description);icon->setToolTip(description);
            }
        }
        const auto notice=status_->text();
        if(notice!=lastNotice_) {
            if(isVisible() && (!actionError_.isEmpty() || granted || armed_ || requested))Toast::show(window(),notice,3500);
            lastNotice_=notice;
        }
        findChild<QPushButton*>("revokeController")->setVisible(granted||requested||armed_);
    }
}
