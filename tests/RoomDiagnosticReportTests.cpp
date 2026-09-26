#include "shared/RoomDiagnosticReport.h"
#include "shared/RoomLaunch.h"
#include "media/webrtc/TransportSendRate.h"
#include "media/webrtc/RoomIceConfiguration.h"
#include "codec/DecoderLowLatency.h"
#include "api/make_ref_counted.h"
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
        using namespace screenshare::media;
        // Missing old-OS codec interfaces/properties are compatible. Supported
        // properties that fail to apply must retain their real native error.
        unsigned calls = 0;
        auto supported = [&] { ++calls; return S_OK; };
        auto set = [&] { ++calls; return S_OK; };
        auto get = [&](VARIANT* value) { ++calls; value->vt = VT_UI4; value->ulVal = 1; return S_OK; };
        Check(!screenshare::ConfigureDecoderLowLatencyProperty(E_NOINTERFACE, supported, set, get) && calls == 0);
        Check(!screenshare::ConfigureDecoderLowLatencyProperty(S_OK, [] { return S_FALSE; }, set, get) && calls == 0);
        Check(!screenshare::ConfigureDecoderLowLatencyProperty(S_OK, [] { return E_PROP_ID_UNSUPPORTED; }, set, get));
        Check(screenshare::ConfigureDecoderLowLatencyProperty(S_OK, supported, set, get) && calls == 3);
        bool nativeFailure = false;
        try { screenshare::ConfigureDecoderLowLatencyProperty(S_OK, supported, [] { return E_ACCESSDENIED; }, get); }
        catch (const screenshare::MediaOperationError& error) { nativeFailure = error.code == E_ACCESSDENIED; }
        Check(nativeFailure);
        nativeFailure = false;
        try { screenshare::ConfigureDecoderLowLatencyProperty(S_OK, supported, set, [](VARIANT* value) { value->vt = VT_UI4; value->ulVal = 0; return S_OK; }); }
        catch (const screenshare::MediaOperationError&) { nativeFailure = true; }
        Check(nativeFailure);

        auto defaults = RoomIceConfiguration({}, true);
        Check(defaults.servers.size() == 1 && defaults.servers[0].urls.size() == 2);
        Check(RoomIceConfiguration({}, false).servers.empty());
        auto relay = defaults; relay.type = webrtc::PeerConnectionInterface::kRelay; relay.servers.clear();
        Check(RoomIceConfiguration(relay, true).servers.empty());
        auto custom = defaults; custom.servers[0].urls = {"turn:private-server-secret:3478"};
        Check(RoomIceConfiguration(custom, true).servers[0].urls == custom.servers[0].urls);

        DiagnosticHistory history(4); history.Event("start");
        const auto pinned = history.Read();
        for (int i = 0; i < 10; ++i) history.Event("sample", i);
        Check(pinned.records->size() == 1 && pinned.records->at(0).labels.at("event") == "start");
        Check(history.Read().records->size() == 4 && history.Read().omitted == 7);
        Check(history.Read().records->front().labels.at("event") == "start");
        DiagnosticHistory failures(4, false);
        failures.Event("decoder-frame-failed", E_FAIL);
        for (int i = 0; i < 10; ++i) failures.Event("sample", i);
        Check(failures.Read().records->front().labels.at("event") == "sample");
        Check(failures.Read().firstFailure && failures.Read().firstFailure->labels.at("event") == "decoder-frame-failed");

        auto rtc = webrtc::RTCStatsReport::Create(webrtc::Timestamp::Millis(1000));
        auto local = std::make_unique<webrtc::RTCLocalIceCandidateStats>("private-candidate-secret", rtc->timestamp());
        local->address = "203.0.113.42"; local->username_fragment = "private-ufrag-secret";
        local->url = "turn:private-server-secret:3478"; local->candidate_type = "srflx"; local->protocol = "udp";
        auto remote = std::make_unique<webrtc::RTCRemoteIceCandidateStats>("private-remote-secret", rtc->timestamp());
        remote->address = "192.168.9.42"; remote->candidate_type = "host"; remote->protocol = "udp";
        auto pair = std::make_unique<webrtc::RTCIceCandidatePairStats>("private-pair-secret", rtc->timestamp());
        pair->local_candidate_id = local->id(); pair->remote_candidate_id = remote->id();
        pair->state = "succeeded"; pair->requests_sent = 5; pair->responses_received = 4;
        pair->current_round_trip_time = 0.015;
        auto transport = std::make_unique<webrtc::RTCTransportStats>("private-transport-secret", rtc->timestamp());
        transport->selected_candidate_pair_id = pair->id(); transport->dtls_state = "connected";
        transport->ice_local_username_fragment = "private-ice-secret";
        auto video = std::make_unique<webrtc::RTCInboundRtpStreamStats>("private-rtp-secret", rtc->timestamp());
        video->kind = "video"; video->frames_received = 10; video->frames_decoded = 0; video->bytes_received = 1234;
        video->decoder_implementation = "private-decoder-secret";
        rtc->AddStats(std::move(local)); rtc->AddStats(std::move(remote)); rtc->AddStats(std::move(pair));
        rtc->AddStats(std::move(transport)); rtc->AddStats(std::move(video));
        const auto sample = TransportDiagnostics(*rtc);
        Check(sample.labels.at("localCandidateType") == "srflx" && sample.numbers.at("rttMs") == 15);
        Check(sample.numbers.at("videoReceiveFramesDecoded") == 0 && sample.numbers.at("connectivityRequestsSent") == 5);
        const auto transportBytes = QJsonDocument(DiagnosticRecordJson(sample)).toJson();
        Check(!transportBytes.contains("secret") && !transportBytes.contains("203.0.113.42") && !transportBytes.contains("192.168.9.42"));

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
        screenshare::media::PeerConnectionStatus connection;
        connection.peerId = peer.peerId; connection.negotiated = true; connection.retained = true;
        connection.recovery.state = screenshare::media::PeerLifecycleState::Failed;
        connection.recovery.failure = screenshare::media::PeerLifecycleFailure::DirectConnectTimeout;
        connection.localCandidates = 3; connection.remoteCandidates = 0;
        connection.events = history.Read();
        DiagnosticHistory transportHistory(60); transportHistory.Add(sample); connection.transportHistory = transportHistory.Read();
        status.stream.connections.push_back(connection);
        screenshare::input::Status input; input.peer = peer.peerId; input.reason = screenshare::input::Reason::Backpressure;
        input.transportBlocked = true; input.reliableQueued = 2;
        auto report = RoomDiagnosticReport(status, {input});
        Check(!report["appVersion"].toString().isEmpty());
        const auto bytes = QJsonDocument(report).toJson();
        Check(!bytes.contains("secret") && !bytes.contains("123456"));
        const auto media = report["peers"].toArray()[0].toObject();
        const auto control = report["input"].toArray()[0].toObject();
        Check(media["peerId"] == control["peer"] && media["transportSendBps"].isNull());
        const auto failed = report["connections"].toArray()[0].toObject();
        Check(failed["peerId"] == media["peerId"] && failed["state"] == "failed" &&
            failed["failure"] == "connect-timeout" && failed["negotiated"].toBool() &&
            failed["retained"].toBool() && failed["localCandidates"].toInt() == 3 && failed["remoteCandidates"].toInt() == 0);
        auto viewer = status; viewer.stream.peers.clear();
        Check(RoomDiagnosticReport(viewer)["connections"].toArray().size() == 1);
        Check(!report["platform"].toObject()["kernelVersion"].toString().isEmpty());
        Check(report["build"].toObject()["executableSha256"].toString().size() == 64);
        Check(failed["events"].toObject()["omitted"].toInteger() == 7);
        Check(failed["transportHistory"].toObject()["records"].toArray()[0].toObject()["videoReceiveFramesDecoded"].toInt(-1) == 0);
        auto counterpart = status; counterpart.peerId = peer.peerId;
        Check(RoomDiagnosticReport(counterpart)["selfCorrelation"] == failed["correlation"]);
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
        Check(!WriteRoomDiagnosticReport(path, {{"oversized", QString(32 * 1024 * 1024, 'x')}}));
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
