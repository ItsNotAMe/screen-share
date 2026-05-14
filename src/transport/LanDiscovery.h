#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace screenshare {

constexpr uint16_t LanDiscoveryDefaultPort = 47995;

struct LanDiscoveryPeer {
    std::string address;
    uint16_t sharePort = 0;
    std::string name;
    std::string sessionId;
    uint64_t sessionFingerprint = 0;
    uint64_t accessCodeFingerprint = 0;
    bool pairingAvailable = false;
    bool pairingSucceeded = false;
    std::string pairedAccessCode;
};

struct LanDiscoveryAdvertiseConfig {
    uint16_t discoveryPort = LanDiscoveryDefaultPort;
    uint16_t sharePort = 0;
    std::string name;
    std::string sessionId;
    uint64_t sessionFingerprint = 0;
    uint64_t accessCodeFingerprint = 0;
    std::string accessCode;
    std::string pairCode;
};

class LanDiscoveryResponder {
public:
    LanDiscoveryResponder();
    ~LanDiscoveryResponder();

    LanDiscoveryResponder(const LanDiscoveryResponder&) = delete;
    LanDiscoveryResponder& operator=(const LanDiscoveryResponder&) = delete;

    void Start(const LanDiscoveryAdvertiseConfig& config);
    void Stop();

    [[nodiscard]] bool isRunning() const noexcept { return socket_ != 0; }

private:
    void WorkerLoop();

    uintptr_t socket_ = 0;
    LanDiscoveryAdvertiseConfig config_{};
    std::vector<std::byte> response_;
    std::thread worker_;
    bool stopRequested_ = false;
    bool winsockStarted_ = false;
};

std::vector<LanDiscoveryPeer> DiscoverLanPeers(
    std::chrono::milliseconds timeout,
    uint16_t discoveryPort = LanDiscoveryDefaultPort,
    std::optional<std::string> pairCode = std::nullopt);

} // namespace screenshare
