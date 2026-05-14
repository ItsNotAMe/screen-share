#include "transport/LanDiscovery.h"

#include "transport/UdpCrypto.h"
#include "transport/UdpProtocol.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace screenshare {
namespace {

constexpr uint32_t DiscoveryQueryMagic = 0x53534451; // "SSDQ"
constexpr uint32_t DiscoveryResponseMagic = 0x53534452; // "SSDR"
constexpr uint16_t DiscoveryVersion = 3;
constexpr uint16_t SecurityMetadataDiscoveryVersion = 2;
constexpr uint16_t LegacyDiscoveryVersion = 1;
constexpr size_t MaxDiscoveryNameBytes = 64;
constexpr size_t MaxDiscoverySessionBytes = 64;
constexpr size_t MaxDiscoveryAccessCodeBytes = 64;
constexpr uint16_t DiscoveryFlagEncryptedAccessCode = 1U << 0;

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
    uint16_t pairingBytes = 0;
    uint16_t reserved = 0;
    std::byte pairingNonce[UdpCryptoNonceBytes]{};
    std::byte pairingTag[UdpCryptoTagBytes]{};
};
#pragma pack(pop)

static_assert(sizeof(DiscoveryQueryPacket) == 8);
static_assert(sizeof(DiscoveryResponseHeader) == 64);
constexpr size_t LegacyDiscoveryResponseHeaderBytes = 24;
constexpr size_t SecurityMetadataDiscoveryResponseHeaderBytes = 32;
constexpr size_t DiscoveryResponseAuthenticatedBytes = offsetof(DiscoveryResponseHeader, pairingTag);

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

UdpCryptoKey DeriveLanPairingKey(const std::string& pairCode)
{
    if (pairCode.empty()) {
        throw std::invalid_argument("LAN pair code must not be empty");
    }
    return DeriveUdpCryptoKey("lan-pair:" + pairCode);
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
    const bool includePairingSecret = !config.accessCode.empty() && !config.pairCode.empty();
    if (includePairingSecret && config.accessCode.size() > MaxDiscoveryAccessCodeBytes) {
        throw std::invalid_argument("LAN pairing access code is too large");
    }
    const size_t headerBytes = includePairingSecret ?
        sizeof(DiscoveryResponseHeader) :
        SecurityMetadataDiscoveryResponseHeaderBytes;
    const uint16_t packetBytes = static_cast<uint16_t>(
        headerBytes +
        name.size() +
        session.size() +
        (includePairingSecret ? config.accessCode.size() : 0));

    DiscoveryResponseHeader header;
    header.magic = udp_protocol::ToNetwork32(DiscoveryResponseMagic);
    header.version = udp_protocol::ToNetwork16(
        includePairingSecret ? DiscoveryVersion : SecurityMetadataDiscoveryVersion);
    header.packetBytes = udp_protocol::ToNetwork16(packetBytes);
    header.sharePort = udp_protocol::ToNetwork16(config.sharePort);
    header.nameBytes = udp_protocol::ToNetwork16(static_cast<uint16_t>(name.size()));
    header.sessionBytes = udp_protocol::ToNetwork16(static_cast<uint16_t>(session.size()));
    header.sessionFingerprint = udp_protocol::ToNetwork64(config.sessionFingerprint);
    header.accessCodeFingerprint = udp_protocol::ToNetwork64(config.accessCodeFingerprint);
    if (includePairingSecret) {
        header.flags = udp_protocol::ToNetwork16(DiscoveryFlagEncryptedAccessCode);
        header.pairingBytes = udp_protocol::ToNetwork16(static_cast<uint16_t>(config.accessCode.size()));
        WriteUdpCryptoNonce(
            std::span<std::byte, UdpCryptoNonceBytes>(header.pairingNonce, UdpCryptoNonceBytes),
            GenerateUdpCryptoNoncePrefix(),
            config.sessionFingerprint != 0 ? config.sessionFingerprint : config.accessCodeFingerprint,
            0);
    }

    std::vector<std::byte> datagram(packetBytes);
    std::memcpy(datagram.data(), &header, headerBytes);
    std::memcpy(datagram.data() + headerBytes, name.data(), name.size());
    std::memcpy(datagram.data() + headerBytes + name.size(), session.data(), session.size());
    if (includePairingSecret) {
        UdpAesGcm crypto(DeriveLanPairingKey(config.pairCode));
        auto ciphertext = crypto.Encrypt(
            std::span<const std::byte, UdpCryptoNonceBytes>(header.pairingNonce, UdpCryptoNonceBytes),
            std::span<const std::byte>(datagram.data(), DiscoveryResponseAuthenticatedBytes),
            std::as_bytes(std::span<const char>(config.accessCode.data(), config.accessCode.size())),
            std::span<std::byte, UdpCryptoTagBytes>(
                datagram.data() + offsetof(DiscoveryResponseHeader, pairingTag),
                UdpCryptoTagBytes));
        std::memcpy(
            datagram.data() + headerBytes + name.size() + session.size(),
            ciphertext.data(),
            ciphertext.size());
    }
    return datagram;
}

