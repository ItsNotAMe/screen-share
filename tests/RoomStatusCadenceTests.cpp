#include "api/RoomSession.h"
#include <QCoreApplication>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace screenshare::v2;
using namespace std::chrono_literals;
namespace {
struct Evidence { std::atomic<unsigned> advances{0}, snapshots{0}; };
class Runtime final : public RoomRuntime {
    std::shared_ptr<Evidence> evidence_;
public:
    explicit Runtime(std::shared_ptr<Evidence> evidence) : evidence_(std::move(evidence)) {}
    void Advance() override { ++evidence_->advances; }
    bool Ready(const std::string&) override { return true; }
    bool Add(const std::string&) override { return true; }
    void Remove(const std::string&) noexcept override {}
    bool Receive(const std::string&, screenshare::media::RoomPeerSignal) override { return true; }
    StreamStatus StreamSettings() const override {
        StreamStatus result; result.requestedRevision = ++evidence_->snapshots;
        // Representative multi-viewer snapshot; no media devices are opened.
        for (unsigned i = 0; i < 4; ++i) {
            PeerStreamStatus peer; peer.peerId = "viewer-" + std::to_string(i);
            result.peers.push_back(std::move(peer));
            screenshare::media::PeerConnectionStatus connection;
            for (unsigned field = 0; field < 40; ++field)
                connection.current.numbers["metric-" + std::to_string(field)] = field;
            result.connections.push_back(std::move(connection));
        }
        return result;
    }
    std::shared_future<void> BeginStop() override {
        std::promise<void> stopped; stopped.set_value(); return stopped.get_future().share();
    }
};
template<class F> void Wait(F condition) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Room status cadence timed out");
        QCoreApplication::processEvents(); std::this_thread::sleep_for(1ms);
    }
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2) return 1;
    try {
        auto evidence = std::make_shared<Evidence>();
        RoomSession room([evidence](auto, auto) { return std::make_unique<Runtime>(evidence); }, true);
        RoomOptions options; options.origin = argv[1]; options.host = true;
        options.name = "Status cadence"; options.nickname = "Host";
        auto started = room.Start(options);
        Wait([&] { return started.wait_for(0ms) == std::future_status::ready; });
        if (started.get().error != RoomError::None) throw std::runtime_error("Room admission failed");
        const auto snapshots = evidence->snapshots.load(), advances = evidence->advances.load();
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        Wait([&] { return std::chrono::steady_clock::now() >= deadline; });
        const auto built = evidence->snapshots.load() - snapshots, ticks = evidence->advances.load() - advances;
        if (built < 5 || built > 15 || ticks < 50 || room.Status().stream.peers.size() != 4)
            throw std::runtime_error("Diagnostics must refresh at UI cadence while media/control keep advancing; advances=" +
                std::to_string(ticks) + ", snapshots=" + std::to_string(built));
        auto stopped = room.Stop();
        Wait([&] { return stopped.wait_for(0ms) == std::future_status::ready; }); stopped.get();
        if (room.Status().phase != RoomPhase::Stopped) throw std::runtime_error("Room did not stop");
        std::cout << "{\"passed\":true,\"advances\":" << ticks << ",\"snapshots\":" << built << "}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
