#include "transport/UdpSender.h"

#include <chrono>
#include <cstddef>
#include <iostream>
#include <thread>

namespace {

bool Check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

bool WaitForQueuedViewerLanes(screenshare::UdpSender& sender)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto stats = sender.stats();
        if (stats.viewerLanes.size() == 2 &&
            stats.viewerLanes[0].pendingDatagrams > 1 &&
            stats.viewerLanes[1].pendingDatagrams > 1) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool WaitForAllViewerLanesToDrain(screenshare::UdpSender& sender)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto stats = sender.stats();
        if (stats.viewerLanes.size() == 2 &&
            stats.viewerLanes[0].pendingDatagrams == 0 &&
            stats.viewerLanes[1].pendingDatagrams == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

} // namespace

int main()
{
    using namespace screenshare;

    UdpSenderConfig config;
    config.host = "127.0.0.1";
    config.port = 49171;
    config.group = 1;
    config.additionalTargets.push_back(UdpSenderEndpoint{"127.0.0.1", 49172, 2});
    config.maxPayloadBytes = 1'000;
    config.pacingBitrate = 8'000;
    config.maxQueuedDatagrams = 128;
    config.maxQueueDelay = std::chrono::seconds(30);
    config.pacingEnabled = true;

    UdpSender sender;
    sender.Open(config);

    EncodedPacket packet;
    packet.bytes.resize(12'000, std::byte{0x2a});
    sender.SendFrame(packet);

    bool ok = Check(
        WaitForQueuedViewerLanes(sender),
        "primary and additional viewer lanes build paced queues");

    const auto switchedAt = std::chrono::steady_clock::now();
    sender.SetPacingEnabled(false);
    ok &= Check(
        WaitForAllViewerLanesToDrain(sender),
        "runtime Low Latency drains every viewer lane immediately");
    ok &= Check(
        std::chrono::steady_clock::now() - switchedAt < std::chrono::seconds(1),
        "runtime Low Latency does not retain old paced deadlines");

    sender.Close();
    return ok ? 0 : 1;
}
