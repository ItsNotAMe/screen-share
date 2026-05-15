#include "transport/LanDiscovery.h"

#include "transport/UdpProtocol.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace screenshare {
namespace {

constexpr uint32_t DiscoveryQueryMagic = 0x53534451; // "SSDQ"
constexpr uint32_t DiscoveryResponseMagic = 0x53534452; // "SSDR"
constexpr uint16_t DiscoveryVersion = 2;
constexpr uint16_t LegacyDiscoveryVersion = 1;
constexpr size_t MaxDiscoveryNameBytes = 64;
constexpr size_t MaxDiscoverySessionBytes = 64;

#pragma pack(push, 1)
struct DiscoveryQueryPacket {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t packetBytes = 0;
};

struct DiscoveryResponseHeader {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t packetBytes = 0;
    uint16_t sharePort = 0;
    uint16_t nameBytes = 0;
    uint16_t sessionBytes = 0;
    uint16_t flags = 0;
    uint64_t sessionFingerprint = 0;
    uint64_t accessCodeFingerprint = 0;
};
#pragma pack(pop)

static_assert(sizeof(DiscoveryQueryPacket) == 8);
static_assert(sizeof(DiscoveryResponseHeader) == 32);
constexpr size_t LegacyDiscoveryResponseHeaderBytes = 24;

std::string WinsockErrorMessage(const char* operation)
{
    return std::string(operation) + " failed with WSA error " + std::to_string(WSAGetLastError());
}

SOCKET AsSocket(uintptr_t socket)
{
    return static_cast<SOCKET>(socket);
}

std::string TruncateUtf8Bytes(std::string value, size_t maxBytes)
{
    if (value.size() <= maxBytes) {
        return value;
    }
    value.resize(maxBytes);
    return value;
}

std::vector<std::byte> BuildDiscoveryQuery()
{
    DiscoveryQueryPacket packet;
    packet.magic = udp_protocol::ToNetwork32(DiscoveryQueryMagic);
    packet.version = udp_protocol::ToNetwork16(LegacyDiscoveryVersion);
    packet.packetBytes = udp_protocol::ToNetwork16(static_cast<uint16_t>(sizeof(packet)));

    std::vector<std::byte> datagram(sizeof(packet));
    std::memcpy(datagram.data(), &packet, sizeof(packet));
    return datagram;
}

bool IsDiscoveryQuery(const std::byte* datagram, int datagramBytes)
{
    if (datagramBytes < static_cast<int>(sizeof(DiscoveryQueryPacket))) {
        return false;
    }

    DiscoveryQueryPacket packet;
    std::memcpy(&packet, datagram, sizeof(packet));
    const uint16_t version = udp_protocol::FromNetwork16(packet.version);
    return udp_protocol::FromNetwork32(packet.magic) == DiscoveryQueryMagic &&
           (version == LegacyDiscoveryVersion || version == DiscoveryVersion) &&
           udp_protocol::FromNetwork16(packet.packetBytes) == sizeof(DiscoveryQueryPacket);
}

std::vector<std::byte> BuildDiscoveryResponse(const LanDiscoveryAdvertiseConfig& config)
{
    const std::string name = TruncateUtf8Bytes(config.name, MaxDiscoveryNameBytes);
    const std::string session = TruncateUtf8Bytes(config.sessionId, MaxDiscoverySessionBytes);
    const uint16_t packetBytes = static_cast<uint16_t>(
        sizeof(DiscoveryResponseHeader) + name.size() + session.size());

    DiscoveryResponseHeader header;
    header.magic = udp_protocol::ToNetwork32(DiscoveryResponseMagic);
    header.version = udp_protocol::ToNetwork16(DiscoveryVersion);
    header.packetBytes = udp_protocol::ToNetwork16(packetBytes);
    header.sharePort = udp_protocol::ToNetwork16(config.sharePort);
    header.nameBytes = udp_protocol::ToNetwork16(static_cast<uint16_t>(name.size()));
    header.sessionBytes = udp_protocol::ToNetwork16(static_cast<uint16_t>(session.size()));
    header.sessionFingerprint = udp_protocol::ToNetwork64(config.sessionFingerprint);
    header.accessCodeFingerprint = udp_protocol::ToNetwork64(config.accessCodeFingerprint);

    std::vector<std::byte> datagram(packetBytes);
    std::memcpy(datagram.data(), &header, sizeof(header));
    std::memcpy(datagram.data() + sizeof(header), name.data(), name.size());
    std::memcpy(datagram.data() + sizeof(header) + name.size(), session.data(), session.size());
    return datagram;
}

std::optional<LanDiscoveryPeer> ParseDiscoveryResponse(
    const std::byte* datagram,
    int datagramBytes,
    const sockaddr_in& senderAddress)
{
    if (datagramBytes < static_cast<int>(LegacyDiscoveryResponseHeaderBytes)) {
        return std::nullopt;
    }

    DiscoveryResponseHeader header;
    std::memcpy(&header, datagram, std::min<size_t>(sizeof(header), static_cast<size_t>(datagramBytes)));
    const uint16_t version = udp_protocol::FromNetwork16(header.version);
    if (udp_protocol::FromNetwork32(header.magic) != DiscoveryResponseMagic) {
        return std::nullopt;
    }
    const size_t expectedHeaderBytes =
        version == DiscoveryVersion ? sizeof(DiscoveryResponseHeader) :
        version == LegacyDiscoveryVersion ? LegacyDiscoveryResponseHeaderBytes :
        0;
    if (expectedHeaderBytes == 0 || datagramBytes < static_cast<int>(expectedHeaderBytes)) {
        return std::nullopt;
    }

    const uint16_t packetBytes = udp_protocol::FromNetwork16(header.packetBytes);
    const uint16_t sharePort = udp_protocol::FromNetwork16(header.sharePort);
    const uint16_t nameBytes = udp_protocol::FromNetwork16(header.nameBytes);
    const uint16_t sessionBytes = udp_protocol::FromNetwork16(header.sessionBytes);
    if (sharePort == 0 ||
        packetBytes != datagramBytes ||
        packetBytes < expectedHeaderBytes ||
        nameBytes > MaxDiscoveryNameBytes ||
        sessionBytes > MaxDiscoverySessionBytes ||
        expectedHeaderBytes + nameBytes + sessionBytes != packetBytes) {
        return std::nullopt;
    }

    std::array<char, INET_ADDRSTRLEN> addressText{};
    if (inet_ntop(AF_INET, &senderAddress.sin_addr, addressText.data(), static_cast<socklen_t>(addressText.size())) == nullptr) {
        return std::nullopt;
    }

    const char* payload = reinterpret_cast<const char*>(datagram + expectedHeaderBytes);
    LanDiscoveryPeer peer;
    peer.address = addressText.data();
    peer.sharePort = sharePort;
    peer.name.assign(payload, payload + nameBytes);
    peer.sessionId.assign(payload + nameBytes, payload + nameBytes + sessionBytes);
    peer.sessionFingerprint = udp_protocol::FromNetwork64(header.sessionFingerprint);
    if (version == DiscoveryVersion) {
        peer.accessCodeFingerprint = udp_protocol::FromNetwork64(header.accessCodeFingerprint);
    }
    return peer;
}

void StartWinsock(bool& started)
{
    if (started) {
        return;
    }

    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        throw std::runtime_error("WSAStartup failed with WSA error " + std::to_string(result));
    }
    started = true;
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
        throw std::runtime_error(WinsockErrorMessage("select(lan-discovery)"));
    }
    return result > 0 && FD_ISSET(socket, &readSet);
}

