#include "transport/StunClient.h"

#include "transport/UdpProtocol.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace screenshare {
namespace {

constexpr uint16_t StunBindingRequest = 0x0001;
constexpr uint16_t StunBindingSuccessResponse = 0x0101;
constexpr uint16_t StunBindingErrorResponse = 0x0111;
constexpr uint32_t StunMagicCookie = 0x2112A442;
constexpr uint16_t StunAttributeMappedAddress = 0x0001;
constexpr uint16_t StunAttributeXorMappedAddress = 0x0020;
constexpr size_t StunHeaderBytes = 20;
constexpr size_t StunTransactionIdBytes = 12;
constexpr uint8_t StunAddressFamilyIpv4 = 0x01;

#pragma pack(push, 1)
struct StunHeader {
    uint16_t messageType = 0;
    uint16_t messageLength = 0;
    uint32_t magicCookie = 0;
    std::byte transactionId[StunTransactionIdBytes]{};
};

struct StunAttributeHeader {
    uint16_t type = 0;
    uint16_t length = 0;
};
#pragma pack(pop)

static_assert(sizeof(StunHeader) == StunHeaderBytes);
static_assert(sizeof(StunAttributeHeader) == 4);

std::string WinsockErrorMessage(const char* operation)
{
    return std::string(operation) + " failed with WSA error " + std::to_string(WSAGetLastError());
}

std::string AddressToString(const sockaddr_in& address)
{
    std::array<char, INET_ADDRSTRLEN> text{};
    if (inet_ntop(AF_INET, &address.sin_addr, text.data(), static_cast<socklen_t>(text.size())) == nullptr) {
        throw std::runtime_error(WinsockErrorMessage("inet_ntop(stun)"));
    }
    return text.data();
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
        throw std::runtime_error(WinsockErrorMessage("select(stun)"));
    }
    return result > 0 && FD_ISSET(socket, &readSet);
}

uint16_t ParsePort(const std::string& text, const std::string& target)
{
    char* end = nullptr;
    const long port = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || port <= 0 || port > 65535) {
        throw std::invalid_argument("Invalid STUN server port: " + target);
    }
    return static_cast<uint16_t>(port);
}

std::array<std::byte, StunTransactionIdBytes> GenerateTransactionId()
{
    std::array<std::byte, StunTransactionIdBytes> id{};
    std::random_device random;
    for (auto& byte : id) {
        byte = static_cast<std::byte>(random() & 0xFFU);
    }
    return id;
}

std::vector<std::byte> BuildBindingRequest(const std::array<std::byte, StunTransactionIdBytes>& transactionId)
{
    StunHeader header;
    header.messageType = udp_protocol::ToNetwork16(StunBindingRequest);
    header.messageLength = 0;
    header.magicCookie = udp_protocol::ToNetwork32(StunMagicCookie);
    std::memcpy(header.transactionId, transactionId.data(), transactionId.size());

    std::vector<std::byte> datagram(sizeof(header));
    std::memcpy(datagram.data(), &header, sizeof(header));
    return datagram;
}

bool TransactionMatches(const StunHeader& header, const std::array<std::byte, StunTransactionIdBytes>& transactionId)
{
    return std::memcmp(header.transactionId, transactionId.data(), transactionId.size()) == 0;
}

bool TryParseAddressAttribute(
    uint16_t attributeType,
    const std::byte* value,
    uint16_t length,
    std::string& addressText,
    uint16_t& port)
{
    if ((attributeType != StunAttributeXorMappedAddress && attributeType != StunAttributeMappedAddress) ||
        length < 8 ||
        static_cast<uint8_t>(value[1]) != StunAddressFamilyIpv4) {
        return false;
    }

    uint16_t portField = 0;
    uint32_t addressField = 0;
    std::memcpy(&portField, value + 2, sizeof(portField));
    std::memcpy(&addressField, value + 4, sizeof(addressField));

    if (attributeType == StunAttributeXorMappedAddress) {
        port = static_cast<uint16_t>(
            udp_protocol::FromNetwork16(portField) ^ static_cast<uint16_t>(StunMagicCookie >> 16));
        addressField ^= udp_protocol::ToNetwork32(StunMagicCookie);
    } else {
        port = udp_protocol::FromNetwork16(portField);
    }

    in_addr address{};
    address.s_addr = addressField;
    std::array<char, INET_ADDRSTRLEN> text{};
    if (inet_ntop(AF_INET, &address, text.data(), static_cast<socklen_t>(text.size())) == nullptr) {
        return false;
    }

    addressText = text.data();
    return true;
}

