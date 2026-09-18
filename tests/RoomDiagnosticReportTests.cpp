#include "shared/RoomDiagnosticReport.h"
#include "shared/RoomLaunch.h"
#include <QTemporaryDir>
#include <QFile>
#include <iostream>
#include <source_location>
void Check(bool ok, std::source_location where = std::source_location::current()) {
    if (!ok) throw std::runtime_error("Diagnostic report check failed at " + std::to_string(where.line()));
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        screenshare::v2::RoomStatus status;
        status.roomId = "private-room-secret"; status.peerId = "private-self-secret";
        status.policy.name = "private-title-secret";
        status.members.push_back({"private-peer-secret", "private-nickname-secret", false});
        status.audio.selected.deviceId = L"private-audio-secret";
        status.playback.selected.deviceId = L"private-playback-secret";
        screenshare::v2::PeerStreamStatus peer; peer.peerId = "private-peer-secret";
        peer.transportSampleStale = true; peer.transportSendBps = 123456;
        peer.sender.targetVideoBps = 123456; peer.sender.meanPacketSendDelayMs = 123456;
        peer.sender.framesEncoded = 123456; peer.sender.keyFramesEncoded = 123456;
        status.stream.peers.push_back(peer);
        screenshare::input::Status input; input.peer = peer.peerId; input.reason = screenshare::input::Reason::Backpressure;
        input.transportBlocked = true; input.reliableQueued = 2;
        auto report = RoomDiagnosticReport(status, {input});
        Check(!report["appVersion"].toString().isEmpty());
        const auto bytes = QJsonDocument(report).toJson();
        Check(!bytes.contains("secret") && !bytes.contains("123456"));
        const auto media = report["peers"].toArray()[0].toObject();
        const auto control = report["input"].toArray()[0].toObject();
        Check(media["peerId"] == control["peer"] && media["transportSendBps"].isNull());
        const auto staleSender = media["sender"].toObject();
        for (const auto* key : {"targetVideoBps", "meanPacketSendDelayMs", "framesEncoded", "keyFramesEncoded"})
            Check(staleSender[key].isNull());
        auto fresh = status;
        fresh.stream.peers[0].transportSampleStale = false;
        fresh.stream.peers[0].sender.targetVideoBps = 0;
        fresh.stream.peers[0].sender.meanPacketSendDelayMs = 0;
        const auto freshSender = RoomDiagnosticReport(fresh)["peers"].toArray()[0].toObject()["sender"].toObject();
        Check(freshSender["targetVideoBps"].isDouble() && freshSender["targetVideoBps"].toDouble() == 0 &&
            freshSender["meanPacketSendDelayMs"].isDouble() && freshSender["meanPacketSendDelayMs"].toDouble() == 0);
        Check(control["localQueueWaitUs"].isNull() && control["reasonName"] == "backpressure");
        Check(!report["externalLatencyVerified"].toBool());
        QTemporaryDir directory; Check(directory.isValid());
        const auto path = directory.filePath("nested/report.json");
        Check(WriteRoomDiagnosticReport(path, report));
        QFile saved(path); Check(saved.open(QIODevice::ReadOnly)); Check(saved.readAll() == bytes); saved.close();
        Check(!WriteRoomDiagnosticReport(directory.path(), report));
        Check(!WriteRoomDiagnosticReport(path, {{"oversized", QString(1024 * 1024, 'x')}}));
        Check(saved.open(QIODevice::ReadOnly)); Check(saved.readAll() == bytes);
        const auto config = ParseRoomCommand({"--backend", "v2", "--signal-server", "https://example.com", "--create-room", "--report", path});
        Check(config.reportFile == path);
        QJsonObject jsonConfig{{"origin", "https://example.com"}, {"host", true}, {"reportFile", path}};
        Check(ParseRoomSessionConfig(jsonConfig).reportFile == path);
        for (const auto& invalid : {QJsonValue(" "), QJsonValue(QString(4097, 'a')), QJsonValue(QString(QChar(0))), QJsonValue(42)}) {
            jsonConfig["reportFile"] = invalid;
            bool invalidPath = false;
            try { ParseRoomSessionConfig(jsonConfig); } catch (const std::invalid_argument&) { invalidPath = true; }
            Check(invalidPath);
        }
        bool rejected = false;
        try { ParseRoomCommand({"--backend", "v2", "--signal-server", "https://example.com", "--create-room", "--report", " "}); }
        catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected);
        std::cout << "{\"passed\":true,\"redacted\":true}\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
