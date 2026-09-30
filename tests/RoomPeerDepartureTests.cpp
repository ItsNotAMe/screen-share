#include "room/qt/RoomSessionCoordinator.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QWebSocketServer>
#include <QWebSocket>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
using namespace screenshare::room::qt;
using namespace screenshare::media;
using namespace std::chrono_literals;
namespace {
void Check(bool ok) { if (!ok) throw std::runtime_error("Viewer departure disrupted room transport"); }
QByteArray Bytes(const QJsonObject& value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
template<class Predicate> void Wait(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (!predicate()) {
        Check(std::chrono::steady_clock::now() < deadline);
        QCoreApplication::processEvents(); std::this_thread::sleep_for(1ms);
    }
}
}
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        QWebSocketServer server("departure-test", QWebSocketServer::NonSecureMode);
        Check(server.listen(QHostAddress::LocalHost, 0));
        std::unique_ptr<QWebSocket> socket;
        std::vector<QJsonObject> received;
        QObject::connect(&server, &QWebSocketServer::newConnection, &server, [&] {
            Check(!socket); socket.reset(server.nextPendingConnection());
            QObject::connect(socket.get(), &QWebSocket::textMessageReceived, &server, [&](const auto& text) {
                received.push_back(QJsonDocument::fromJson(text.toUtf8()).object());
            });
        });
        RoomNetwork network(true);
        SignalingExecutor executor;
        std::unique_ptr<RoomSessionCoordinator> coordinator;
        std::atomic<unsigned> revision{0}, errors{0}, advances{0};
        auto run = [&](auto work) { Check(executor.Post(work).get().error == ExecutorError::None); };
        auto stop = [&] {
            std::shared_future<void> stopped;
            run([&] { stopped = coordinator->Stop(); }); stopped.get();
            run([&] { coordinator.reset(); }); executor.Stop();
        };
        try {
            RoomSocket::Config config;
            config.origin = QUrl("ws://127.0.0.1:" + QString::number(server.serverPort()));
            config.roomId = "room"; config.selfPeerId = "host"; config.expectedRole = "host"; config.token = QByteArray(43, 'x');
            std::future<bool> opened;
            run([&] {
                coordinator = std::make_unique<RoomSessionCoordinator>(executor, network, [&] { ++advances; });
                opened = coordinator->Open(1, config, [&](const auto& event) {
                    if (event.kind == RoomSocket::EventKind::Snapshot) revision = unsigned(*event.revision);
                    if (event.kind == RoomSocket::EventKind::Error) ++errors;
                });
            });
            Check(opened.get()); Wait([&] { return bool(socket); });
            QJsonArray members{
                QJsonObject{{"peerId", "host"}, {"nickname", "Host"}, {"role", "host"}, {"status", "connected"}},
                QJsonObject{{"peerId", "leaving"}, {"nickname", "Leaving"}, {"role", "viewer"}, {"status", "connected"}},
                QJsonObject{{"peerId", "healthy"}, {"nickname", "Healthy"}, {"role", "viewer"}, {"status", "connected"}}};
            QJsonObject snapshot{{"v", 2}, {"type", "state.snapshot"}, {"roomId", "room"}, {"revision", 1},
                {"payload", QJsonObject{{"selfPeerId", "host"}, {"status", "open"}, {"members", members},
                    {"policy", QJsonObject{{"name", "Room"}, {"visibility", "public"}, {"viewerLimit", 4}, {"passwordProtected", false}}}}}};
            socket->sendTextMessage(QString::fromUtf8(Bytes(snapshot)));
            Wait([&] { return revision == 1; });
            for (unsigned next = 2; next <= 3; ++next) {
                QJsonObject change;
                if (next == 2) {
                    auto leaving = members[1].toObject(); leaving["status"] = "reconnecting";
                    change = {{"op", "member.upsert"}, {"member", leaving}};
                } else change = {{"op", "member.remove"}, {"peerId", "leaving"}};
                socket->sendTextMessage(QString::fromUtf8(Bytes({{"v", 2}, {"type", "state.delta"}, {"roomId", "room"},
                    {"revision", int(next)}, {"payload", change}})));
                Wait([&] { return revision == next; });
                const auto before = received.size();
                run([&] {
                    for (const auto* target : {"leaving", "healthy"}) {
                        Check(coordinator->Send(1, Bytes({{"v", 2}, {"type", "signal.candidate"}, {"roomId", "room"},
                            {"toPeerId", target}, {"connectionId", "connection"},
                            {"payload", QJsonObject{{"candidate", "candidate:test"}, {"sdpMid", "0"}, {"sdpMLineIndex", 0}}}})));
                    }
                });
                Wait([&] { return received.size() > before; });
                const auto tick = advances.load(); Wait([&] { return advances >= tick + 2; });
                Check(errors == 0 && received.size() == before + 1 && received.back()["toPeerId"] == "healthy");
                run([&] { Check(!coordinator->failed()); });
            }
            stop();
        } catch (...) { if (coordinator) stop(); throw; }
        std::cout << "{\"passed\":true,\"viewer_departure_send_isolated\":true}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