void AddUniqueAddress(std::vector<uint32_t>& addresses, uint32_t address)
{
    if (std::find(addresses.begin(), addresses.end(), address) == addresses.end()) {
        addresses.push_back(address);
    }
}

std::vector<uint32_t> DiscoveryTargetAddresses()
{
    std::vector<uint32_t> addresses;
    AddUniqueAddress(addresses, htonl(INADDR_BROADCAST));
    AddUniqueAddress(addresses, htonl(INADDR_LOOPBACK));

    ULONG bufferBytes = 15 * 1024;
    std::vector<unsigned char> buffer(bufferBytes);
    constexpr ULONG flags =
        GAA_FLAG_SKIP_ANYCAST |
        GAA_FLAG_SKIP_MULTICAST |
        GAA_FLAG_SKIP_DNS_SERVER;

    ULONG result = GetAdaptersAddresses(
        AF_INET,
        flags,
        nullptr,
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
        &bufferBytes);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.assign(bufferBytes, 0);
        result = GetAdaptersAddresses(
            AF_INET,
            flags,
            nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
            &bufferBytes);
    }
    if (result != NO_ERROR) {
        return addresses;
    }

    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp ||
            adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }

        for (auto* unicast = adapter->FirstUnicastAddress;
             unicast != nullptr;
             unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr == nullptr ||
                unicast->Address.lpSockaddr->sa_family != AF_INET ||
                unicast->OnLinkPrefixLength > 31) {
                continue;
            }

            const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            const uint32_t hostAddress = ntohl(ipv4->sin_addr.s_addr);
            if ((hostAddress >> 24) == 127U) {
                continue;
            }

            const uint32_t mask = unicast->OnLinkPrefixLength == 0 ?
                0U :
                (0xFFFFFFFFU << (32U - unicast->OnLinkPrefixLength));
            const uint32_t directedBroadcast = (hostAddress & mask) | ~mask;
            if (directedBroadcast == hostAddress) {
                continue;
            }

            AddUniqueAddress(addresses, htonl(directedBroadcast));
        }
    }

    return addresses;
}

} // namespace

