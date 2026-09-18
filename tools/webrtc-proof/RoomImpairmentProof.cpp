#include "PublicRoomSessionFixture.h"
#include "ImpairedPacketSocket.h"
#include "ImpairedPacketSocketChecks.h"
#include "../../frontend/shared/LatestRoomVideoFrame.h"
#include "../../frontend/shared/FrameQueueDiagnostics.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <set>
#include "RoomProcessProof.h"
#include "BoundedRtcEventLog.h"

// Enough changing detail to exercise bandwidth adaptation, unlike the low-rate
// gradient used by lifecycle tests. This never captures the user's desktop.
class NoiseCapture final : public ICaptureSource {
    SyntheticCaptureSource source_{640, 360, 30};
    uint32_t state_ = 12345;
public:
    void Start() override { source_.Start(); }
    std::optional<CaptureSample> Poll() override {
        auto sample = source_.Poll();
        if (sample) {
            auto frame = std::static_pointer_cast<SyntheticCaptureResource>(sample->resource);
            for (auto& pixel : frame->luma) {
                state_ ^= state_ << 13; state_ ^= state_ >> 17; state_ ^= state_ << 5;
                pixel = uint8_t(70 + state_ % 100);
            }
        }
        return sample;
    }
    bool Closed() const override { return false; }
    void Retire() noexcept override {}
    void Rebuild() override { Start(); }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
    webrtc::InitializeLogging(std::move(logging)); webrtc::WinsockInitializer winsock;
    if (winsock.error() || !webrtc::InitializeSSL()) return 1;
    int result = 0;
    try {
        if (argc == 4 && std::string(argv[1]) == "--process-viewer") { ProcessViewer(argv[2], argv[3]); webrtc::CleanupSSL(); return 0; }
        if (argc == 3 && std::string(argv[2]) == "processes") { SeparateProcessProof(argv[1]); webrtc::CleanupSSL(); return 0; }
        proof::CheckImpairedPacketSocket();
        Check(argc >= 3);
        bool fastAudioExperiment = false;
        std::filesystem::path eventLogDirectory;
        for (int i = 3; i < argc; ++i) {
            const std::string flag = argv[i];
            if (flag == "--fast-audio-experiment" && !fastAudioExperiment) fastAudioExperiment = true;
            else if (flag == "--event-logs" && eventLogDirectory.empty() && i + 1 < argc) eventLogDirectory = argv[++i];
            else throw std::invalid_argument("Unknown or duplicate impairment option");
        }
        const std::string scenario = argv[2];
        Check(scenario == "collapse" || scenario == "loss2" || scenario == "loss5" || scenario == "reorder" || scenario == "duplicate");
        auto link = std::make_shared<proof::LinkControl>(12345);
        const int healthyCapacityKbps = scenario == "collapse" ? 20000 : 100000;
        auto initialNetwork = link->Read().network;
        initialNetwork.link_capacity = webrtc::DataRate::KilobitsPerSec(healthyCapacityKbps);
        link->Set(initialNetwork);
        auto hostEvidence = std::make_shared<Evidence>();
        auto eventLogs = std::make_shared<proof::EventLogEvidence>();
        if (!eventLogDirectory.empty()) {
            Check(std::filesystem::create_directory(eventLogDirectory));
            hostEvidence->eventLogFactory = [eventLogDirectory, eventLogs] {
                const auto index = eventLogs->opened.load();
                Check(index < 4);
                return std::make_unique<proof::BoundedRtcEventLog>(eventLogDirectory / ("host-" + std::to_string(index) + ".rtc"), eventLogs);
            };
        }
        hostEvidence->localizedInputResponse = true;
        hostEvidence->captureFactory = [] { return std::make_unique<NoiseCapture>(); };
        StreamPreferences preferences;
        preferences.resolution = ResolutionMode::Fixed; preferences.width = 640; preferences.height = 360;
        preferences.fps = 30; preferences.bitrateMode = SettingMode::Manual; preferences.bitrateLimitBps = 20000000;
        hostEvidence->preferences = preferences;
        auto factory = [](auto evidence, std::shared_ptr<LatestRoomVideoFrame> frames = {}) { return [evidence, frames](auto identity, auto send) { return std::make_unique<Runtime>(identity, std::move(send), evidence, frames); }; };
        RoomOptions options; options.origin = argv[1]; options.host = true; options.nickname = "NetworkHost"; options.name = "Impairment proof";
        RoomSession host(factory(hostEvidence), true);
        auto start = host.Start(options); Check(Get(start).error == RoomError::None);
        options.host = false; options.roomId = host.Status().roomId;
        std::array<std::shared_ptr<Evidence>, 4> evidence;
        std::array<std::shared_ptr<LatestRoomVideoFrame>, 4> presentation;
        std::array<std::unique_ptr<RoomSession>, 4> viewers;
        std::array<bool, 4> responseVisible{};
        for (size_t i = 0; i < viewers.size(); ++i) {
            evidence[i] = std::make_shared<Evidence>();
            evidence[i]->fastAudioExperiment = fastAudioExperiment;
            if (i == 0) evidence[i]->packetFactory = [link](auto* sockets) { return std::make_unique<proof::ImpairedPacketFactory>(sockets, link); };
            presentation[i] = std::make_shared<LatestRoomVideoFrame>();
            viewers[i] = std::make_unique<RoomSession>(factory(evidence[i], presentation[i]), true);
            options.nickname = "NetworkViewer" + std::to_string(i);
            auto join = viewers[i]->Start(options); Check(Get(join).error == RoomError::None);
        }
        auto consume = [&] {
            for (size_t i = 0; i < 4; ++i) if (auto frame = presentation[i]->Take()) {
                Check(frame->width == 640 && frame->height == 360 && frame->pixels().size() == 640 * 360 * 3 / 2);
                Check(frame->pixels()[640 * 180 + 320] >= 35); ++evidence[i]->frames;
                unsigned marker = 0;
                for (unsigned y = 178; y < 182; ++y) for (unsigned x = 318; x < 322; ++x)
                    marker += frame->pixels()[640 * y + x];
                responseVisible[i] = marker > 16 * 195; // Recording input makes the existing response scene white.
            }
        };
        Wait([&] { consume(); for (auto& e : evidence) if (e->frames < 30 || e->audio->audibleBlocks < 10) return false; return host.Status().activePeers == 4; });
        const auto hostId = host.Status().peerId, viewerId = viewers[0]->Status().peerId;
        Wait([&] {
            consume();
            for (const auto& peer : viewers[0]->Input()->Read()) if (peer.peer == hostId && peer.ready && peer.permission) return true;
            return false;
        });
        Check(viewers[0]->Input()->Request(hostId, screenshare::input::Keyboard));
        Wait([&] { consume(); for (const auto& peer : host.Input()->Read()) if (peer.peer == viewerId && peer.requested == screenshare::input::Keyboard) return true; return false; });
        Check(host.Input()->Grant(viewerId, screenshare::input::Keyboard));
        auto granted = [&] { for (const auto& peer : viewers[0]->Input()->Read()) if (peer.peer == hostId && peer.granted == screenshare::input::Keyboard) return true; return false; };
        Wait([&] { consume(); return granted(); });
        // WebRTC starts at 3 Mbps, below the 4 Mbps collapse under test. Give
        // congestion control a fixed, recorded convergence interval; never retry
        // or select a favorable baseline. The runner still rejects low load.
        QJsonArray warmupIngress;
        for (int second = 0; second < 20; ++second) {
            const auto bytes = link->deliveredBytes.load();
            const auto began = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - began < 1s) { consume(); std::this_thread::sleep_for(5ms); }
            const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
            warmupIngress.append(double(link->deliveredBytes.load() - bytes) * 8 / seconds);
        }
        bool inputApplied = false, inputRevoked = false;
        std::optional<int64_t> inputResponseInternalMs;
        QJsonArray samples;
        std::array<unsigned, 4> previous{};
        auto previousBytes = link->deliveredBytes.load();
        for (auto phase : {"baseline", "impaired", "recovery"}) {
            auto config = webrtc::BuiltInNetworkBehaviorConfig{};
            config.queue_length_packets = 256; config.link_capacity = webrtc::DataRate::KilobitsPerSec(healthyCapacityKbps);
            const bool impaired = std::string(phase) == "impaired";
            if (impaired && scenario == "collapse") config.link_capacity = webrtc::DataRate::KilobitsPerSec(4000);
            if (impaired && (scenario == "loss2" || scenario == "loss5" || scenario == "reorder")) {
                config.queue_delay_ms = 25; config.delay_standard_deviation_ms = 10;
                config.loss_percent = scenario == "loss2" ? 2 : scenario == "loss5" ? 5 : 0;
                config.allow_reordering = scenario == "reorder";
            }
            link->Set(config, impaired && scenario == "duplicate" ? 50 : 0);
            for (auto& p : previous) p = 0;
            for (size_t i = 0; i < 4; ++i) previous[i] = evidence[i]->frames;
            auto previousSampleAt = std::chrono::steady_clock::now();
            for (int second = 1; second <= 12; ++second) {
                const auto before = std::chrono::steady_clock::now();
                while (std::chrono::steady_clock::now() - before < 1s) { consume(); std::this_thread::sleep_for(5ms); }
                const auto sampledAt = std::chrono::steady_clock::now();
                const double seconds = std::chrono::duration<double>(sampledAt - previousSampleAt).count();
                previousSampleAt = sampledAt;
                QJsonArray frames;
                for (size_t i = 0; i < 4; ++i) { const auto n = evidence[i]->frames.load(); frames.append(double(n - previous[i]) / seconds); previous[i] = n; }
                QJsonArray peers;
                const auto status = host.Status();
                for (size_t i = 0; i < 4; ++i) {
                    QJsonObject observation{{"viewer", int(i)}, {"audioBlocks", qint64(evidence[i]->audio->audibleBlocks.load())}};
                    const auto queue = presentation[i]->statistics();
                    Check(queue.received >= queue.delivered + queue.replaced && queue.received - queue.delivered - queue.replaced <= 1);
                    observation.insert("pending", qint64(queue.pending));
                    observation.insert("handoff", FrameQueueDiagnostics(queue));
                    for (const auto& peer : status.stream.peers) if (peer.peerId == viewers[i]->Status().peerId) {
                        auto number = [](const auto& value) -> QJsonValue { return value ? QJsonValue(double(*value)) : QJsonValue(QJsonValue::Null); };
                        observation.insert("payloadBps", number(peer.sender.payloadBps));
                        observation.insert("targetVideoBps", number(peer.sender.targetVideoBps));
                        observation.insert("meanPacketSendDelayMs", number(peer.sender.meanPacketSendDelayMs));
                        observation.insert("framesEncoded", number(peer.sender.framesEncoded));
                        observation.insert("keyFramesEncoded", number(peer.sender.keyFramesEncoded));
                        observation.insert("meanEncodeMs", number(peer.sender.meanEncodeMs));
                        observation.insert("availableOutgoingBps", number(peer.sender.availableOutgoingBps));
                        observation.insert("rttMs", number(peer.sender.rttMs));
                        observation.insert("lossFraction", number(peer.sender.lossFraction));
                        observation.insert("jitterBufferMeanMs", peer.receiver.observation ? number(peer.receiver.observation->jitterBufferMeanMs) : QJsonValue(QJsonValue::Null));
                        observation.insert("jitterBufferRecentMs", peer.receiver.observation ? number(peer.receiver.observation->jitterBufferRecentMs) : QJsonValue(QJsonValue::Null));
                    }
                    peers.append(observation);
                }
                const auto bytes = link->deliveredBytes.load();
                QJsonObject sample{{"phase", phase}, {"second", second}, {"intervalSeconds", seconds}, {"fps", frames}, {"ingressBps", double(bytes - previousBytes) * 8 / seconds},
                    {"capacityBps", qint64(config.link_capacity.bps())}, {"delayMeanMs", config.queue_delay_ms},
                    {"delayStddevMs", config.delay_standard_deviation_ms}, {"configuredLossPercent", config.loss_percent},
                    {"allowReordering", config.allow_reordering}, {"duplicateEvery", impaired && scenario == "duplicate" ? 50 : 0},
                    {"lost", qint64(link->lost.load())}, {"overflow", qint64(link->overflow.load())}, {"queued", qint64(link->queued.load())},
                    {"duplicated", qint64(link->duplicated.load())}, {"reordered", qint64(link->reordered.load())}, {"peers", peers}};
                previousBytes = bytes; samples.append(sample);
                std::cerr << QJsonDocument(sample).toJson(QJsonDocument::Compact).toStdString() << std::endl;
                Check(!link->invalid && host.Status().activePeers == 4);
                if (impaired && second == 6) {
                    Check(!responseVisible[0]);
                    const auto inputStarted = std::chrono::steady_clock::now();
                    screenshare::input::Event key; key.kind = screenshare::input::Kind::Key; key.key = 65; key.down = true;
                    Check(viewers[0]->Input()->Submit(hostId, key));
                    Wait([&] { consume(); return hostEvidence->input->applied > 0 && hostEvidence->input->pressed; });
                    inputApplied = true;
                    Wait([&] { consume(); return responseVisible[0]; });
                    inputResponseInternalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - inputStarted).count();
                    host.Input()->Revoke();
                    Wait([&] { consume(); return !granted() && !hostEvidence->input->pressed && hostEvidence->input->releases > 0; });
                    Wait([&] { consume(); return !responseVisible[0]; });
                    Check(!viewers[0]->Input()->Submit(hostId, key)); inputRevoked = true;
                }
            }
        }
        for (auto& viewer : viewers) { auto stop = viewer->Stop(); Get(stop); viewer.reset(); }
        auto stop = host.Stop(); Get(stop);
        if (!eventLogDirectory.empty()) Check(eventLogs->opened == 4 && eventLogs->closed == 4 && !eventLogs->failed);
        for (auto& frames : presentation) frames->Stop();
        Check(!link->queued && !link->bytes && !link->liveSockets && !link->invalid && link->received > 0);
        for (const auto& e : evidence) Check(e->destroyed == 1 && e->invalid == 0);
        std::cout << QJsonDocument(QJsonObject{{"schema", 2}, {"scenario", QString::fromStdString(scenario)}, {"seed", 12345},
            {"samples", samples}, {"warmupIngressBps", warmupIngress}, {"fastAudioExperiment", fastAudioExperiment},
            {"peakQueued", qint64(link->peakQueued.load())}, {"peakBytes", qint64(link->peakBytes.load())},
            {"maximumSchedulingDelayUs", qint64(link->maximumSchedulingDelayUs.load())},
            {"released", true}, {"inputApplied", inputApplied}, {"inputRevoked", inputRevoked},
            {"inputResponseInternalMs", inputResponseInternalMs ? QJsonValue(*inputResponseInternalMs) : QJsonValue(QJsonValue::Null)},
            {"inputResponseSamples", 1}, {"inputResponseEndpoint", "decoded-frame-consumption"},
            {"inputResponseScene", "localized-marker"},
            {"externalLatencyVerified", false}}).toJson(QJsonDocument::Compact).toStdString() << std::endl;
    } catch (const std::exception& error) { std::cerr << error.what() << std::endl; result = 1; }
    webrtc::CleanupSSL(); return result;
}
