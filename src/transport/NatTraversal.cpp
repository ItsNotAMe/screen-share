#include "transport/NatTraversal.h"

#include "transport/UdpProtocol.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace screenshare {
namespace {

constexpr uint32_t NatProbeMagic = 0x53534E50; // "SSNP"
constexpr uint16_t NatProbeVersion = 1;
constexpr uint16_t NatProbeTypeProbe = 1;
constexpr uint16_t NatProbeTypeReply = 2;

#pragma pack(push, 1)
struct NatProbePacket {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t packetBytes = 0;
    uint16_t type = 0;
    uint16_t reserved = 0;
    uint64_t sequence = 0;
    uint64_t sessionFingerprint = 0;
    uint64_t accessCodeFingerprint = 0;
    uint64_t sentTickMs = 0;
};
#pragma pack(pop)

static_assert(sizeof(NatProbePacket) == 44);

struct ResolvedTarget {
    sockaddr_in address{};
    bool isLocalEndpoint = false;
};

std::string WinsockErrorMessage(const char* operation)
{
    return std::string(operation) + " failed with WSA error " + std::to_string(WSAGetLastError());
}

std::string TrimInviteLine(std::string text)
{
    constexpr std::string_view prefix = "nat_invite=";
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(" \t\r\n");
    text = text.substr(first, last - first + 1);
    if (text.rfind(prefix, 0) == 0) {
        text.erase(0, prefix.size());
    }
    return text;
}

uint16_t ParsePort(const std::string& text, const std::string& fieldName)
{
    char* end = nullptr;
    const long port = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || port <= 0 || port > 65535) {
        throw std::invalid_argument("Invalid NAT invite " + fieldName + " port: " + text);
    }
    return static_cast<uint16_t>(port);
}

NatInviteEndpoint ParseInviteEndpoint(const std::string& text, const std::string& fieldName)
{
    if (text.empty()) {
        throw std::invalid_argument("NAT invite " + fieldName + " endpoint is empty");
    }

    NatInviteEndpoint endpoint;
    if (text.front() == '[') {
        const size_t endBracket = text.find(']');
        if (endBracket == std::string::npos || endBracket == 1 || endBracket + 2 >= text.size() ||
            text[endBracket + 1] != ':') {
            throw std::invalid_argument("NAT invite " + fieldName + " expects HOST:PORT");
        }
        endpoint.host = text.substr(1, endBracket - 1);
        endpoint.port = ParsePort(text.substr(endBracket + 2), fieldName);
    } else {
        const size_t separator = text.rfind(':');
        if (separator == std::string::npos || separator == 0 || separator + 1 >= text.size()) {
            throw std::invalid_argument("NAT invite " + fieldName + " expects HOST:PORT");
        }
        endpoint.host = text.substr(0, separator);
        endpoint.port = ParsePort(text.substr(separator + 1), fieldName);
    }

    if (endpoint.host.empty()) {
        throw std::invalid_argument("NAT invite " + fieldName + " host is empty");
    }
    return endpoint;
}

uint64_t ParseHex64(const std::string& text, const std::string& fieldName)
{
    if (text.empty() || text.size() > 16) {
        throw std::invalid_argument("Invalid NAT invite " + fieldName + " fingerprint: " + text);
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 16);
    if (end == text.c_str() || *end != '\0') {
        throw std::invalid_argument("Invalid NAT invite " + fieldName + " fingerprint: " + text);
    }
    return static_cast<uint64_t>(value);
}

int64_t ParseInt64(const std::string& text, const std::string& fieldName)
{
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        throw std::invalid_argument("Invalid NAT invite " + fieldName + ": " + text);
    }
    return static_cast<int64_t>(value);
}

std::string AddressToString(const sockaddr_in& address)
{
    std::array<char, INET_ADDRSTRLEN> text{};
    if (inet_ntop(AF_INET, &address.sin_addr, text.data(), static_cast<socklen_t>(text.size())) == nullptr) {
        throw std::runtime_error(WinsockErrorMessage("inet_ntop(nat-probe)"));
    }
    return text.data();
}