LanDiscoveryResponder::LanDiscoveryResponder()
{
    StartWinsock(winsockStarted_);
}

LanDiscoveryResponder::~LanDiscoveryResponder()
{
    Stop();
    if (winsockStarted_) {
        WSACleanup();
    }
}

void LanDiscoveryResponder::Start(const LanDiscoveryAdvertiseConfig& config)
{
    Stop();

    if (config.discoveryPort == 0) {
        throw std::invalid_argument("LAN discovery port must be non-zero");
    }
    if (config.sharePort == 0) {
        throw std::invalid_argument("LAN advertised share port must be non-zero");
    }

    const SOCKET udpSocket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET) {
        throw std::runtime_error(WinsockErrorMessage("socket(lan-discovery)"));
    }

    const BOOL reuseAddress = TRUE;
    static_cast<void>(setsockopt(
        udpSocket,
        SOL_SOCKET,
        SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuseAddress),
        sizeof(reuseAddress)));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(config.discoveryPort);

    if (bind(udpSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(udpSocket);
        throw std::runtime_error(WinsockErrorMessage("bind(lan-discovery)"));
    }

    config_ = config;
    socket_ = static_cast<uintptr_t>(udpSocket);
    stopRequested_ = false;
    worker_ = std::thread(&LanDiscoveryResponder::WorkerLoop, this);
}

void LanDiscoveryResponder::Stop()
{
    stopRequested_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }

    if (socket_ != 0) {
        closesocket(AsSocket(socket_));
        socket_ = 0;
    }
    stopRequested_ = false;
}

