#include "PublicRoomSessionFixture.h"
#include "LifecycleDiagnostics.h"
#include "core/WindowsMediaRuntime.h"

namespace {
void RunCycle(const std::string& origin, int cycle, int seconds) {
    auto factory = [](auto evidence) { return [evidence](auto identity, auto send) {
        return std::make_unique<Runtime>(identity, std::move(send), evidence);
    }; };
    auto hostEvidence = std::make_shared<Evidence>();
    std::array<std::shared_ptr<Evidence>, 4> evidence;
    std::array<std::unique_ptr<RoomSession>, 4> viewers;
    RoomSession host(factory(hostEvidence), true);
    RoomOptions options; options.origin = origin; options.host = true;
    options.name = "Lifecycle stress"; options.nickname = "StressHost";
    auto starting = host.Start(options); Check(Get(starting).error == RoomError::None);
    options.host = false; options.roomId = host.Status().roomId;
    for (size_t i = 0; i < viewers.size(); ++i) {
        evidence[i] = std::make_shared<Evidence>();
        viewers[i] = std::make_unique<RoomSession>(factory(evidence[i]), true);
        options.nickname = "StressViewer" + std::to_string(i);
        auto joining = viewers[i]->Start(options); Check(Get(joining).error == RoomError::None);
    }
    Wait([&] {
        for (const auto& value : evidence) if (value->frames < 15 || value->audio->audibleBlocks < 10) return false;
        return host.Status().activePeers == 4;
    });
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    auto checkpoint = std::chrono::steady_clock::now();
    std::array<unsigned, 4> before{};
    std::array<uint64_t, 4> audioBefore{};
    while (std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(100ms);
        Check(host.Status().phase == RoomPhase::Active && host.Status().activePeers == 4 && !host.Status().failedPeers);
        if (std::chrono::steady_clock::now() - checkpoint >= 5s) {
            uint64_t frames = 0, maxAge = 0;
            for (size_t i = 0; i < evidence.size(); ++i) {
                Check(!evidence[i]->invalid && evidence[i]->frames > before[i] && evidence[i]->audio->audibleBlocks > audioBefore[i]);
                before[i] = evidence[i]->frames; audioBefore[i] = evidence[i]->audio->audibleBlocks;
                frames += before[i];
            }
            for (const auto& peer : host.Status().stream.peers)
                maxAge = std::max(maxAge, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(peer.delivery.maxHandoffAge).count()));
            std::cout << "{\"type\":\"progress\",\"cycle\":" << cycle << ",\"frames\":" << frames
                << ",\"maxCaptureHandoffUs\":" << maxAge << "}" << std::endl;
            proof::LifecycleSample(-cycle, std::chrono::duration<double>(std::chrono::steady_clock::now() - checkpoint).count());
            checkpoint = std::chrono::steady_clock::now();
        }
    }
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
    }
    Check(oldPort && !oldPort->Request(hostId, screenshare::input::Gamepad));
    oldPort.reset();
    std::this_thread::sleep_for(50ms);
    for (size_t i = 0; i < evidence.size(); ++i)
        Check(evidence[i]->frames == frames[i] && evidence[i]->audio->playedBlocks == audio[i]);
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
    webrtc::InitializeLogging(std::move(logging)); webrtc::WinsockInitializer winsock;
    if (winsock.error() || !webrtc::InitializeSSL()) return 1;
    int result = 0;
    try {
        Check(argc == 3);
        size_t countUsed = 0, secondsUsed = 0;
        const int cycles = std::stoi(argv[1], &countUsed), seconds = std::stoi(argv[2], &secondsUsed);
        Check(countUsed == std::string(argv[1]).size() && secondsUsed == std::string(argv[2]).size());
        Check(cycles >= 1 && cycles <= 100 && seconds >= 0 && seconds <= 7200 && (cycles == 1 || seconds == 0));
        screenshare::WindowsMediaRuntime runtime; Check(SUCCEEDED(runtime.result()));
        proof::LifecycleSample(0, 0);
        for (int cycle = 1; cycle <= cycles; ++cycle) {
            std::string origin;
            Check(bool(std::getline(std::cin, origin)) && origin.starts_with("http://127.0.0.1:"));
            const auto start = std::chrono::steady_clock::now();
            RunCycle(origin, cycle, seconds);
            proof::LifecycleSample(cycle, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            std::cout << "{\"type\":\"cycle\",\"cycle\":" << cycle << "}" << std::endl;
        }
        std::cout << "{\"type\":\"complete\",\"passed\":true,\"cycles\":" << cycles << ",\"soakSeconds\":" << seconds << "}" << std::endl;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    webrtc::CleanupSSL(); return result;
}