bool TryParseBindingResponse(
    const std::byte* datagram,
    int datagramBytes,
    const std::array<std::byte, StunTransactionIdBytes>& transactionId,
    StunQueryResult& result)
{
    if (datagramBytes < static_cast<int>(sizeof(StunHeader))) {
        return false;
    }

    StunHeader header;
    std::memcpy(&header, datagram, sizeof(header));
    const uint16_t messageType = udp_protocol::FromNetwork16(header.messageType);
    const uint16_t messageLength = udp_protocol::FromNetwork16(header.messageLength);
    if (udp_protocol::FromNetwork32(header.magicCookie) != StunMagicCookie ||
        !TransactionMatches(header, transactionId) ||
        messageLength % 4 != 0 ||
        StunHeaderBytes + messageLength > static_cast<size_t>(datagramBytes)) {
        return false;
    }
    if (messageType == StunBindingErrorResponse) {
        throw std::runtime_error("STUN server returned a binding error response");
    }
    if (messageType != StunBindingSuccessResponse) {
        return false;
    }

    bool foundMappedAddress = false;
    size_t offset = StunHeaderBytes;
    const size_t end = StunHeaderBytes + messageLength;
    while (offset + sizeof(StunAttributeHeader) <= end) {
        StunAttributeHeader attribute;
        std::memcpy(&attribute, datagram + offset, sizeof(attribute));
        offset += sizeof(attribute);

        const uint16_t type = udp_protocol::FromNetwork16(attribute.type);
        const uint16_t length = udp_protocol::FromNetwork16(attribute.length);
        if (offset + length > end) {
            return false;
        }

        std::string address;
        uint16_t port = 0;
        if (TryParseAddressAttribute(type, datagram + offset, length, address, port)) {
            result.publicAddress = std::move(address);
            result.publicPort = port;
            if (type == StunAttributeXorMappedAddress) {
                return true;
            }
            foundMappedAddress = true;
        }

        offset += (static_cast<size_t>(length) + 3U) & ~size_t{3U};
    }

    return foundMappedAddress;
}

} // namespace

StunServerTarget ParseStunServerTarget(const std::string& target)
{
    if (target.empty()) {
        throw std::invalid_argument("--stun expects HOST[:PORT]");
    }

    StunServerTarget parsed;
    if (target.front() == '[') {
        const size_t endBracket = target.find(']');
        if (endBracket == std::string::npos || endBracket == 1) {
            throw std::invalid_argument("--stun expects HOST[:PORT]");
        }
        parsed.host = target.substr(1, endBracket - 1);
        if (endBracket + 1 < target.size()) {
            if (target[endBracket + 1] != ':' || endBracket + 2 >= target.size()) {
                throw std::invalid_argument("--stun expects HOST[:PORT]");
            }
            parsed.port = ParsePort(target.substr(endBracket + 2), target);
        }
    } else {
        const size_t firstColon = target.find(':');
        const size_t lastColon = target.rfind(':');
        if (firstColon != std::string::npos && firstColon != lastColon) {
            throw std::invalid_argument("--stun currently supports IPv4 or DNS names; wrap IPv6 literals in brackets for parsing");
        }
        if (lastColon == std::string::npos) {
            parsed.host = target;
        } else {
            if (lastColon == 0 || lastColon + 1 >= target.size()) {
                throw std::invalid_argument("--stun expects HOST[:PORT]");
            }
            parsed.host = target.substr(0, lastColon);
            parsed.port = ParsePort(target.substr(lastColon + 1), target);
        }
    }

    if (parsed.host.empty()) {
        throw std::invalid_argument("--stun expects HOST[:PORT]");
    }
    return parsed;
}