bool SameAddress(const sockaddr_in& lhs, const sockaddr_in& rhs)
{
    return lhs.sin_family == rhs.sin_family &&
           lhs.sin_addr.s_addr == rhs.sin_addr.s_addr &&
           lhs.sin_port == rhs.sin_port;
}

sockaddr_in ResolveEndpoint(const NatInviteEndpoint& endpoint)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* resolved = nullptr;
    const std::string port = std::to_string(endpoint.port);
    const int result = getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &resolved);
    if (result != 0 || resolved == nullptr) {
        throw std::runtime_error("getaddrinfo failed for NAT endpoint " + endpoint.host + ":" + port);
    }

    sockaddr_in address{};
    std::memcpy(&address, resolved->ai_addr, std::min<size_t>(sizeof(address), resolved->ai_addrlen));
    freeaddrinfo(resolved);
    return address;
}

bool WaitForReadable(SOCKET socket, std::chrono::milliseconds timeout)
{
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socket, &readSet);

    timeval wait{};
    wait.tv_sec = static_cast<long>(timeout.count() / 1000);
    wait.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    const int result = select(0, &readSet, nullptr, nullptr, &wait);
    if (result == SOCKET_ERROR) {
        throw std::runtime_error(WinsockErrorMessage("select(nat-probe)"));
    }
    return result > 0 && FD_ISSET(socket, &readSet);
}

uint64_t TickMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

NatProbePacket BuildProbePacket(
    uint16_t type,
    uint64_t sequence,
    uint64_t sessionFingerprint,
    uint64_t accessCodeFingerprint)
{
    NatProbePacket packet;
    packet.magic = udp_protocol::ToNetwork32(NatProbeMagic);
    packet.version = udp_protocol::ToNetwork16(NatProbeVersion);
    packet.packetBytes = udp_protocol::ToNetwork16(static_cast<uint16_t>(sizeof(packet)));
    packet.type = udp_protocol::ToNetwork16(type);
    packet.sequence = udp_protocol::ToNetwork64(sequence);
    packet.sessionFingerprint = udp_protocol::ToNetwork64(sessionFingerprint);
    packet.accessCodeFingerprint = udp_protocol::ToNetwork64(accessCodeFingerprint);
    packet.sentTickMs = udp_protocol::ToNetwork64(TickMs());
    return packet;
}

std::optional<NatProbePacket> ParseProbePacket(const std::byte* datagram, int datagramBytes)
{
    if (datagramBytes != static_cast<int>(sizeof(NatProbePacket))) {
        return std::nullopt;
    }

    NatProbePacket packet;
    std::memcpy(&packet, datagram, sizeof(packet));
    packet.magic = udp_protocol::FromNetwork32(packet.magic);
    packet.version = udp_protocol::FromNetwork16(packet.version);
    packet.packetBytes = udp_protocol::FromNetwork16(packet.packetBytes);
    packet.type = udp_protocol::FromNetwork16(packet.type);
    packet.sequence = udp_protocol::FromNetwork64(packet.sequence);
    packet.sessionFingerprint = udp_protocol::FromNetwork64(packet.sessionFingerprint);
    packet.accessCodeFingerprint = udp_protocol::FromNetwork64(packet.accessCodeFingerprint);
    packet.sentTickMs = udp_protocol::FromNetwork64(packet.sentTickMs);

    if (packet.magic != NatProbeMagic ||
        packet.version != NatProbeVersion ||
        packet.packetBytes != sizeof(NatProbePacket) ||
        (packet.type != NatProbeTypeProbe && packet.type != NatProbeTypeReply)) {
        return std::nullopt;
    }
    return packet;
}

void AddSeenEndpoint(NatProbeStats& stats, const sockaddr_in& address)
{
    NatProbeSeenEndpoint seen;
    seen.address = AddressToString(address);
    seen.port = ntohs(address.sin_port);
    const auto found = std::find_if(
        stats.seenEndpoints.begin(),
        stats.seenEndpoints.end(),
        [&](const NatProbeSeenEndpoint& existing) {
            return existing.address == seen.address && existing.port == seen.port;
        });
    if (found == stats.seenEndpoints.end()) {
        stats.seenEndpoints.push_back(std::move(seen));
    }
}