void LanDiscoveryResponder::WorkerLoop()
{
    const SOCKET socket = AsSocket(socket_);
    const std::vector<std::byte> response = BuildDiscoveryResponse(config_);
    std::array<std::byte, 512> buffer{};

    while (!stopRequested_) {
        bool readable = false;
        try {
            readable = WaitForReadable(socket, std::chrono::milliseconds(200));
        } catch (...) {
            return;
        }
        if (!readable) {
            continue;
        }

        sockaddr_in senderAddress{};
        int senderAddressLength = sizeof(senderAddress);
        const int received = recvfrom(
            socket,
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&senderAddress),
            &senderAddressLength);
        if (received == SOCKET_ERROR || !IsDiscoveryQuery(buffer.data(), received)) {
            continue;
        }

        static_cast<void>(sendto(
            socket,
            reinterpret_cast<const char*>(response.data()),
            static_cast<int>(response.size()),
            0,
            reinterpret_cast<const sockaddr*>(&senderAddress),
            senderAddressLength));
    }
}

std::vector<LanDiscoveryPeer> DiscoverLanPeers(std::chrono::milliseconds timeout, uint16_t discoveryPort)
{
    if (timeout <= std::chrono::milliseconds(0)) {
        throw std::invalid_argument("LAN discovery timeout must be positive");
    }
    if (discoveryPort == 0) {
        throw std::invalid_argument("LAN discovery port must be non-zero");
    }

    bool winsockStarted = false;
    StartWinsock(winsockStarted);

    const SOCKET udpSocket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET) {
        if (winsockStarted) {
            WSACleanup();
        }
        throw std::runtime_error(WinsockErrorMessage("socket(lan-discover)"));
    }

    try {
        const BOOL broadcast = TRUE;
        static_cast<void>(setsockopt(
            udpSocket,
            SOL_SOCKET,
            SO_BROADCAST,
            reinterpret_cast<const char*>(&broadcast),
            sizeof(broadcast)));

        sockaddr_in bindAddress{};
        bindAddress.sin_family = AF_INET;
        bindAddress.sin_addr.s_addr = htonl(INADDR_ANY);
        bindAddress.sin_port = htons(0);
        if (bind(udpSocket, reinterpret_cast<const sockaddr*>(&bindAddress), sizeof(bindAddress)) == SOCKET_ERROR) {
            throw std::runtime_error(WinsockErrorMessage("bind(lan-discover)"));
        }

        const std::vector<std::byte> query = BuildDiscoveryQuery();
        auto sendQuery = [&](uint32_t address) {
            sockaddr_in target{};
            target.sin_family = AF_INET;
            target.sin_addr.s_addr = address;
            target.sin_port = htons(discoveryPort);
            static_cast<void>(sendto(
                udpSocket,
                reinterpret_cast<const char*>(query.data()),
                static_cast<int>(query.size()),
                0,
                reinterpret_cast<const sockaddr*>(&target),
                sizeof(target)));
        };
        for (const uint32_t address : DiscoveryTargetAddresses()) {
            sendQuery(address);
        }

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::array<std::byte, 512> buffer{};
        std::vector<LanDiscoveryPeer> peers;

        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (!WaitForReadable(udpSocket, std::min(remaining, std::chrono::milliseconds(200)))) {
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

            auto peer = ParseDiscoveryResponse(buffer.data(), received, senderAddress);
            if (!peer) {
                continue;
            }

            const auto key = std::tuple{
                peer->address,
                peer->sharePort,
                peer->sessionFingerprint,
            };
            const auto duplicate = std::any_of(peers.begin(), peers.end(), [&](const LanDiscoveryPeer& existing) {
                return std::tuple{existing.address, existing.sharePort, existing.sessionFingerprint} == key;
            });
            if (!duplicate) {
                peers.push_back(std::move(*peer));
            }
        }

        std::sort(peers.begin(), peers.end(), [](const LanDiscoveryPeer& lhs, const LanDiscoveryPeer& rhs) {
            return std::tuple{lhs.name, lhs.address, lhs.sharePort} <
                   std::tuple{rhs.name, rhs.address, rhs.sharePort};
        });

        closesocket(udpSocket);
        if (winsockStarted) {
            WSACleanup();
        }
        return peers;
    } catch (...) {
        closesocket(udpSocket);
        if (winsockStarted) {
            WSACleanup();
        }
        throw;
    }
}

} // namespace screenshare
