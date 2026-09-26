#include "RoomSocket.h"
#include "room/protocol/RoomProtocol.h"
#include <QWebSocket>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QThread>
#include <algorithm>
#include <stdexcept>

namespace screenshare::room::qt {
namespace {
constexpr qint64 WriteLimit = 256 * 1024;
bool Identifier(const QString& value) {
    static const QRegularExpression pattern("\\A[A-Za-z0-9_-]{1,128}\\z");
    return pattern.match(value).hasMatch();
}
}
RoomSocket::RoomSocket(Notify notify, bool allowPlainLoopback, QObject* parent)
    : QObject(parent), notify_(std::move(notify)), allowPlainLoopback_(allowPlainLoopback),
      reconnect_(this), heartbeat_(this), deadline_(this) {
    reconnect_.setSingleShot(true); heartbeat_.setSingleShot(true); deadline_.setSingleShot(true);
    connect(&reconnect_, &QTimer::timeout, this, [this] { Open(); });
    connect(&deadline_, &QTimer::timeout, this, [this] { Drop(Error::Timeout, true); });
    connect(&heartbeat_, &QTimer::timeout, this, [this] {
        if (!Write("v2:ping")) { Drop(Error::Backpressure, true); return; }
        awaitingPong_ = true;
        deadline_.start(10000);
    });
}
RoomSocket::~RoomSocket() { Stop(); }
void RoomSocket::CheckThread() const {
    if (QThread::currentThread() != thread()) throw std::logic_error("Room socket requires its networking thread");
}
void RoomSocket::Emit(EventKind kind, Error error, QJsonObject value) {
    if (notify_) notify_({kind, generation_, error, std::move(value), kind == EventKind::Snapshot ? cache_.Revision() : std::nullopt});
}
bool RoomSocket::Start(Config config) {
    CheckThread(); Stop();
    const auto& url = config.origin;
    const bool loopback = url.host() == "127.0.0.1" || url.host() == "::1";
    const bool secure = url.scheme() == "wss" || (allowPlainLoopback_ && loopback && url.scheme() == "ws");
    bool valid = secure && url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() &&
        !url.hasQuery() && !url.hasFragment() && (url.path().isEmpty() || url.path() == "/");
    if (config.directory) valid = valid && config.token.isEmpty() && config.roomId.isEmpty() && config.selfPeerId.isEmpty() && config.expectedRole.isEmpty();
    else {
        valid = valid && Identifier(config.roomId) && Identifier(config.selfPeerId) &&
            (config.expectedRole == "host" || config.expectedRole == "viewer") &&
            !config.token.isEmpty() && config.token.size() <= 512;
        for (auto c : config.token) valid = valid && c >= 33 && c <= 126;
    }
    if (!valid) { Emit(EventKind::Error, Error::Configuration); return false; }
    config_ = std::move(config);
    cache_ = wire::StateSubscription(config_.directory);
    retries_ = 0; active_ = true;
    Open();
    return true;
}
void RoomSocket::Stop() {
    CheckThread();
    active_ = false; ready_ = false; awaitingPong_ = false; ++generation_;
    reconnect_.stop(); heartbeat_.stop(); deadline_.stop(); cache_.Stop();
    if (socket_) {
        auto* old = socket_; socket_ = nullptr;
        old->disconnect(this); old->abort(); old->deleteLater();
    }
    config_ = {};
}
void RoomSocket::Open() {
    if (!active_) return;
    ++generation_; ready_ = false; awaitingPong_ = false;
    cacheGeneration_ = cache_.Start(config_.roomId, config_.selfPeerId);
    const auto generation = generation_;
    socket_ = new QWebSocket(QString{}, QWebSocketProtocol::VersionLatest, this);
    socket_->setMaxAllowedIncomingFrameSize(config_.directory ? 256 * 1024 : 64 * 1024);
    socket_->setMaxAllowedIncomingMessageSize(config_.directory ? 256 * 1024 : 64 * 1024);
    connect(socket_, &QWebSocket::connected, this, [this, generation] {
        if (generation != generation_) return;
        Emit(EventKind::Connected);
        // Handshake alone is not readiness. Require an authoritative snapshot.
        deadline_.start(10000);
    });
    connect(socket_, &QWebSocket::disconnected, this, [this, generation] {
        if (generation == generation_) Drop(Error::Transport, true);
    });
    connect(socket_, &QWebSocket::errorOccurred, this, [this, generation](auto) {
        if (generation == generation_) Drop(Error::Transport, true);
    });
    connect(socket_, &QWebSocket::textMessageReceived, this, [this, generation](const QString& text) { Receive(generation, text); });
    connect(socket_, &QWebSocket::binaryMessageReceived, this, [this, generation](const QByteArray&) {
        if (generation == generation_) Drop(Error::Protocol, true);
    });
    auto url = config_.origin;
    url.setPath(config_.directory ? "/v2/directory/events" : "/v2/rooms/" + config_.roomId + "/events");
    QNetworkRequest request(url);
    if (!config_.directory) request.setRawHeader("Authorization", "Bearer " + config_.token);
    else request.setRawHeader("X-ScreenShare-Directory-Features", "host-nickname");
    // Keep Qt certificate/hostname validation; do not ignore SSL errors.
    deadline_.start(10000);
    Emit(EventKind::Connecting);
    socket_->open(request);
}
void RoomSocket::Drop(Error error, bool retry) {
    if (!active_) return;
    ++generation_; ready_ = false; awaitingPong_ = false;
    heartbeat_.stop(); deadline_.stop(); cache_.Stop();
    if (socket_) {
        auto* old = socket_; socket_ = nullptr;
        old->disconnect(this); old->abort(); old->deleteLater();
    }
    Emit(EventKind::Error, error);
    if (!retry) { active_ = false; config_.token.clear(); return; }
    const auto base = std::min(30000u, 1000u << std::min(retries_++, 5u));
    // Jitter without exceeding the 30-second maximum.
    reconnect_.start(int(base * (0.8 + QRandomGenerator::global()->generateDouble() * 0.2)));
    Emit(EventKind::Reconnecting);
}
bool RoomSocket::Write(const QByteArray& bytes) {
    if (!socket_ || socket_->state() != QAbstractSocket::ConnectedState ||
        bytes.size() > WriteLimit || socket_->bytesToWrite() > WriteLimit - bytes.size()) return false;
    return socket_->sendTextMessage(QString::fromUtf8(bytes)) >= 0;
}
bool RoomSocket::Allowed(const QJsonObject& message, bool incoming) const {
    const auto type = message["type"].toString();
    if (!type.startsWith("signal.")) {
        if (incoming || (type != "room.update" && type != "peer.disconnect")) return true;
    }
    const auto self = config_.selfPeerId;
    const auto remote = message[incoming ? "fromPeerId" : "toPeerId"].toString();
    QString selfRole, remoteRole;
    for (auto member : cache_.Payload()["members"].toArray()) {
        const auto item = member.toObject();
        if (item["peerId"] == self) selfRole = item["role"].toString();
        if (item["peerId"] == remote) remoteRole = item["role"].toString();
    }
    if (!type.startsWith("signal.")) return selfRole == "host";
    if (incoming && message["toPeerId"] != self) return false;
    if (self == remote || selfRole.isEmpty() || remoteRole.isEmpty() || selfRole == remoteRole) return false;
    const auto senderRole = incoming ? remoteRole : selfRole;
    if (type == "signal.offer") return senderRole == "host";
    if (type == "signal.answer" || type == "signal.restart_request") return senderRole == "viewer";
    return true;
}
RoomSocket::SendResult RoomSocket::Send(const QByteArray& bytes) {
    CheckThread();
    if (!active_ || !ready_) return SendResult::NotReady;
    const auto decoded = wire::ValidateClientCommand(bytes, config_.directory);
    if (!decoded.ok || (!config_.directory && decoded.message["roomId"] != config_.roomId)) return SendResult::Invalid;
    if (!Allowed(decoded.message, false)) return SendResult::Forbidden;
    const auto canonical = QJsonDocument(decoded.message).toJson(QJsonDocument::Compact);
    if (!Write(canonical)) { Drop(Error::Backpressure, true); return SendResult::Backpressure; }
    return SendResult::Sent;
}
void RoomSocket::Receive(uint64_t generation, const QString& text) {
    if (!active_ || generation != generation_) return;
    if (text == "v2:pong") {
        if (!awaitingPong_) return;
        awaitingPong_ = false; deadline_.stop();
        heartbeat_.start(29000 + QRandomGenerator::global()->bounded(2001));
        return;
    }
    const auto bytes = text.toUtf8();
    const auto decoded = wire::ValidateServerEvent(bytes, config_.directory);
    if (!decoded.ok || (!config_.directory && decoded.message["roomId"] != config_.roomId)) {
        Drop(Error::Protocol, true); return;
    }
    const auto type = decoded.message["type"].toString();
    const auto result = cache_.Receive(cacheGeneration_, bytes);
    if (result == wire::StateSubscription::Result::Invalid) { Drop(Error::Protocol, true); return; }
    if (result == wire::StateSubscription::Result::Closed) {
        Stop(); Emit(EventKind::Closed, Error::None, decoded.message); return;
    }
    if (result == wire::StateSubscription::Result::Resync) {
        ready_ = false;
        heartbeat_.stop(); awaitingPong_ = false;
        QJsonObject command{{"v", 2}, {"type", "state.resync"}, {"payload", QJsonObject{}}};
        if (!config_.directory) command["roomId"] = config_.roomId;
        if (!Write(QJsonDocument(command).toJson(QJsonDocument::Compact))) { Drop(Error::Backpressure, true); return; }
        deadline_.start(10000);
        return;
    }
    if (result == wire::StateSubscription::Result::Applied) {
        if (!config_.expectedRole.isEmpty()) {
            for (auto member : cache_.Payload()["members"].toArray()) {
                const auto item = member.toObject();
                if (item["peerId"] == config_.selfPeerId && item["role"] != config_.expectedRole) {
                    Drop(Error::Protocol, true); return;
                }
            }
        }
        ready_ = true;
        if (type == "state.snapshot") {
            retries_ = 0; awaitingPong_ = false; deadline_.stop();
            heartbeat_.start(29000 + QRandomGenerator::global()->bounded(2001));
        }
        Emit(EventKind::Snapshot, Error::None, cache_.Payload());
        return;
    }
    if (type.startsWith("state.")) return;
    if (!ready_ || !Allowed(decoded.message, true)) { Drop(Error::Protocol, true); return; }
    Emit(type == "command.result" ? EventKind::Result : EventKind::Signal, Error::None, decoded.message);
}
}
