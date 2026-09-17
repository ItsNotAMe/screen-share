#include "RoomGamepadControl.h"
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
    status_ = new QLabel("Controller control is off.", this);
    status_->setObjectName("controllerStatus"); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true);
    layout->addWidget(status_);
    peers_ = new QComboBox(this); peers_->setObjectName("controllerPeer"); layout->addWidget(peers_);
    devices_ = new QComboBox(this); devices_->setObjectName("controllerDevice");
    devices_->setVisible(!host); layout->addWidget(devices_);
    auto* refresh = new QPushButton("Refresh controllers", this); refresh->setObjectName("refreshControllers");
    refresh->setVisible(!host); layout->addWidget(refresh);
    consent_ = new QCheckBox(host ? "I allow the selected viewer to control a virtual gamepad." :
        "Use my selected controller in this room.", this);
    consent_->setObjectName("controllerConsent"); layout->addWidget(consent_);
    action_ = new QPushButton(host ? "Grant controller" : "Request controller", this);
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
    connect(consent_, &QCheckBox::toggled, this, [this](bool checked) { if (!checked && !host_) Revoke(); Tick(); });
    connect(action_, &QPushButton::clicked, this, [this] {
        auto port = port_(); const auto peer = peers_->currentData().toString().toStdString();
        if (!port || peer.empty() || !consent_->isChecked()) return;
        if (host_) {
            if (!port->Grant(peer, input::Gamepad)) status_->setText("Controller grant is unavailable.");
            consent_->setChecked(false); // Consent applies only to this action/peer.
        } else if (!devices_->currentData().toString().isEmpty()) {
            for (const auto& state : port->Read()) if (state.peer == peer) requestPermission_ = state.permission;
            armed_ = port->Request(peer, input::Gamepad); requestedPeer_ = armed_ ? peer : "";
        }
        Tick();
    });
    auto* timer = new QTimer(this); timer->setInterval(50);
    connect(timer, &QTimer::timeout, this, [this] { Tick(); }); timer->start();
    qApp->installEventFilter(this);
}
RoomGamepadControl::~RoomGamepadControl() { qApp->removeEventFilter(this); Revoke(); }
void RoomGamepadControl::Revoke() {
    armed_ = false; requestedPeer_.clear(); poller_.reset();
    if (auto port = port_()) port->Revoke();
    const QSignalBlocker blocker(consent_); consent_->setChecked(false);
}
bool RoomGamepadControl::eventFilter(QObject* watched, QEvent* event) {
    if (!host_ && watched == window() && (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide)) Revoke();
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
    uint8_t granted = 0, requested = 0; uint64_t permission = 0; input::Reason reason = input::Reason::Unavailable;
    bool pending = false, revoking = false;
    QStringList active;
    for (const auto& state : states) {
        if (state.granted) active << QString::fromStdString(state.peer);
        if (state.peer == peer) { granted = state.granted; requested = state.requested; permission = state.permission; reason = state.reason; pending = state.grantPending; revoking = state.revokePending; }
    }
    action_->setEnabled(room.phase == v2::RoomPhase::Active && consent_->isChecked() && !peer.empty() &&
        (host_ ? bool(requested & input::Gamepad) : devices_->count() > 0 && permission && !armed_ && !revoking));
    if (!host_) {
        if (armed_ && !granted && permission > requestPermission_) Revoke();
        if (granted && (!armed_ || requestedPeer_ != peer)) { port->Revoke(peer); granted = 0; }
        if ((granted & input::Gamepad) && !poller_) {
            const auto device = devices_->currentData().toString().toStdString();
            poller_ = std::make_unique<input::GamepadPoller>(port, peer, [read = read_, device]() -> std::optional<input::Event> {
                const auto value = read(device); if (!value) return {};
                input::Event event; event.kind = input::Kind::Pad; event.buttons = value->buttons;
                event.leftTrigger = value->leftTrigger; event.rightTrigger = value->rightTrigger;
                event.axes = {value->thumbLX, value->thumbLY, value->thumbRX, value->thumbRY}; return event;
            });
        }
        if (!granted && poller_) Revoke();
    }
    status_->setText(pending ? "Starting the selected controller…" : !active.empty() ? "Controller control active: " + active.join(", ") :
        reason == input::Reason::Backend ? "Controller unavailable. Check the host controller driver and free player slots." :
        reason == input::Reason::Ownership ? "Controller slots are already in use." :
        armed_ || requested ? "Waiting for explicit host permission." : "Controller control is off.");
}
