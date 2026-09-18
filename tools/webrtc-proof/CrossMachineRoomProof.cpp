#include "PublicRoomSessionFixture.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSslSocket>
#include <QUrl>
#include "CrossMachineLoadProof.h"

namespace {
// Actual room/media transport; all endpoints remain synthetic and silent.
struct RemoteEvidence : Evidence {
    std::atomic<unsigned> clearFrames{0};
    void OnFrame(const webrtc::VideoFrame& frame) override {
        Evidence::OnFrame(frame);
        const auto pixels = frame.video_frame_buffer()->ToI420();
        if (pixels && pixels->DataY()[pixels->StrideY() * (pixels->height() / 2) + pixels->width() / 2] <= 185)
            ++clearFrames;
    }
};
auto Factory(const std::shared_ptr<RemoteEvidence>& evidence) {
    return [evidence](auto identity, auto send) {
        return std::make_unique<Runtime>(identity, std::move(send), evidence);
    };
}
void Print(const QJsonObject& value) {
    std::cout << QJsonDocument(value).toJson(QJsonDocument::Compact).toStdString() << std::endl;
}
void Host(const std::string& origin, const QString& readyFile) {
    Check(!QFile::exists(readyFile));
    auto evidence = std::make_shared<RemoteEvidence>();
    evidence->localizedInputResponse = true;
    RoomSession host(Factory(evidence));
    RoomOptions options; options.origin = origin; options.host = true;
    options.nickname = "NetworkHost"; options.name = "Two-machine acceptance";
    options.publicRoom = false; options.viewerLimit = 1;
    auto starting = host.Start(options); Check(Get(starting).error == RoomError::None);
    QSaveFile ready(readyFile); Check(ready.open(QIODevice::WriteOnly));
    const auto admission = QJsonDocument(QJsonObject{{"roomId", QString::fromStdString(host.Status().roomId)},
        {"origin", QString::fromStdString(origin)}}).toJson(QJsonDocument::Compact);
    Check(ready.write(admission) == admission.size() && ready.commit());
    // The launcher needs time to transfer the invitation to the other machine.
    const auto joinDeadline = std::chrono::steady_clock::now() + 60s;
    while (host.Status().activePeers != 1) {
        Check(host.Status().phase == RoomPhase::Active && std::chrono::steady_clock::now() < joinDeadline);
        std::this_thread::sleep_for(10ms);
    }
    std::string firstPeer;
    Wait([&] {
        if (!host.Input()) return false;
        for (const auto& peer : host.Input()->Read()) if (peer.ready && peer.requested == screenshare::input::Keyboard) {
            firstPeer = peer.peer;
            return host.Input()->Grant(firstPeer, screenshare::input::Keyboard);
        }
        return false;
    });
    Wait([&] { return evidence->input->applied >= 40 && !evidence->input->pressed; });
    StreamPreferences fixed; fixed.resolution = ResolutionMode::Fixed;
    fixed.preset = StreamPreset::Quality;
    fixed.width = 320; fixed.height = 180; fixed.fps = 20;
    fixed.bitrateMode = SettingMode::Manual; fixed.bitrateLimitBps = 1'000'000;
    auto change = host.UpdateStreamPreferences(fixed); Check(Get(change).error == StreamUpdateError::None);
    Wait([&] {
        for (const auto& peer : host.Status().stream.peers)
            if (peer.peerId != firstPeer && peer.receiver.observation && !peer.receiver.stale &&
                peer.receiver.observation->width == 320 && peer.receiver.observation->framesDecoded >= 30)
                return true;
        return false;
    });
    Check(evidence->input->releases > 0 && !evidence->input->pressed);
    auto restored = fixed; restored.width = 640; restored.height = 360; restored.fps = 30;
    restored.preset = StreamPreset::Gaming;
    restored.bitrateMode = SettingMode::Auto; restored.bitrateLimitBps.reset();
    auto restore = host.UpdateStreamPreferences(restored); Check(Get(restore).error == StreamUpdateError::None);
    auto nextDiagnostic = std::chrono::steady_clock::now();
    Wait([&] {
        const auto status = host.Status();
        if (std::chrono::steady_clock::now() >= nextDiagnostic) {
            nextDiagnostic = std::chrono::steady_clock::now() + 1s;
            QJsonArray peers;
            for (const auto& peer : status.stream.peers) {
                auto number = [](const auto& value) -> QJsonValue { return value ? QJsonValue(double(*value)) : QJsonValue(QJsonValue::Null); };
                peers.append(QJsonObject{{"sourceWidth", peer.source.width}, {"appliedRevision", qint64(peer.appliedRevision)},
                    {"observedRevision", qint64(peer.observedRevision)}, {"rejected", peer.rejected},
                    {"encoded", number(peer.sender.framesEncoded)}, {"keyframes", number(peer.sender.keyFramesEncoded)},
                    {"targetBps", number(peer.sender.targetVideoBps)}, {"receiverStale", peer.receiver.stale},
                    {"receiverWidth", peer.receiver.observation ? int(peer.receiver.observation->width) : 0},
                    {"decoded", peer.receiver.observation ? qint64(peer.receiver.observation->framesDecoded) : 0}});
            }
            std::cerr << QJsonDocument(QJsonObject{{"stage", "restore"}, {"activePeers", int(status.activePeers)},
                {"failedPeers", int(status.failedPeers)}, {"peers", peers}}).toJson(QJsonDocument::Compact).toStdString() << std::endl;
        }
        for (const auto& peer : status.stream.peers)
            if (peer.receiver.observation && !peer.receiver.stale && peer.receiver.observation->width == 640)
                return true;
        return false;
    });
    Wait([&] { return host.Status().activePeers == 0; });
    auto stop = host.Stop(); Get(stop); Check(evidence->destroyed == 1 && !evidence->input->pressed);
    Print({{"passed", true}, {"role", "host"}, {"inputEvents", int(evidence->input->applied.load())},
        {"freshRejoin", true}, {"fixedAndAutoSettings", true}, {"gamingQualityTransitions", true}, {"runtimeReleased", true},
        {"physicalInput", false}, {"audibleOutput", false}, {"productionTls", true}});
}
void Viewer(const std::string& origin, const std::string& room) {
    auto evidence = std::make_shared<RemoteEvidence>();
    RoomSession viewer(Factory(evidence));
    RoomOptions options; options.origin = origin; options.roomId = room; options.nickname = "NetworkViewer";
    auto joining = viewer.Start(options); Check(Get(joining).error == RoomError::None);
    Wait([&] { return evidence->frames >= 45 && evidence->audio->audibleBlocks >= 20; });
    std::string hostPeer;
    Wait([&] {
        if (!viewer.Input()) return false;
        for (const auto& peer : viewer.Input()->Read()) if (peer.ready && peer.permission) {
            hostPeer = peer.peer; return true;
        }
        return false;
    });
    Check(viewer.Input()->Request(hostPeer, screenshare::input::Keyboard));
    Wait([&] {
        for (const auto& peer : viewer.Input()->Read()) if (peer.peer == hostPeer && peer.granted == screenshare::input::Keyboard) return true;
        return false;
    });
    QJsonArray samples;
    for (unsigned i = 0; i < 20; ++i) {
        screenshare::input::Event press; press.kind = screenshare::input::Kind::Key; press.key = 65; press.down = true;
        const auto before = evidence->responseFrames.load();
        const auto started = std::chrono::steady_clock::now();
        Check(viewer.Input()->Submit(hostPeer, press));
        Wait([&] { return evidence->responseFrames > before; });
        samples.append(double(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count()) / 1000.0);
        const auto clear = evidence->clearFrames.load();
        press.down = false; Check(viewer.Input()->Submit(hostPeer, press));
        Wait([&] { return evidence->clearFrames >= clear + 3; });
    }
    viewer.Input()->Revoke();
    Wait([&] { return evidence->smallFrames >= 30; });
    const auto beforeRestart = evidence->frames.load();
    evidence->restart = true;
    Wait([&] { return evidence->offers >= 2 && evidence->frames >= beforeRestart + 30; });
    auto stop = viewer.Stop(); Get(stop); Check(evidence->destroyed == 1 && !evidence->invalid);
    auto fresh = std::make_shared<RemoteEvidence>();
    RoomSession rejoined(Factory(fresh));
    auto rejoin = rejoined.Start(options); Check(Get(rejoin).error == RoomError::None);
    Wait([&] { return fresh->smallFrames >= 30 && fresh->audio->audibleBlocks >= 20; });
    // Three seconds of full-size frames leave time for fresh telemetry to reach
    // the host. This is a progress assertion, not a latency threshold.
    Wait([&] { return fresh->frames >= fresh->smallFrames + 90; });
    auto finalStop = rejoined.Stop(); Get(finalStop); Check(fresh->destroyed == 1 && !fresh->invalid);
    Print({{"passed", true}, {"role", "viewer"}, {"frames", int(evidence->frames + fresh->frames)},
        {"audioBlocks", qint64(evidence->audio->audibleBlocks + fresh->audio->audibleBlocks)},
        {"restart", true}, {"freshRejoin", true}, {"runtimeReleased", true},
        {"inputImageSamplesMs", samples}, {"timingScope", "viewer-submit-to-decoded-synthetic-response"},
        {"externalLatencyVerified", false}, {"physicalInput", false}, {"audibleOutput", false}, {"productionTls", true}});
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc == 3 && std::string_view(argv[1]) == "gpu-resources") {
        try {
            bool valid = false; const int seconds = QString::fromUtf8(argv[2]).toInt(&valid); Check(valid);
            Print(loadproof::GpuResources(seconds)); return 0;
        } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
    webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
    webrtc::InitializeLogging(std::move(logging)); webrtc::WinsockInitializer winsock;
    if (winsock.error() || !webrtc::InitializeSSL()) return 1;
    int result = 0;
    try {
        Check(argc >= 4 && argc <= 7);
        const QUrl origin(QString::fromUtf8(argv[2]));
        Check(origin.isValid() && origin.scheme() == "https" && !origin.host().isEmpty() && origin.userInfo().isEmpty() &&
            !origin.hasQuery() && !origin.hasFragment() && (origin.path().isEmpty() || origin.path() == "/") && QSslSocket::supportsSsl());
        const std::string role = argv[1];
        if (role == "load-host" || role == "load-viewer") {
            Check(argc >= 5 && argc <= 7);
            const std::string decoder = argc >= 6 ? argv[5] : "hardware";
            Check(decoder == "hardware" || decoder == "software");
            const std::string consumer = argc == 7 ? argv[6] : "pixels";
            Check(consumer == "pixels" || consumer == "presentation");
            bool valid = false; const int seconds = QString::fromUtf8(argv[4]).toInt(&valid); Check(valid);
            const auto metrics = loadproof::Run(role == "load-host", argv[2], QString::fromUtf8(argv[3]), seconds, decoder == "hardware", consumer == "presentation");
            Print(metrics); Check(metrics["passed"].toBool());
        }
        else if (role == "host") { Check(argc == 4); Host(argv[2], QString::fromUtf8(argv[3])); }
        else { Check(role == "viewer"); Viewer(argv[2], argv[3]); }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    webrtc::CleanupSSL(); return result;
}
