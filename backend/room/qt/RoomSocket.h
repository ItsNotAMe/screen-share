#pragma once
#include "room/protocol/StateSubscription.h"
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <optional>
class QWebSocket;

namespace screenshare::room::qt {
// Private Qt boundary. Own on the networking event loop, never on signaling.
// Events must be queued to consumers with their generation; callbacks must not
// destroy/reenter this object or throw. Admission and media connection IDs are external.
class RoomSocket final : public QObject {
public:
    enum class EventKind { Connecting, Connected, Snapshot, Signal, Result, Closed, Reconnecting, Error };
    enum class Error { None, Configuration, Transport, Protocol, Backpressure, Timeout };
    struct Event { EventKind kind; uint64_t generation; Error error = Error::None; QJsonObject value;
        std::optional<uint64_t> revision; }; // Snapshot revision travels with the immutable payload.
    struct Config {
        QUrl origin;
        QString roomId, selfPeerId;
        QString expectedRole; // Required for room sockets; supplied by admission.
        QByteArray token;
        bool directory = false;
    };
    enum class SendResult { Sent, NotReady, Invalid, Forbidden, Backpressure };
    using Notify = std::function<void(const Event&)>;
    // Plain loopback is an explicit diagnostic-only opt-in; remote ws is never allowed.
    explicit RoomSocket(Notify, bool allowPlainLoopback = false, QObject* parent = nullptr);
    ~RoomSocket() override;
    bool Start(Config);
    void Stop();
    SendResult Send(const QByteArray& command);
    uint64_t generation() const { return generation_; }
private:
    void CheckThread() const;
    void Open();
    void Drop(Error, bool retry);
    void Receive(uint64_t, const QString&);
    bool Write(const QByteArray&);
    bool Allowed(const QJsonObject&, bool incoming) const;
    void Emit(EventKind, Error = Error::None, QJsonObject = {});
    Config config_;
    Notify notify_;
    bool allowPlainLoopback_, active_ = false, ready_ = false, awaitingPong_ = false;
    uint64_t generation_ = 0, cacheGeneration_ = 0;
    unsigned retries_ = 0;
    QWebSocket* socket_ = nullptr;
    QTimer reconnect_, heartbeat_, deadline_;
    wire::StateSubscription cache_;
};
}
