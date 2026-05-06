#include "UdpSender.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace screenshare {
namespace {

constexpr uint32_t PacketMagic = 0x53535631; // "SSV1"
constexpr uint16_t PacketVersion = 1;

#pragma pack(push, 1)
struct UdpVideoPacketHeader {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t headerBytes = 0;
    uint64_t frameId = 0;
    uint64_t timestamp100ns = 0;
    uint32_t frameBytes = 0;
    uint16_t fragmentIndex = 0;
    uint16_t fragmentCount = 0;
    uint32_t payloadBytes = 0;
};
#pragma pack(pop)

static_assert(sizeof(UdpVideoPacketHeader) == 36);

uint64_t HostToNetwork64(uint64_t value)
{
    const uint32_t high = htonl(static_cast<uint32_t>(value >> 32));
    const uint32_t low = htonl(static_cast<uint32_t>(value & 0xFFFFFFFFULL));
    return (static_cast<uint64_t>(low) << 32) | high;
}

std::string WinsockErrorMessage(const char* operation)
{
    return std::string(operation) + " failed with WSA error " + std::to_string(WSAGetLastError());
}

SOCKET AsSocket(uintptr_t socket)
{
    return static_cast<SOCKET>(socket);
}

} // namespace

UdpSender::UdpSender()
{
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        throw std::runtime_error("WSAStartup failed with WSA error " + std::to_string(result));
    }

    winsockStarted_ = true;
}

UdpSender::~UdpSender()
{
    Close();

    if (winsockStarted_) {
        WSACleanup();
    }
}

void UdpSender::Open(const UdpSenderConfig& config)
{
    Close();

    if (config.host.empty()) {
        throw std::invalid_argument("UDP host is empty");
    }
    if (config.port == 0) {
        throw std::invalid_argument("UDP port must be non-zero");
    }
    if (config.maxPayloadBytes == 0 || config.maxPayloadBytes > 60'000) {
        throw std::invalid_argument("UDP max payload must be between 1 and 60000 bytes");
    }

    config_ = config;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* resolved = nullptr;
    const std::string port = std::to_string(config.port);
    const int addressResult = getaddrinfo(config.host.c_str(), port.c_str(), &hints, &resolved);
    if (addressResult != 0 || resolved == nullptr) {
        throw std::runtime_error("getaddrinfo failed for UDP target " + config.host + ":" + port);
    }

    const SOCKET udpSocket = ::socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
    if (udpSocket == INVALID_SOCKET) {
        freeaddrinfo(resolved);
        throw std::runtime_error(WinsockErrorMessage("socket"));
    }

    address_.resize(static_cast<size_t>(resolved->ai_addrlen));
    std::memcpy(address_.data(), resolved->ai_addr, static_cast<size_t>(resolved->ai_addrlen));

    socket_ = static_cast<uintptr_t>(udpSocket);
    addressLength_ = static_cast<int>(resolved->ai_addrlen);

    freeaddrinfo(resolved);
}

void UdpSender::Close()
{
    if (socket_ != 0) {
        closesocket(AsSocket(socket_));
        socket_ = 0;
    }

    address_.clear();
    addressLength_ = 0;
}

void UdpSender::SendFrame(const EncodedPacket& packet)
{
    if (!isOpen()) {
        throw std::logic_error("UdpSender::Open must be called before SendFrame");
    }
    if (packet.bytes.empty()) {
        return;
    }

    if (packet.bytes.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Encoded frame is too large for UDP sender");
    }

    const uint32_t maxPayload = config_.maxPayloadBytes;
    const uint32_t frameBytes = static_cast<uint32_t>(packet.bytes.size());
    const uint32_t fragmentCount32 = (frameBytes + maxPayload - 1) / maxPayload;

    if (fragmentCount32 == 0) {
        return;
    }

    if (fragmentCount32 > 4096) {
        throw std::runtime_error("Encoded frame is too large for UDP fragmentation limit");
    }

    const auto fragmentCount = static_cast<uint16_t>(fragmentCount32);
    const auto* data = packet.bytes.data();
    for (uint16_t fragmentIndex = 0; fragmentIndex < fragmentCount; ++fragmentIndex) {
        const uint32_t offset = static_cast<uint32_t>(fragmentIndex) * maxPayload;
        const uint32_t payloadBytes = std::min(maxPayload, frameBytes - offset);
        SendDatagram(data + offset, payloadBytes, fragmentIndex, fragmentCount, packet);
    }

    ++frameId_;
    ++stats_.framesSent;
    stats_.payloadBytesSent += frameBytes;
}

bool UdpSender::isOpen() const noexcept
{
    return socket_ != 0 && !address_.empty();
}

void UdpSender::SendDatagram(const std::byte* payload, uint32_t payloadBytes, uint16_t fragmentIndex, uint16_t fragmentCount, const EncodedPacket& packet)
{
    UdpVideoPacketHeader header;
    header.magic = htonl(PacketMagic);
    header.version = htons(PacketVersion);
    header.headerBytes = htons(static_cast<uint16_t>(sizeof(UdpVideoPacketHeader)));
    header.frameId = HostToNetwork64(frameId_);
    header.timestamp100ns = HostToNetwork64(static_cast<uint64_t>(packet.timestamp100ns));
    header.frameBytes = htonl(static_cast<uint32_t>(packet.bytes.size()));
    header.fragmentIndex = htons(fragmentIndex);
    header.fragmentCount = htons(fragmentCount);
    header.payloadBytes = htonl(payloadBytes);

    std::vector<std::byte> datagram(sizeof(UdpVideoPacketHeader) + payloadBytes);
    std::memcpy(datagram.data(), &header, sizeof(header));
    std::memcpy(datagram.data() + sizeof(header), payload, payloadBytes);

    const int sent = sendto(
        AsSocket(socket_),
        reinterpret_cast<const char*>(datagram.data()),
        static_cast<int>(datagram.size()),
        0,
        reinterpret_cast<const sockaddr*>(address_.data()),
        addressLength_);

    if (sent == SOCKET_ERROR) {
        throw std::runtime_error(WinsockErrorMessage("sendto"));
    }

    ++stats_.datagramsSent;
    stats_.wireBytesSent += static_cast<uint64_t>(sent);
}

UdpSenderConfig ParseUdpSenderTarget(const std::string& target)
{
    const size_t separator = target.rfind(':');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= target.size()) {
        throw std::invalid_argument("--udp-send expects HOST:PORT");
    }

    UdpSenderConfig config;
    config.host = target.substr(0, separator);

    char* end = nullptr;
    const long port = std::strtol(target.c_str() + separator + 1, &end, 10);
    if (end == target.c_str() + separator + 1 || *end != '\0' || port <= 0 || port > 65535) {
        throw std::invalid_argument("Invalid UDP port in --udp-send: " + target);
    }

    config.port = static_cast<uint16_t>(port);
    return config;
}

} // namespace screenshare
