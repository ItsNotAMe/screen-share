#include "room/qt/RoomSocket.h"
#include <QCoreApplication>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QThread>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
using screenshare::room::qt::RoomSocket;
using namespace std::chrono_literals;
void Check(bool ok) { if (!ok) throw std::runtime_error("Live room socket invariant failed"); }
QByteArray Bytes(QJsonObject value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
QJsonObject Snapshot(int revision = 1) {
    return {{"v", 2}, {"type", "state.snapshot"}, {"roomId", "room"}, {"revision", revision},
        {"payload", QJsonObject{{"selfPeerId", "viewer"}, {"status", "open"},
            {"policy", QJsonObject{{"name", "Room"}, {"visibility", "public"}, {"viewerLimit", 4}, {"passwordProtected", false}}},
            {"members", QJsonArray{
                QJsonObject{{"peerId", "host"}, {"nickname", "Host"}, {"role", "host"}, {"status", "connected"}},
                QJsonObject{{"peerId", "viewer"}, {"nickname", "Viewer"}, {"role", "viewer"}, {"status", "connected"}}}}}}};
}
template<class Predicate> void Wait(Predicate predicate, std::chrono::milliseconds timeout = 4000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        Check(std::chrono::steady_clock::now() < deadline);
        QCoreApplication::processEvents(); std::this_thread::sleep_for(1ms);
    }
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QWebSocketServer server("room-test", QWebSocketServer::NonSecureMode);
        Check(server.listen(QHostAddress::LocalHost, 0));
        std::vector<std::unique_ptr<QWebSocket>> peers;
        std::vector<QString> received;
        QObject::connect(&server, &QWebSocketServer::newConnection, &server, [&] {
            auto socket = std::unique_ptr<QWebSocket>(server.nextPendingConnection());
            QObject::connect(socket.get(), &QWebSocket::textMessageReceived, &server,
                [&](const QString& text) { received.push_back(text); });
            peers.push_back(std::move(socket));
        });
        QThread network;
        QObject context;
        context.moveToThread(&network); network.start();
        std::unique_ptr<RoomSocket> client;
        std::mutex mutex;
        std::vector<RoomSocket::Event> events;
        auto run = [&](auto task) {
            std::exception_ptr error;
            Check(QMetaObject::invokeMethod(&context, [&] { try { task(); } catch (...) { error = std::current_exception(); } }, Qt::BlockingQueuedConnection));
            if (error) std::rethrow_exception(error);
        };
        auto count = [&](RoomSocket::EventKind kind) {
            std::lock_guard lock(mutex);
            return std::count_if(events.begin(), events.end(), [&](const auto& e) { return e.kind == kind; });
        };
        auto cleanup = [&] {
            run([&] { client.reset(); context.moveToThread(app.thread()); });
            network.quit(); network.wait();
        };
        try {
            run([&] {
                client = std::make_unique<RoomSocket>([&](const auto& event) {
                    Check(QThread::currentThread() == &network);
                    std::lock_guard lock(mutex); events.push_back(event);
                }, true);
            });
            RoomSocket::Config config;
            config.origin = QUrl("ws://127.0.0.1:" + QString::number(server.serverPort()));
            config.roomId = "room"; config.selfPeerId = "viewer"; config.token = QByteArray(43, 'x');
            run([&] {
                auto invalid = config; invalid.origin = QUrl("ws://example.com");
                Check(!client->Start(invalid));
                invalid = config; invalid.token = "bad\r\nheader";
                Check(!client->Start(invalid));
                Check(client->Start(config));
                Check(client->Send("{}") == RoomSocket::SendResult::NotReady);
            });
            Wait([&] { return peers.size() == 1; });
            Check(peers[0]->request().rawHeader("Authorization") == "Bearer " + config.token);
            Check(peers[0]->requestUrl().path() == "/v2/rooms/room/events" && !peers[0]->requestUrl().hasQuery());
            peers[0]->sendTextMessage(QString::fromUtf8(Bytes(Snapshot())));
            Wait([&] { return count(RoomSocket::EventKind::Snapshot) == 1; });
            QJsonObject profile{{"v", 2}, {"type", "profile.update"}, {"roomId", "room"}, {"requestId", "request"},
                {"payload", QJsonObject{{"nickname", "  Player  "}, {"expectedRevision", 1}}}};
            run([&] { Check(client->Send(Bytes(profile)) == RoomSocket::SendResult::Sent); });
            Wait([&] { return received.size() == 1; });
            Check(QJsonDocument::fromJson(received[0].toUtf8()).object()["payload"].toObject()["nickname"] == "Player");
            QJsonObject signal{{"v", 2}, {"type", "signal.answer"}, {"roomId", "room"},
                {"connectionId", "connection"}, {"toPeerId", "host"}, {"payload", QJsonObject{{"sdp", "test-sdp"}}}};
            run([&] {
                Check(client->Send(Bytes(signal)) == RoomSocket::SendResult::Sent);
                auto forbidden = signal; forbidden["type"] = "signal.offer";
                Check(client->Send(Bytes(forbidden)) == RoomSocket::SendResult::Forbidden);
                forbidden = signal; forbidden["toPeerId"] = "unknown";
                Check(client->Send(Bytes(forbidden)) == RoomSocket::SendResult::Forbidden);
                auto wrongRoom = signal; wrongRoom["roomId"] = "other";
                Check(client->Send(Bytes(wrongRoom)) == RoomSocket::SendResult::Invalid);
            });
            Wait([&] { return received.size() == 2; });
            signal["type"] = "signal.offer"; signal["fromPeerId"] = "host"; signal["toPeerId"] = "viewer";
            peers[0]->sendTextMessage(QString::fromUtf8(Bytes(signal)));
            Wait([&] { return count(RoomSocket::EventKind::Signal) == 1; });
            QJsonObject gap{{"v", 2}, {"type", "state.delta"}, {"roomId", "room"}, {"revision", 3},
                {"payload", QJsonObject{{"op", "host.status"}, {"status", "reconnecting"}}}};
            peers[0]->sendTextMessage(QString::fromUtf8(Bytes(gap)));
            gap["revision"] = 4;
            peers[0]->sendTextMessage(QString::fromUtf8(Bytes(gap)));
            Wait([&] { return received.size() == 3; });
            Check(QJsonDocument::fromJson(received[2].toUtf8()).object()["type"] == "state.resync");
            run([&] { Check(client->Send(Bytes(profile)) == RoomSocket::SendResult::NotReady); });
            peers[0]->sendTextMessage(QString::fromUtf8(Bytes(Snapshot(4))));
            Wait([&] { return count(RoomSocket::EventKind::Snapshot) == 2; });
            Check(received.size() == 3);
            // Actual timer-driven hibernation ping; no test clock or manual timeout.
            Wait([&] { return received.size() == 4; }, 33000ms);
            Check(received.back() == "v2:ping");
            // Withhold pong: the client must reconnect and authenticate again.
            Wait([&] { return peers.size() == 2; }, 13000ms);
            Check(peers[1]->request().rawHeader("Authorization") == "Bearer " + config.token);
            peers[1]->sendTextMessage(QString::fromUtf8(Bytes(Snapshot(4))));
            Wait([&] { return count(RoomSocket::EventKind::Snapshot) == 3; });
            const auto before = count(RoomSocket::EventKind::Error);
            signal["toPeerId"] = "other";
            peers[1]->sendTextMessage(QString::fromUtf8(Bytes(signal)));
            Wait([&] { return count(RoomSocket::EventKind::Error) > before; });
            Check(count(RoomSocket::EventKind::Signal) == 1);
            RoomSocket::Config directory;
            directory.origin = config.origin; directory.directory = true;
            run([&] { Check(client->Start(directory)); });
            Wait([&] { return peers.size() == 3; });
            Check(peers[2]->request().rawHeader("Authorization").isEmpty());
            Check(peers[2]->requestUrl().path() == "/v2/directory/events");
            QJsonObject listing{{"v", 2}, {"type", "state.snapshot"}, {"revision", 1}, {"payload", QJsonObject{{"rooms", QJsonArray{}}}}};
            peers[2]->sendTextMessage(QString::fromUtf8(Bytes(listing)));
            Wait([&] { return count(RoomSocket::EventKind::Snapshot) == 4; });
            const auto callbacks = count(RoomSocket::EventKind::Snapshot);
            run([&] { client->Stop(); });
            peers[2]->sendTextMessage(QString::fromUtf8(Bytes(listing)));
            QCoreApplication::processEvents();
            Check(count(RoomSocket::EventKind::Snapshot) == callbacks);
            // Keep the server event loop paused while the networking thread
            // writes. The client must reject pressure instead of queuing an
            // unbounded signaling backlog.
            run([&] { Check(client->Start(config)); });
            Wait([&] { return peers.size() == 4; });
            peers[3]->sendTextMessage(QString::fromUtf8(Bytes(Snapshot())));
            Wait([&] { return count(RoomSocket::EventKind::Snapshot) == callbacks + 1; });
            run([&] {
                auto large = signal;
                large.remove("fromPeerId"); large["type"] = "signal.answer"; large["toPeerId"] = "host";
                large["payload"] = QJsonObject{{"sdp", QString(50 * 1024, 'x')}};
                const auto message = Bytes(large);
                auto result = RoomSocket::SendResult::Sent;
                for (int i = 0; i < 2048 && result == RoomSocket::SendResult::Sent; ++i) result = client->Send(message);
                Check(result == RoomSocket::SendResult::Backpressure);
                client->Stop();
            });
            cleanup();
        } catch (...) { cleanup(); throw; }
        std::cout << "{\"passed\":true,\"mode\":\"live-room-websocket\",\"connections\":" << peers.size() << "}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
