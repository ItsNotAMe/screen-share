#include "PublicRoomSessionFixture.h"
#include "LifecycleDiagnostics.h"
#include "MemoryAccounting.h"
#include "../../frontend/shared/LatestRoomVideoFrame.h"
#include "../../frontend/shared/FrameQueueDiagnostics.h"
#include "core/WindowsMediaRuntime.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
struct CycleLifetime {
    std::vector<std::weak_ptr<void>> dependencies;
    unsigned peakCaptureResources = 0;
    double activeSeconds = 0;
};
CycleLifetime RunCycle(const std::string& origin, int cycle, int seconds, bool slowViewer) {
    CycleLifetime lifetime;
    auto factory = [](auto evidence, std::shared_ptr<LatestRoomVideoFrame> frames = {}) { return [evidence, frames](auto identity, auto send) {
        return std::make_unique<Runtime>(identity, std::move(send), evidence, frames);
    }; };
    auto hostEvidence = std::make_shared<Evidence>();
    std::array<std::shared_ptr<Evidence>, 4> evidence;
    std::array<std::shared_ptr<LatestRoomVideoFrame>, 4> presentation;
    auto consume = [&](size_t i) {
        if (auto frame = presentation[i]->Take()) {
            const auto pixels = frame->pixels();
            Check(frame->width == 640 && frame->height == 360 && pixels.size() == 640 * 360 * 3 / 2);
            Check(pixels[640 * 180 + 320] >= 35);
            ++evidence[i]->frames;
        }
    };
    std::array<std::unique_ptr<RoomSession>, 4> viewers;
    RoomSession host(factory(hostEvidence), true);
    RoomOptions options; options.origin = origin; options.host = true;
    options.name = "Lifecycle stress"; options.nickname = "StressHost";
    auto starting = host.Start(options); Check(Get(starting).error == RoomError::None);
    options.host = false; options.roomId = host.Status().roomId;
    for (size_t i = 0; i < viewers.size(); ++i) {
        evidence[i] = std::make_shared<Evidence>();
        presentation[i] = std::make_shared<LatestRoomVideoFrame>();
        viewers[i] = std::make_unique<RoomSession>(factory(evidence[i], presentation[i]), true);
        options.nickname = "StressViewer" + std::to_string(i);
        auto joining = viewers[i]->Start(options); Check(Get(joining).error == RoomError::None);
    }
    Wait([&] {
        for (size_t i = 0; i < viewers.size(); ++i) consume(i);
        for (const auto& value : evidence) if (value->frames < 15 || value->audio->audibleBlocks < 10) return false;
        return host.Status().activePeers == 4;
    });
    const auto began = std::chrono::steady_clock::now();
    const auto end = began + std::chrono::seconds(seconds);
    auto checkpoint = began;
    auto lastSlowTake = began;
    std::string checkpointPhase = "healthy";
    std::array<unsigned, 4> before{};
    std::array<uint64_t, 4> audioBefore{};
    for (size_t i = 0; i < evidence.size(); ++i) {
        before[i] = evidence[i]->frames; audioBefore[i] = evidence[i]->audio->audibleBlocks;
    }
    while (std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(10ms);
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double>(now - began).count();
        const std::string phase = !slowViewer || elapsed < 15 ? "healthy" : elapsed < 35 ? "slow" : "recovered";
        for (size_t i = 0; i < viewers.size(); ++i) {
            if (i == 0 && phase == "slow" && now - lastSlowTake < 250ms) continue;
            consume(i);
            if (i == 0) lastSlowTake = now;
        }
        Check(host.Status().phase == RoomPhase::Active && host.Status().activePeers == 4 && !host.Status().failedPeers);
        // Four independent delivery workers each own at most an active and a
        // pending capture resource, plus acquisition/handoff. This is only the
        // capture queue budget, not a claim about decoder/presentation queues.
        Check(hostEvidence->capture->peakResources <= 10);
        if (now - checkpoint >= 5s) {
            uint64_t frames = 0, maxAge = 0;
            QJsonArray viewerProgress;
            for (size_t i = 0; i < evidence.size(); ++i) {
                Check(!evidence[i]->invalid && evidence[i]->frames > before[i] && evidence[i]->audio->audibleBlocks > audioBefore[i]);
                Check(!evidence[i]->smallFrames); // The fixed-resolution contract applies to every viewer.
                const auto received = evidence[i]->frames.load();
                const auto audible = evidence[i]->audio->audibleBlocks.load();
                const auto queue = presentation[i]->statistics();
                Check(queue.received >= queue.delivered + queue.replaced &&
                    queue.received - queue.delivered - queue.replaced <= 1);
                QJsonObject viewer{{"viewer", int(i)}, {"frames", double(received)},
                    {"frameDelta", double(received - before[i])}, {"audioBlocks", double(audible)},
                    {"audioDelta", double(audible - audioBefore[i])}, {"received", double(queue.received)},
                    {"replaced", double(queue.replaced)}, {"pending", double(queue.pending)},
                    {"handoff", FrameQueueDiagnostics(queue)}};
                // Reuse the scheduled public snapshot; do not add stats requests
                // or change encoder/transport policy to make a stress result pass.
                const auto peerId = viewers[i]->Status().peerId;
                for (const auto& peer : host.Status().stream.peers) if (peer.peerId == peerId) {
                    auto number = [](const auto& value) -> QJsonValue { return value ? QJsonValue(double(*value)) : QJsonValue(QJsonValue::Null); };
                    viewer.insert("sender", QJsonObject{{"encodedFps", number(peer.sender.encodedFps)},
                        {"payloadBps", number(peer.sender.payloadBps)}, {"availableOutgoingBps", number(peer.sender.availableOutgoingBps)},
                        {"meanEncodeMs", number(peer.sender.meanEncodeMs)}, {"rttMs", number(peer.sender.rttMs)},
                        {"lossFraction", number(peer.sender.lossFraction)}, {"limitingReason", int(peer.sender.limitingReason)},
                        {"nackCount", number(peer.sender.nackCount)}, {"pliCount", number(peer.sender.pliCount)},
                        {"retransmittedPackets", number(peer.sender.retransmittedPackets)},
                        {"sourceDropped", double(peer.source.dropped)}, {"delivered", double(peer.delivery.delivered)},
                        {"deliveryReplaced", double(peer.delivery.replaced)}});
                    if (const auto& remote = peer.receiver.observation) {
                        viewer.insert("receiver", QJsonObject{{"framesDecoded", double(remote->framesDecoded)},
                            {"fpsMilli", number(remote->fpsMilli)}, {"decoderDrops", number(remote->decoderDrops)},
                            {"jitterBufferMeanMs", number(remote->jitterBufferMeanMs)}});
                    } else viewer.insert("receiver", QJsonValue(QJsonValue::Null));
                }
                viewerProgress.append(viewer);
                before[i] = received; audioBefore[i] = audible;
                frames += before[i];
            }
            for (const auto& peer : host.Status().stream.peers)
                maxAge = std::max(maxAge, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(peer.delivery.maxHandoffAge).count()));
            const auto interval = std::chrono::duration<double>(now - checkpoint).count();
            const QJsonObject progress{{"type", "progress"}, {"cycle", cycle}, {"frames", double(frames)},
                {"elapsedSeconds", elapsed}, {"intervalSeconds", interval},
                {"phase", QString::fromStdString(phase == checkpointPhase ? phase : "transition")},
                {"viewers", viewerProgress}, {"maxCaptureHandoffUs", double(maxAge)},
                {"peakCaptureResources", int(hostEvidence->capture->peakResources.load())}};
            std::cout << QJsonDocument(progress).toJson(QJsonDocument::Compact).constData() << std::endl;
            proof::LifecycleSample(-cycle, interval);
            checkpoint = now; checkpointPhase = phase;
        }
    }
    lifetime.activeSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    // Alternate viewer-first and host-first shutdown, retaining the public port
    // past stop to verify it cannot submit into a retired runtime.
    auto oldPort = viewers[0]->Input();
    const auto hostId = host.Status().peerId;
    if (cycle % 2 == 0) { auto stop = host.Stop(); Get(stop); }
    std::array<std::shared_future<void>, 4> stops;
    for (size_t i = 0; i < viewers.size(); ++i) stops[i] = viewers[i]->Stop();
    for (auto& stop : stops) Get(stop);
    auto stop = host.Stop(); Get(stop);
    Check(hostEvidence->destroyed == 1);
    std::array<unsigned, 4> frames{};
    std::array<uint64_t, 4> audio{};
    for (size_t i = 0; i < evidence.size(); ++i) {
        Check(evidence[i]->destroyed == 1 && !evidence[i]->invalid);
        frames[i] = evidence[i]->frames; audio[i] = evidence[i]->audio->playedBlocks;
        viewers[i].reset();
        presentation[i]->Stop();
        Check(!presentation[i]->Take());
        const auto finalQueue = presentation[i]->statistics();
        Check(!finalQueue.pending && !finalQueue.inFlight && !finalQueue.failed &&
            finalQueue.received == finalQueue.delivered + finalQueue.replaced + finalQueue.discardedOnStop);
        lifetime.dependencies.push_back(presentation[i]);
        presentation[i].reset();
    }
    Check(oldPort && !oldPort->Request(hostId, screenshare::input::Gamepad));
    oldPort.reset();
    std::this_thread::sleep_for(50ms);
    for (size_t i = 0; i < evidence.size(); ++i)
        Check(evidence[i]->frames == frames[i] && evidence[i]->audio->playedBlocks == audio[i]);
    auto observe = [&](const auto& value) {
        Check(value->audio->liveCaptures == 0 && value->audio->liveOutputs == 0);
        Check(value->capture->sources == 0 && value->capture->resources == 0);
        lifetime.dependencies.insert(lifetime.dependencies.end(), {value, value->audio, value->input, value->capture});
    };
    observe(hostEvidence);
    for (const auto& value : evidence) observe(value);
    lifetime.peakCaptureResources = hostEvidence->capture->peakResources;
    return lifetime; // Weak checks run only after RoomSession and factory closures are destroyed.
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
    webrtc::InitializeLogging(std::move(logging)); webrtc::WinsockInitializer winsock;
    if (winsock.error() || !webrtc::InitializeSSL()) return 1;
    int result = 0;
    try {
        Check(argc == 3 || argc == 5 || argc == 6);
        size_t countUsed = 0, secondsUsed = 0;
        const int cycles = std::stoi(argv[1], &countUsed), seconds = std::stoi(argv[2], &secondsUsed);
        Check(countUsed == std::string(argv[1]).size() && secondsUsed == std::string(argv[2]).size());
        Check(cycles >= 1 && cycles <= 100 && seconds >= 0 && seconds <= 7200 && (cycles == 1 || seconds == 0));
        int idleSeconds = 0; bool slowViewer = false;
        if (argc >= 5) {
            size_t idleUsed = 0;
            idleSeconds = std::stoi(argv[3], &idleUsed);
            Check(idleUsed == std::string(argv[3]).size() && idleSeconds >= 0 && idleSeconds <= 600);
            Check(std::string(argv[4]) == "0" || std::string(argv[4]) == "1"); slowViewer = std::string(argv[4]) == "1";
        }
        Check(!slowViewer || (cycles == 1 && seconds >= 60));
        bool memoryAccounting = false;
        if (argc == 6) { Check(std::string(argv[5]) == "0" || std::string(argv[5]) == "1"); memoryAccounting = std::string(argv[5]) == "1"; }
        screenshare::WindowsMediaRuntime runtime; Check(SUCCEEDED(runtime.result()));
        proof::LifecycleSample(0, 0);
        if (memoryAccounting) proof::MemoryAccountingSample(0, false);
        for (int cycle = 1; cycle <= cycles; ++cycle) {
            std::string origin;
            Check(bool(std::getline(std::cin, origin)) && origin.starts_with("http://127.0.0.1:"));
            const auto start = std::chrono::steady_clock::now();
            const auto lifetime = RunCycle(origin, cycle, seconds, slowViewer);
            Check(lifetime.dependencies.size() == 24);
            for (const auto& value : lifetime.dependencies) Check(value.expired());
            Check(lifetime.peakCaptureResources <= 10);
            proof::LifecycleSample(cycle, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            if (memoryAccounting && (cycle % 10 == 0 || cycle == cycles)) proof::MemoryAccountingSample(cycle, false);
            std::cout << "{\"type\":\"cycle\",\"cycle\":" << cycle << ",\"ownershipReleased\":true,\"peakCaptureResources\":"
                << lifetime.peakCaptureResources << ",\"activeSeconds\":" << lifetime.activeSeconds << "}" << std::endl;
        }
        if (idleSeconds) {
            const auto idleBegan = std::chrono::steady_clock::now();
            for (int second = 0; second <= idleSeconds; ++second) {
                std::this_thread::sleep_until(idleBegan + std::chrono::seconds(second));
                proof::LifecycleSample(second, std::chrono::duration<double>(std::chrono::steady_clock::now() - idleBegan).count(), "ROOM_IDLE");
                if (memoryAccounting && (second % 10 == 0 || second == idleSeconds)) proof::MemoryAccountingSample(second, true);
                std::cout << "{\"type\":\"idle\",\"second\":" << second << "}" << std::endl;
            }
        }
        std::cout << "{\"type\":\"complete\",\"passed\":true,\"cycles\":" << cycles << ",\"soakSeconds\":" << seconds
            << ",\"idleSeconds\":" << idleSeconds << ",\"slowViewer\":" << (slowViewer ? "true" : "false")
            << ",\"memoryAccounting\":" << (memoryAccounting ? "true" : "false") << "}" << std::endl;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    webrtc::CleanupSSL(); return result;
}