void SendPacket(SOCKET socket, const sockaddr_in& target, const NatProbePacket& packet)
{
    const int sent = sendto(
        socket,
        reinterpret_cast<const char*>(&packet),
        static_cast<int>(sizeof(packet)),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target));
    if (sent == SOCKET_ERROR) {
        throw std::runtime_error(WinsockErrorMessage("sendto(nat-probe)"));
    }
}

} // namespace

NatInvite ParseNatInvite(const std::string& text)
{
    const std::string inviteText = TrimInviteLine(text);
    if (inviteText.empty()) {
        throw std::invalid_argument("NAT invite is empty");
    }

    std::vector<std::string> fields;
    size_t offset = 0;
    while (offset <= inviteText.size()) {
        const size_t separator = inviteText.find(';', offset);
        if (separator == std::string::npos) {
            fields.push_back(inviteText.substr(offset));
            break;
        }
        fields.push_back(inviteText.substr(offset, separator - offset));
        offset = separator + 1;
    }

    if (fields.empty() || fields.front() != "screenshare-invite-v1") {
        throw std::invalid_argument("Unsupported NAT invite format");
    }

    std::map<std::string, std::string> values;
    for (size_t i = 1; i < fields.size(); ++i) {
        const size_t equals = fields[i].find('=');
        if (equals == std::string::npos || equals == 0 || equals + 1 > fields[i].size()) {
            throw std::invalid_argument("Invalid NAT invite field: " + fields[i]);
        }
        values[fields[i].substr(0, equals)] = fields[i].substr(equals + 1);
    }

    auto require = [&](const std::string& name) -> const std::string& {
        const auto found = values.find(name);
        if (found == values.end() || found->second.empty()) {
            throw std::invalid_argument("NAT invite is missing " + name);
        }
        return found->second;
    };

    NatInvite invite;
    invite.publicEndpoint = ParseInviteEndpoint(require("public"), "public");
    invite.localEndpoint = ParseInviteEndpoint(require("local"), "local");
    if (const auto found = values.find("stun"); found != values.end() && !found->second.empty()) {
        invite.stunEndpoint = ParseInviteEndpoint(found->second, "stun");
    }
    invite.sessionId = require("session");
    invite.sessionFingerprint = ParseHex64(require("session_fingerprint"), "session");

    const std::string& security = require("security");
    if (security == "encrypted") {
        invite.encrypted = true;
        invite.accessCodeFingerprint = ParseHex64(require("access_fingerprint"), "access");
        if (invite.accessCodeFingerprint == 0) {
            throw std::invalid_argument("Encrypted NAT invite has an empty access-code fingerprint");
        }
    } else if (security == "plaintext") {
        invite.encrypted = false;
        const auto found = values.find("access_fingerprint");
        if (found != values.end() && found->second != "none") {
            invite.accessCodeFingerprint = ParseHex64(found->second, "access");
        }
    } else {
        throw std::invalid_argument("Invalid NAT invite security value: " + security);
    }

    invite.expiresUnix = ParseInt64(require("expires_unix"), "expires_unix");
    return invite;
}

