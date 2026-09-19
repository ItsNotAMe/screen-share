#include "room/qt/RoomAdmission.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
using screenshare::room::qt::RoomAdmission;
using screenshare::room::qt::RoomSocket;
using namespace std::chrono_literals;
namespace {
void Check(bool ok, const char* stage) { if (!ok) throw std::runtime_error(stage); }
template<class Predicate> void Wait(Predicate predicate, const char* stage) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!predicate()) {
        Check(std::chrono::steady_clock::now() < deadline, stage);
        QCoreApplication::processEvents(); std::this_thread::sleep_for(1ms);
    }
}
struct Observed {
    QJsonObject state;
    std::optional<uint64_t> revision;
    std::vector<RoomSocket::Event> events;
    unsigned errors = 0;
    void Record(const RoomSocket::Event& event) {
        if (event.kind == RoomSocket::EventKind::Snapshot) { state = event.value; revision = event.revision; }
        if (event.kind == RoomSocket::EventKind::Error) ++errors;
        events.push_back(event);
    }
    bool Has(RoomSocket::EventKind kind, const QString& type = {}) const {
        for (const auto& e : events) if (e.kind == kind && (type.isEmpty() || e.value["type"] == type)) return true;
        return false;
    }
};
QByteArray Bytes(QJsonObject value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        Check(argc == 2, "expected loopback service origin");
        const QUrl origin(QString::fromLocal8Bit(argv[1]));
        Check(origin.scheme() == "http" && origin.host() == "127.0.0.1", "test requires numeric loopback");
        RoomAdmission admission(true);
        auto admit = [&](RoomAdmission::Request request) {
            auto future = admission.Start(std::move(request));
            Wait([&] { return future.wait_for(0ms) == std::future_status::ready; }, "admission deadline");
            return future.get();
        };
        Observed directoryState, hostState, viewerState;
        RoomSocket directory([&](const auto& event) { directoryState.Record(event); }, true);
        RoomSocket host([&](const auto& event) { hostState.Record(event); }, true);
        RoomSocket viewer([&](const auto& event) { viewerState.Record(event); }, true);
        RoomSocket::Config directoryConfig;
        directoryConfig.origin = origin; directoryConfig.origin.setScheme("ws"); directoryConfig.directory = true;
        Check(directory.Start(directoryConfig), "directory start");
        Wait([&] { return directoryState.revision.has_value(); }, "directory snapshot");
        Check(directoryState.state["rooms"].toArray().isEmpty(), "initial directory empty");
        RoomAdmission::Request create;
        create.origin = origin; create.create = true; create.nickname = " Host "; create.name = " Native Room "; create.password = "test-only"; create.viewerLimit = 1;
        auto created = admit(create);
        Check(created.error == RoomAdmission::Error::None && created.membership.has_value(), "create");
        auto hostConfig = *created.membership;
        Check(host.Start(hostConfig), "host start");
        Wait([&] { return hostState.revision && directoryState.state["rooms"].toArray().size() == 1; }, "host publication");
        Check(hostState.state["policy"].toObject()["name"] == "Native Room", "canonical room name");
        RoomAdmission::Request join;
        join.origin = origin; join.roomId = hostConfig.roomId; join.nickname = " Viewer "; join.password = "wrong";
        Check(admit(join).error == RoomAdmission::Error::Forbidden, "password rejection");
        join.password = "test-only";
        auto joined = admit(join);
        Check(joined.error == RoomAdmission::Error::None && joined.membership.has_value(), "join");
        auto viewerConfig = *joined.membership;
        Check(viewer.Start(viewerConfig), "viewer start");
        Wait([&] { return viewerState.revision && hostState.state["members"].toArray().size() == 2; }, "member push");
        Check(admit(join).error == RoomAdmission::Error::Full, "capacity rejection");
        auto command = [&](RoomSocket& socket, Observed& observed, QString type, QString id, QJsonObject payload) {
            const auto start = observed.events.size();
            Check(socket.Send(Bytes({{"v", 2}, {"type", type}, {"roomId", hostConfig.roomId}, {"requestId", id}, {"payload", payload}})) == RoomSocket::SendResult::Sent, "command send");
            QJsonObject result;
            Wait([&] { for (size_t i = start; i < observed.events.size(); ++i) if (observed.events[i].value["requestId"] == id) { result = observed.events[i].value["payload"].toObject(); return true; } return false; }, "command result");
            return result;
        };
        const auto priorRevision = *viewerState.revision;
        Check(command(viewer, viewerState, "profile.update", "nickname", {{"nickname", " New Name "}, {"expectedRevision", static_cast<qint64>(priorRevision)}})["status"] == "ok", "nickname update");
        Wait([&] { return hostState.revision && *hostState.revision > priorRevision; }, "nickname push");
        Check(command(host, hostState, "room.update", "stale", {{"name", "Stale"}, {"expectedRevision", static_cast<qint64>(priorRevision)}})["status"] == "conflict", "revision conflict");
        auto relay = [&](RoomSocket& source, Observed& destination, QString type, QString target, QJsonObject payload) {
            const auto start = destination.events.size();
            Check(source.Send(Bytes({{"v", 2}, {"type", type}, {"roomId", hostConfig.roomId}, {"connectionId", "native-offer-1"}, {"toPeerId", target}, {"payload", payload}})) == RoomSocket::SendResult::Sent, "signal send");
            Wait([&] { for (size_t i = start; i < destination.events.size(); ++i) if (destination.events[i].kind == RoomSocket::EventKind::Signal && destination.events[i].value["type"] == type) return true; return false; }, "signal relay");
        };
        relay(host, viewerState, "signal.offer", viewerConfig.selfPeerId, {{"sdp", "v=0\r\n"}});
        relay(viewer, hostState, "signal.answer", hostConfig.selfPeerId, {{"sdp", "v=0\r\n"}});
        relay(host, viewerState, "signal.candidate", viewerConfig.selfPeerId, {{"candidate", "candidate:test"}, {"sdpMid", "0"}, {"sdpMLineIndex", 0}});
        relay(viewer, hostState, "signal.restart_request", hostConfig.selfPeerId, {});
        host.Stop();
        Wait([&] { const auto rooms = directoryState.state["rooms"].toArray(); return !rooms.isEmpty() && rooms[0].toObject()["status"] == "reconnecting"; }, "host disconnect push");
        Check(admit(join).error == RoomAdmission::Error::Closed, "join blocked during host disconnect");
        hostState.revision.reset();
        Check(host.Start(hostConfig), "host reconnect");
        Wait([&] { const auto rooms = directoryState.state["rooms"].toArray(); return hostState.revision && !rooms.isEmpty() && rooms[0].toObject()["status"] == "full"; }, "host reconnect push");
        const auto directoryEvents = directoryState.events.size();
        Check(directory.Send(Bytes({{"v", 2}, {"type", "state.resync"}, {"payload", QJsonObject{}}})) == RoomSocket::SendResult::Sent, "directory resync send");
        Wait([&] { return directoryState.events.size() > directoryEvents; }, "directory resync");
        Check(command(host, hostState, "room.update", "hide", {{"visibility", "unlisted"}, {"expectedRevision", static_cast<qint64>(*hostState.revision)}})["status"] == "ok", "hide room");
        Wait([&] { return directoryState.state["rooms"].toArray().isEmpty(); }, "directory removal push");
        Check(command(host, hostState, "peer.disconnect", "kick", {{"peerId", viewerConfig.selfPeerId}})["status"] == "ok", "kick");
        Wait([&] { return viewerState.Has(RoomSocket::EventKind::Closed) && hostState.state["members"].toArray().size() == 1; }, "kick push");
        Check(command(host, hostState, "peer.leave", "close", {})["status"] == "ok", "host leave");
        Wait([&] { return hostState.Has(RoomSocket::EventKind::Closed); }, "room closure");
        directory.Stop(); viewer.Stop(); host.Stop();
        Check(directoryState.errors + hostState.errors + viewerState.errors == 0, "unexpected native transport errors");
        std::cout << "{\"passed\":true,\"native_worker_integration\":true,\"signaling_messages\":4,\"physical_input\":false}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "Room service test failed: " << error.what() << '\n'; return 1; }
}