std::optional<LanDiscoveryPeer> ParseDiscoveryResponse(
    const std::byte* datagram,
    int datagramBytes,
    const sockaddr_in& senderAddress,
    const std::optional<std::string>& pairCode)
{
    if (datagramBytes < static_cast<int>(LegacyDiscoveryResponseHeaderBytes)) {
        return std::nullopt;
    }

    DiscoveryResponseHeader header{};
    std::memcpy(&header, datagram, std::min<size_t>(sizeof(header), static_cast<size_t>(datagramBytes)));
    const uint16_t version = udp_protocol::FromNetwork16(header.version);
    if (udp_protocol::FromNetwork32(header.magic) != DiscoveryResponseMagic) {
        return std::nullopt;
    }
    const size_t expectedHeaderBytes =
        version == DiscoveryVersion ? sizeof(DiscoveryResponseHeader) :
        version == SecurityMetadataDiscoveryVersion ? SecurityMetadataDiscoveryResponseHeaderBytes :
        version == LegacyDiscoveryVersion ? LegacyDiscoveryResponseHeaderBytes :
        0;
    if (expectedHeaderBytes == 0 || datagramBytes < static_cast<int>(expectedHeaderBytes)) {
        return std::nullopt;
    }

    const uint16_t packetBytes = udp_protocol::FromNetwork16(header.packetBytes);
    const uint16_t sharePort = udp_protocol::FromNetwork16(header.sharePort);
    const uint16_t nameBytes = udp_protocol::FromNetwork16(header.nameBytes);
    const uint16_t sessionBytes = udp_protocol::FromNetwork16(header.sessionBytes);
    const uint16_t flags = udp_protocol::FromNetwork16(header.flags);
    const uint16_t pairingBytes = version == DiscoveryVersion ? udp_protocol::FromNetwork16(header.pairingBytes) : 0;
    const uint16_t allowedFlags = version == DiscoveryVersion ? DiscoveryFlagEncryptedAccessCode : 0;
    if (sharePort == 0 ||
        packetBytes != datagramBytes ||
        packetBytes < expectedHeaderBytes ||
        nameBytes > MaxDiscoveryNameBytes ||
        sessionBytes > MaxDiscoverySessionBytes ||
        pairingBytes > MaxDiscoveryAccessCodeBytes ||
        (flags & ~allowedFlags) != 0 ||
        ((flags & DiscoveryFlagEncryptedAccessCode) == 0 && pairingBytes != 0) ||
        ((flags & DiscoveryFlagEncryptedAccessCode) != 0 && pairingBytes == 0) ||
        expectedHeaderBytes + nameBytes + sessionBytes + pairingBytes != packetBytes) {
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
    if (version >= SecurityMetadataDiscoveryVersion) {
        peer.accessCodeFingerprint = udp_protocol::FromNetwork64(header.accessCodeFingerprint);
    }
    peer.pairingAvailable = (flags & DiscoveryFlagEncryptedAccessCode) != 0;
    if (peer.pairingAvailable && pairCode && !pairCode->empty()) {
        try {
            UdpAesGcm crypto(DeriveLanPairingKey(*pairCode));
            const auto plaintext = crypto.Decrypt(
                std::span<const std::byte, UdpCryptoNonceBytes>(header.pairingNonce, UdpCryptoNonceBytes),
                std::span<const std::byte>(datagram, DiscoveryResponseAuthenticatedBytes),
                std::span<const std::byte>(
                    datagram + expectedHeaderBytes + nameBytes + sessionBytes,
                    pairingBytes),
                std::span<const std::byte, UdpCryptoTagBytes>(header.pairingTag, UdpCryptoTagBytes));
            if (plaintext && !plaintext->empty() && plaintext->size() <= MaxDiscoveryAccessCodeBytes) {
                const auto* accessCodeBytes = reinterpret_cast<const char*>(plaintext->data());
                const bool hasControlCharacter = std::any_of(
                    accessCodeBytes,
                    accessCodeBytes + plaintext->size(),
                    [](char ch) {
                        const auto value = static_cast<unsigned char>(ch);
                        return value < 32U || value == 127U;
                    });
                if (!hasControlCharacter) {
                    peer.pairingSucceeded = true;
                    peer.pairedAccessCode.assign(accessCodeBytes, accessCodeBytes + plaintext->size());
                }
            }
        } catch (...) {
            peer.pairingSucceeded = false;
        }
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
    std::vector<std::byte> response = BuildDiscoveryResponse(config);

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
    response_ = std::move(response);
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
    response_.clear();
    stopRequested_ = false;
}

void LanDiscoveryResponder::WorkerLoop()
{
    const SOCKET socket = AsSocket(socket_);
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
            reinterpret_cast<const char*>(response_.data()),
            static_cast<int>(response_.size()),
            0,
            reinterpret_cast<const sockaddr*>(&senderAddress),
            senderAddressLength));
    }
}

std::vector<LanDiscoveryPeer> DiscoverLanPeers(
    std::chrono::milliseconds timeout,
    uint16_t discoveryPort,
    std::optional<std::string> pairCode)
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

            auto peer = ParseDiscoveryResponse(buffer.data(), received, senderAddress, pairCode);
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
            } else if (peer->pairingSucceeded) {
                auto existing = std::find_if(peers.begin(), peers.end(), [&](const LanDiscoveryPeer& candidate) {
                    return std::tuple{candidate.address, candidate.sharePort, candidate.sessionFingerprint} == key;
                });
                if (existing != peers.end() && !existing->pairingSucceeded) {
                    *existing = std::move(*peer);
                }
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
