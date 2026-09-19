#include "room/qt/RoomAdmission.h"
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <chrono>
#include <iostream>
#include <thread>
using screenshare::room::qt::RoomAdmission;
using namespace std::chrono_literals;
void Check(bool ok) { if (!ok) throw std::runtime_error("Admission invariant failed"); }
QByteArray Bytes(QJsonObject value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
template<class Predicate> void Wait(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!predicate()) {
        Check(std::chrono::steady_clock::now() < deadline);
        QCoreApplication::processEvents(); std::this_thread::sleep_for(1ms);
    }
}
void Respond(QTcpSocket* peer, int status, QByteArray body, QByteArray extra = {}) {
    peer->write("HTTP/1.1 " + QByteArray::number(status) + " Result\r\nContent-Type: application/json\r\nContent-Length: " +
        QByteArray::number(body.size()) + "\r\nConnection: close\r\n" + extra + "\r\n" + body);
    peer->disconnectFromHost();
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTcpServer server;
        Check(server.listen(QHostAddress::LocalHost, 0));
        int requests = 0;
        QByteArray lastHeaders, lastBody;
        std::function<void(QTcpSocket*)> response;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto* peer = server.nextPendingConnection();
            QObject::connect(peer, &QTcpSocket::disconnected, peer, &QObject::deleteLater);
            auto buffer = std::make_shared<QByteArray>();
            auto handled = std::make_shared<bool>(false);
            QObject::connect(peer, &QTcpSocket::readyRead, &server, [&, peer, buffer, handled] {
                *buffer += peer->readAll();
                if (*handled) return;
                const auto end = buffer->indexOf("\r\n\r\n");
                if (end < 0) return;
                qint64 length = -1;
                for (auto line : buffer->left(end).split('\n'))
                    if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toLongLong();
                Check(length >= 0 && length < 16384);
                if (buffer->size() < end + 4 + length) return;
                *handled = true; ++requests;
                lastHeaders = buffer->left(end); lastBody = buffer->mid(end + 4, length);
                Check(lastHeaders.startsWith("POST "));
                response(peer);
            });
        });
        RoomAdmission admission(true);
        RoomAdmission::Request join;
        join.origin = QUrl("http://127.0.0.1:" + QString::number(server.serverPort()));
        join.roomId = "room"; join.nickname = "  Player  "; join.password = "private-password";
        auto wait = [&](auto& future) { Wait([&] { return future.wait_for(0ms) == std::future_status::ready; }); return future.get(); };
        auto valid = QJsonObject{{"v", 2}, {"roomId", "room"}, {"peerId", "peer"}, {"role", "viewer"},
            {"token", QString::fromLatin1(QByteArray(32, 'k').toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))}};
        auto issue = [&](QJsonObject value, int status = 200) {
            response = [value, status](auto* peer) { Respond(peer, status, Bytes(value), "Set-Cookie: ignored=value\r\n"); };
            auto pending = admission.Start(join);
            return wait(pending);
        };
        auto created = join;
        created.create = true; created.roomId.clear(); created.name = "  Friends  ";
        auto host = valid; host["role"] = "host";
        response = [host](auto* peer) { Respond(peer, 201, Bytes(host)); };
        auto start = admission.Start(created);
        auto result = wait(start);
        Check(result.error == RoomAdmission::Error::None && result.membership && !result.outcomeUnconfirmed);
        Check(result.membership->origin.scheme() == "ws" && result.membership->expectedRole == "host");
        Check(lastHeaders.startsWith("POST /v2/rooms HTTP/") && !lastHeaders.contains("private-password"));
        const auto body = QJsonDocument::fromJson(lastBody).object();
        Check(body["nickname"] == "Player" && body["policy"].toObject()["name"] == "Friends");
        Check(body["password"] == "private-password");
        result = issue(valid);
        Check(result.error == RoomAdmission::Error::None && result.membership->expectedRole == "viewer");
        Check(lastHeaders.startsWith("POST /v2/rooms/room/join HTTP/"));
        const auto initialRequests = requests;
        auto invalid = join; invalid.origin = QUrl("http://example.com");
        Check(admission.Start(invalid).get().error == RoomAdmission::Error::InvalidRequest);
        invalid = join; invalid.password = QString(65, QChar(0x00e9));
        Check(admission.Start(invalid).get().error == RoomAdmission::Error::InvalidRequest);
        invalid = join; invalid.roomId = "../other";
        Check(admission.Start(invalid).get().error == RoomAdmission::Error::InvalidRequest);
        Check(requests == initialRequests);
        for (auto field : {"token", "roomId", "role"}) {
            auto bad = valid; bad[field] = "wrong";
            result = issue(bad);
            Check(result.error == RoomAdmission::Error::Unconfirmed && result.outcomeUnconfirmed && !result.membership);
        }
        auto extra = valid; extra["secret"] = "must-not-escape";
        Check(issue(extra).error == RoomAdmission::Error::Unconfirmed);
        Check(issue({{"v", 2}, {"error", "forbidden"}}, 403).error == RoomAdmission::Error::Forbidden);
        Check(issue({{"v", 2}, {"error", "full"}}, 409).error == RoomAdmission::Error::Full);
        Check(issue({{"v", 2}, {"error", "rate_limited"}}, 429).error == RoomAdmission::Error::RateLimited);
        Check(issue({{"v", 2}, {"error", "full"}}, 500).error == RoomAdmission::Error::Unconfirmed);
        Check(!lastHeaders.toLower().contains("cookie:"));
        const auto beforeRedirect = requests;
        response = [&](auto* peer) { Respond(peer, 307, "{}", "Location: /redirected\r\n"); };
        start = admission.Start(join);
        Check(wait(start).error == RoomAdmission::Error::Unconfirmed && requests == beforeRedirect + 1);
        response = [](auto* peer) { Respond(peer, 200, QByteArray(17000, 'x')); };
        start = admission.Start(join);
        Check(wait(start).error == RoomAdmission::Error::Unconfirmed);
        response = [](auto*) {}; // Accepted request with no response.
        auto before = requests;
        start = admission.Start(join);
        Check(admission.Start(join).get().error == RoomAdmission::Error::Busy);
        Wait([&] { return requests == before + 1; });
        admission.Cancel();
        result = start.get();
        Check(result.error == RoomAdmission::Error::Cancelled && result.outcomeUnconfirmed);
        // A cancelled reply cannot complete or corrupt the next operation.
        Check(issue(valid).error == RoomAdmission::Error::None);
        response = [](auto*) {};
        before = requests;
        start = admission.Start(join);
        result = wait(start); // Exercise the real ten-second admission deadline.
        Check(result.error == RoomAdmission::Error::Unconfirmed && requests == before + 1);
        admission.Cancel();
        std::cout << "{\"passed\":true,\"mode\":\"live-room-admission\",\"requests\":" << requests << "}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