NatProbeStats RunNatProbeExchange(const NatProbeConfig& config)
{
    if (config.localPort == 0) {
        throw std::invalid_argument("NAT probe local port must be non-zero");
    }
    if (config.peerInvite.publicEndpoint.host.empty() || config.peerInvite.publicEndpoint.port == 0) {
        throw std::invalid_argument("NAT probe peer public endpoint is missing");
    }
    if (config.duration <= std::chrono::milliseconds(0)) {
        throw std::invalid_argument("NAT probe duration must be positive");
    }
    if (config.interval < std::chrono::milliseconds(50) || config.interval > std::chrono::seconds(5)) {
        throw std::invalid_argument("NAT probe interval must be between 50 and 5000 ms");
    }

    WSADATA data{};
    const int startupResult = WSAStartup(MAKEWORD(2, 2), &data);
    if (startupResult != 0) {
        throw std::runtime_error("WSAStartup failed with WSA error " + std::to_string(startupResult));
    }

    SOCKET udpSocket = INVALID_SOCKET;
    try {
        udpSocket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udpSocket == INVALID_SOCKET) {
            throw std::runtime_error(WinsockErrorMessage("socket(nat-probe)"));
        }

        sockaddr_in bindAddress{};
        bindAddress.sin_family = AF_INET;
        bindAddress.sin_addr.s_addr = htonl(INADDR_ANY);
        bindAddress.sin_port = htons(config.localPort);
        if (bind(udpSocket, reinterpret_cast<const sockaddr*>(&bindAddress), sizeof(bindAddress)) == SOCKET_ERROR) {
            throw std::runtime_error(WinsockErrorMessage("bind(nat-probe)"));
        }

        std::vector<ResolvedTarget> targets;
        targets.push_back(ResolvedTarget{ResolveEndpoint(config.peerInvite.publicEndpoint), false});
        if (!config.peerInvite.localEndpoint.host.empty() && config.peerInvite.localEndpoint.port != 0) {
            const sockaddr_in localTarget = ResolveEndpoint(config.peerInvite.localEndpoint);
            const bool duplicate = std::any_of(
                targets.begin(),
                targets.end(),
                [&](const ResolvedTarget& existing) { return SameAddress(existing.address, localTarget); });
            if (!duplicate) {
                targets.push_back(ResolvedTarget{localTarget, true});
            }
        }

        NatProbeStats stats;
        const auto deadline = std::chrono::steady_clock::now() + config.duration;
        auto nextSendAt = std::chrono::steady_clock::now();
        uint64_t sequence = 1;
        std::array<std::byte, 512> buffer{};

        while (std::chrono::steady_clock::now() < deadline) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextSendAt) {
                const NatProbePacket packet = BuildProbePacket(
                    NatProbeTypeProbe,
                    sequence++,
                    config.sessionFingerprint,
                    config.accessCodeFingerprint);
                for (const auto& target : targets) {
                    SendPacket(udpSocket, target.address, packet);
                    if (target.isLocalEndpoint) {
                        ++stats.sentLocalProbes;
                    } else {
                        ++stats.sentPublicProbes;
                    }
                }
                nextSendAt = now + config.interval;
            }

            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            const auto sendWait = std::chrono::duration_cast<std::chrono::milliseconds>(nextSendAt - now);
            const auto positiveSendWait = std::max(std::chrono::milliseconds(1), sendWait);
            const auto wait = std::min({remaining, positiveSendWait, std::chrono::milliseconds(100)});
            if (wait <= std::chrono::milliseconds(0) || !WaitForReadable(udpSocket, wait)) {
                continue;
            }

            sockaddr_in senderAddress{};
            int senderAddressLength = sizeof(senderAddress);
            const int received = recvfrom(
                udpSocket,
                reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()),
                0,
                reinterpret_cast<sockaddr*>(&senderAddress),
                &senderAddressLength);
            if (received == SOCKET_ERROR) {
                continue;
            }

            const auto packet = ParseProbePacket(buffer.data(), received);
            if (!packet) {
                ++stats.invalidPackets;
                continue;
            }

            ++stats.receivedPackets;
            if (packet->type == NatProbeTypeProbe) {
                ++stats.receivedProbes;
            } else {
                ++stats.receivedReplies;
            }
            if (config.sessionFingerprint != 0 && packet->sessionFingerprint != config.sessionFingerprint) {
                ++stats.sessionMismatches;
            }
            if (config.accessCodeFingerprint != 0 && packet->accessCodeFingerprint != config.accessCodeFingerprint) {
                ++stats.accessCodeMismatches;
            }
            AddSeenEndpoint(stats, senderAddress);

            if (packet->type == NatProbeTypeProbe) {
                const NatProbePacket reply = BuildProbePacket(
                    NatProbeTypeReply,
                    packet->sequence,
                    config.sessionFingerprint,
                    config.accessCodeFingerprint);
                SendPacket(udpSocket, senderAddress, reply);
                ++stats.repliesSent;
            }
        }

        closesocket(udpSocket);
        WSACleanup();
        return stats;
    } catch (...) {
        if (udpSocket != INVALID_SOCKET) {
            closesocket(udpSocket);
        }
        WSACleanup();
        throw;
    }
}

} // namespace screenshare
