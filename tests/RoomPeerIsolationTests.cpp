#include "../tools/webrtc-proof/PublicRoomSessionFixture.h"
#include "media/SignalingExecutor.h"
#include <deque>

// Real native peers, codecs and synthetic media; no physical devices or service.
// The second viewer negotiates SDP but has no usable ICE path.
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE);
    logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
    webrtc::InitializeLogging(std::move(logging));
    webrtc::WinsockInitializer winsock;
    if (winsock.error() || !webrtc::InitializeSSL()) return 1;
    int result = 0;
    try {
        SignalingExecutor executor;
        std::unique_ptr<RoomRuntime> host, healthy, stalled;
        struct Signal { int destination; RoomPeerSignal value; };
        std::deque<Signal> pendingSignals;
        auto evidence = std::make_shared<Evidence>();
        auto run = [&](auto work) {
            std::exception_ptr failure;
            auto task = executor.Post([&] { try { work(); } catch (...) { failure = std::current_exception(); } });
            Check(Get(task).error == ExecutorError::None);
            if (failure) std::rethrow_exception(failure);
        };
        auto tick = [&] {
            host->Advance(); healthy->Advance(); if (stalled) stalled->Advance();
            while (!pendingSignals.empty()) {
                auto signal = std::move(pendingSignals.front()); pendingSignals.pop_front();
                if (signal.destination == 0) Check(host->Receive("healthy", std::move(signal.value)));
                else if (signal.destination == 1) Check(healthy->Receive("host", std::move(signal.value)));
                else if (signal.destination == 2) Check(host->Receive("stalled", std::move(signal.value)));
                else Check(stalled->Receive("host", std::move(signal.value)));
            }
        };
        auto wait = [&](auto predicate) {
            Wait([&] { bool done = false; run([&] { tick(); done = predicate(); }); return done; });
        };
        auto stop = [&] {
            std::vector<std::shared_future<void>> stops;
            run([&] { for (auto* peer : {host.get(), healthy.get(), stalled.get()}) if (peer) stops.push_back(peer->BeginStop()); });
            Wait([&] {
                run([&] { for (auto* peer : {host.get(), healthy.get(), stalled.get()}) if (peer) peer->Advance(); });
                return std::all_of(stops.begin(), stops.end(), [](auto& future) { return future.wait_for(0ms) == std::future_status::ready; });
            });
            for (auto& future : stops) future.get();
            run([&] { stalled.reset(); healthy.reset(); host.reset(); });
        };
        try {
            run([&] {
                host = std::make_unique<Runtime>(RoomIdentity{true, "test", "host"},
                    [&](const auto& peer, auto signal) {
                        if (peer == "stalled" && signal.kind == RoomPeerSignal::Kind::Candidate) return true;
                        pendingSignals.push_back({peer == "healthy" ? 1 : 3, std::move(signal)}); return true;
                    }, std::make_shared<Evidence>());
                healthy = std::make_unique<Runtime>(RoomIdentity{false, "test", "healthy"},
                    [&](const auto&, auto signal) { pendingSignals.push_back({0, std::move(signal)}); return true; }, evidence);
            });
            wait([&] { return host->Ready("healthy") && healthy->Ready("host"); });
            run([&] { Check(host->Add("healthy")); Check(healthy->Add("host")); });
            wait([&] { return evidence->frames >= 30; });
            uint64_t revision = 0;
            run([&] {
                revision = host->StreamSettings().requestedRevision;
                NativeRoomRuntimeOptions options;
                options.connection.type = webrtc::PeerConnectionInterface::kRelay;
                options.engine = [] {
                    return std::make_unique<MediaEngine>(CreatePcmAudioDeviceModule(proof::SyntheticAudio(std::make_shared<proof::AudioEvidence>()),
                        std::make_shared<PcmAudioDiagnostics>()), std::make_unique<MfVideoEncoderFactory>(), std::make_unique<MfVideoDecoderFactory>());
                };
                stalled = CreateNativeRoomRuntime({false, "test", "stalled"}, [&](const auto&, auto signal) {
                    if (signal.kind != RoomPeerSignal::Kind::Candidate) pendingSignals.push_back({2, std::move(signal)});
                    return true;
                }, std::move(options));
                Check(stalled->Add("host")); Check(host->Add("stalled"));
            });
            wait([&] {
                for (const auto& peer : host->StreamSettings().peers)
                    if (peer.peerId == "stalled") return peer.appliedRevision == revision;
                return false;
            });
            const auto before = evidence->frames.load();
            wait([&] { return evidence->frames >= before + 45; });
            run([&] {
                const auto status = host->StreamSettings();
                Check(status.requestedRevision == revision && status.peers.size() == 2);
                for (const auto& peer : status.peers) {
                    if (peer.peerId == "stalled") {
                        Check(peer.recovery.state == PeerLifecycleState::Connecting);
                        if (peer.source.observedRevision != 0)
                            throw std::runtime_error("Unconnected viewer is processing capture frames on shared resources");
                    } else Check(peer.recovery.state == PeerLifecycleState::Connected && peer.observedRevision == revision);
                }
            });
            auto lastFrameAt = std::chrono::steady_clock::now();
            auto lastFrameCount = evidence->frames.load();
            wait([&] {
                const auto now = std::chrono::steady_clock::now();
                const auto frames = evidence->frames.load();
                if (frames != lastFrameCount) { lastFrameCount = frames; lastFrameAt = now; }
                if (now - lastFrameAt > 2s) throw std::runtime_error("Stalled peer interrupted healthy viewer delivery");
                return !host->FailedPeers().empty() && !stalled->FailedPeers().empty();
            });
            run([&] {
                host->Remove("stalled"); stalled->Remove("host");
                for (auto* runtime : {host.get(), stalled.get()}) {
                    const auto status = runtime->StreamSettings();
                    const auto failed = std::find_if(status.connections.begin(), status.connections.end(),
                        [](const auto& value) { return value.retained; });
                    Check(failed != status.connections.end() && failed->negotiated &&
                        failed->recovery.state == PeerLifecycleState::Failed &&
                        failed->recovery.failure == PeerLifecycleFailure::DirectConnectTimeout);
                    Check(failed->events.records && !failed->events.records->empty());
                    Check(failed->transportHistory.records && failed->transportHistory.records->size() >= 5);
                    Check(failed->statsAgeMs && *failed->statsAgeMs < 3000);
                    Check(failed->current.labels.at("connectionState") != "connected");
                }
                Check(host->StreamSettings().requestedRevision == revision);
            });
            const auto after = evidence->frames.load();
            wait([&] { return evidence->frames >= after + 30; });
            stop();
            std::cout << "{\"passed\":true,\"unconnected_peer_capture_isolated\":true}\n";
        } catch (...) { stop(); throw; }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    webrtc::CleanupSSL();
    return result;
}
