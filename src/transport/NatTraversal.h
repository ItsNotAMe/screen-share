#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace screenshare {

struct NatInviteEndpoint {
    std::string host;
    uint16_t port = 0;
};

struct NatInvite {
    std::string sessionId;
    uint64_t sessionFingerprint = 0;
    uint64_t accessCodeFingerprint = 0;
    bool encrypted = false;
    int64_t expiresUnix = 0;
    NatInviteEndpoint publicEndpoint;
    NatInviteEndpoint localEndpoint;
    NatInviteEndpoint stunEndpoint;
};

struct NatProbeConfig {
    uint16_t localPort = 0;
    NatInvite peerInvite;
    std::chrono::milliseconds duration{10'000};
    std::chrono::milliseconds interval{250};
    uint64_t sessionFingerprint = 0;
    uint64_t accessCodeFingerprint = 0;
};

struct NatProbeSeenEndpoint {
    std::string address;
    uint16_t port = 0;
};

struct NatProbeStats {
    uint64_t sentPublicProbes = 0;
    uint64_t sentLocalProbes = 0;
    uint64_t receivedPackets = 0;
    uint64_t receivedProbes = 0;
    uint64_t receivedReplies = 0;
    uint64_t invalidPackets = 0;
    uint64_t sessionMismatches = 0;
    uint64_t accessCodeMismatches = 0;
    uint64_t repliesSent = 0;
    std::vector<NatProbeSeenEndpoint> seenEndpoints;
};

NatInvite ParseNatInvite(const std::string& text);
NatProbeStats RunNatProbeExchange(const NatProbeConfig& config);

} // namespace screenshare
