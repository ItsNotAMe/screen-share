#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace screenshare {

constexpr uint16_t StunDefaultPort = 3478;

struct StunServerTarget {
    std::string host;
    uint16_t port = StunDefaultPort;
};

struct StunQueryConfig {
    StunServerTarget server;
    std::chrono::milliseconds timeout{3000};
};

struct StunQueryResult {
    std::string publicAddress;
    uint16_t publicPort = 0;
    std::string localAddress;
    uint16_t localPort = 0;
    std::string serverAddress;
    uint16_t serverPort = 0;
};

StunServerTarget ParseStunServerTarget(const std::string& target);
StunQueryResult QueryPublicUdpEndpoint(const StunQueryConfig& config);

} // namespace screenshare