StunQueryResult QueryPublicUdpEndpoint(const StunQueryConfig& config)
{
    if (config.server.host.empty()) {
        throw std::invalid_argument("STUN server host is empty");
    }
    if (config.server.port == 0) {
        throw std::invalid_argument("STUN server port must be non-zero");
    }
    if (config.timeout <= std::chrono::milliseconds(0)) {
        throw std::invalid_argument("STUN timeout must be positive");
    }

    WSADATA data{};
    const int startupResult = WSAStartup(MAKEWORD(2, 2), &data);
    if (startupResult != 0) {
        throw std::runtime_error("WSAStartup failed with WSA error " + std::to_string(startupResult));
    }

    SOCKET udpSocket = INVALID_SOCKET;
    addrinfo* resolved = nullptr;
    try {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        const std::string portText = std::to_string(config.server.port);
        const int addressResult = getaddrinfo(config.server.host.c_str(), portText.c_str(), &hints, &resolved);
        if (addressResult != 0 || resolved == nullptr) {
            throw std::runtime_error("getaddrinfo failed for STUN server " + config.server.host + ":" + portText);
        }

        udpSocket = ::socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
        if (udpSocket == INVALID_SOCKET) {
            throw std::runtime_error(WinsockErrorMessage("socket(stun)"));
        }

        sockaddr_in bindAddress{};
        bindAddress.sin_family = AF_INET;
        bindAddress.sin_addr.s_addr = htonl(INADDR_ANY);
        bindAddress.sin_port = htons(0);
        if (bind(udpSocket, reinterpret_cast<const sockaddr*>(&bindAddress), sizeof(bindAddress)) == SOCKET_ERROR) {
            throw std::runtime_error(WinsockErrorMessage("bind(stun)"));
        }
        if (connect(udpSocket, resolved->ai_addr, static_cast<int>(resolved->ai_addrlen)) == SOCKET_ERROR) {
            throw std::runtime_error(WinsockErrorMessage("connect(stun)"));
        }

        const auto transactionId = GenerateTransactionId();
        const auto request = BuildBindingRequest(transactionId);
        StunQueryResult result;
        const auto* serverAddress = reinterpret_cast<const sockaddr_in*>(resolved->ai_addr);
        result.serverAddress = AddressToString(*serverAddress);
        result.serverPort = ntohs(serverAddress->sin_port);

        sockaddr_in localAddress{};
        int localAddressLength = sizeof(localAddress);
        if (getsockname(udpSocket, reinterpret_cast<sockaddr*>(&localAddress), &localAddressLength) == 0) {
            result.localAddress = AddressToString(localAddress);
            result.localPort = ntohs(localAddress.sin_port);
        }

        const auto deadline = std::chrono::steady_clock::now() + config.timeout;
        auto nextSendAt = std::chrono::steady_clock::now();
        std::array<std::byte, 1024> buffer{};

        while (std::chrono::steady_clock::now() < deadline) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextSendAt) {
                const int sent = send(
                    udpSocket,
                    reinterpret_cast<const char*>(request.data()),
                    static_cast<int>(request.size()),
                    0);
                if (sent == SOCKET_ERROR) {
                    throw std::runtime_error(WinsockErrorMessage("send(stun)"));
                }
                nextSendAt = now + std::chrono::milliseconds(500);
            }

            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            const auto wait = std::min(remaining, std::chrono::milliseconds(100));
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

            if (TryParseBindingResponse(buffer.data(), received, transactionId, result)) {
                closesocket(udpSocket);
                freeaddrinfo(resolved);
                WSACleanup();
                return result;
            }
        }

        throw std::runtime_error("Timed out waiting for STUN binding response");
    } catch (...) {
        if (udpSocket != INVALID_SOCKET) {
            closesocket(udpSocket);
        }
        if (resolved != nullptr) {
            freeaddrinfo(resolved);
        }
        WSACleanup();
        throw;
    }
}

} // namespace screenshare
