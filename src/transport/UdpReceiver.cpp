#include "transport/UdpReceiver.h"

#include "transport/NatTraversal.h"
#include "transport/UdpProtocol.h"

#include <winsock2.h>
#include <mstcpip.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <span>
#include <stdexcept>
#include <string>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace screenshare {
namespace {

// Insert byte range [begin, end) into a list of already-received fragment
// ranges kept sorted by `begin`, rejecting (returning false) if it overlaps an
// existing range. Because the stored ranges are disjoint and sorted, only the
// immediate neighbors can overlap, so this is O(log n) (binary search + two
// neighbor checks) instead of the O(n) linear scan it replaces — bounding the
// per-frame reassembly cost at O(n log n) rather than O(n^2). Works for both
// the video and audio pending-fragment structs (identical FragmentRange shape).
template <typename RangeVec>
bool InsertDisjointFragmentRange(RangeVec& ranges, uint32_t begin, uint32_t end)
{
    // First stored range whose begin is >= the new range's begin.
    auto next = std::lower_bound(
        ranges.begin(),
        ranges.end(),
        begin,
        [](const auto& range, uint32_t value) { return range.begin < value; });
    // Overlap with the successor: the new range extends past its start.
    if (next != ranges.end() && end > next->begin) {
        return false;
    }
    // Overlap with the predecessor: its end extends past the new range's start.
    if (next != ranges.begin()) {
        const auto prev = std::prev(next);
        if (prev->end > begin) {
            return false;
        }
    }
    ranges.insert(next, {begin, end});
    return true;
}

std::string WinsockErrorDescription(int error)
{
    switch (error) {
    case WSAEADDRINUSE:
        return "address already in use";
    case WSAEADDRNOTAVAIL:
        return "address not available";
    case WSAEACCES:
        return "permission denied";
    case WSAENETUNREACH:
        return "network unreachable";
    case WSAEHOSTUNREACH:
        return "host unreachable";
    default:
        return {};
    }
}

std::string WinsockErrorMessage(const std::string& operation, int error)
{
    std::string message = operation + " failed with WSA error " + std::to_string(error);
    const std::string description = WinsockErrorDescription(error);
    if (!description.empty()) {
        message += " (" + description + ")";
    }
    return message;
}

std::string WinsockErrorMessage(const char* operation)
{
    return WinsockErrorMessage(operation, WSAGetLastError());
}

SOCKET AsSocket(uintptr_t socket)
{
    return static_cast<SOCKET>(socket);
}

void DisableUdpConnReset(SOCKET socket)
{
    BOOL newBehavior = FALSE;
    DWORD bytesReturned = 0;
    static_cast<void>(WSAIoctl(
        socket,
        SIO_UDP_CONNRESET,
        &newBehavior,
        sizeof(newBehavior),
        nullptr,
        0,
        &bytesReturned,
        nullptr,
        nullptr));
}

std::string SocketAddressToString(const void* address, int addressLength)
{
    if (address == nullptr || addressLength < static_cast<int>(sizeof(sockaddr))) {
        return {};
    }

    const auto* socketAddress = static_cast<const sockaddr*>(address);
    if (socketAddress->sa_family != AF_INET || addressLength < static_cast<int>(sizeof(sockaddr_in))) {
        return {};
    }

    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
    std::array<char, INET_ADDRSTRLEN> host{};
    if (inet_ntop(AF_INET, &ipv4->sin_addr, host.data(), static_cast<socklen_t>(host.size())) == nullptr) {
        return {};
    }

    return std::string(host.data()) + ":" + std::to_string(ntohs(ipv4->sin_port));
}

} // namespace

UdpReceiver::UdpReceiver()
{
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        throw std::runtime_error("WSAStartup failed with WSA error " + std::to_string(result));
    }

    winsockStarted_ = true;
}

UdpReceiver::~UdpReceiver()
{
    Close();

    if (winsockStarted_) {
        WSACleanup();
    }
}

void UdpReceiver::Open(const UdpReceiverConfig& config)
{
    Close();

    if (config.port == 0) {
        throw std::invalid_argument("UDP receive port must be non-zero");
    }
    if (config.maxDatagramBytes < sizeof(udp_protocol::PacketHeader) || config.maxDatagramBytes > 65'507) {
        throw std::invalid_argument("UDP max datagram bytes must be between the protocol header size and 65507");
    }
    if (config.maxFrameBytes == 0) {
        throw std::invalid_argument("UDP max frame bytes must be non-zero");
    }
    if (config.maxPendingFrames == 0) {
        throw std::invalid_argument("UDP max pending frames must be non-zero");
    }
    if (config.maxPendingAudioPackets == 0) {
        throw std::invalid_argument("UDP max pending audio packets must be non-zero");
    }
    if (config.maxCompletedAudioPackets == 0) {
        throw std::invalid_argument("UDP max completed audio packets must be non-zero");
    }
    if (config.maxAudioPacketBytes == 0) {
        throw std::invalid_argument("UDP max audio packet bytes must be non-zero");
    }
    if (config.socketReceiveBufferBytes > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("UDP socket receive buffer is too large");
    }
    if (config.simulatedLossPercent < 0.0f || config.simulatedLossPercent > 100.0f) {
        throw std::invalid_argument("UDP simulated loss percent must be between 0 and 100");
    }
    if (config.simulatedJitter.count() < 0 || config.simulatedJitter > std::chrono::seconds(5)) {
        throw std::invalid_argument("UDP simulated jitter must be between 0 and 5000 ms");
    }
    if (config.encryptionKey && config.accessCodeFingerprint == 0) {
        throw std::invalid_argument("UDP encryption requires an access code fingerprint");
    }
    if (config.natProbeInterval < std::chrono::milliseconds(50) ||
        config.natProbeInterval > std::chrono::seconds(5)) {
        throw std::invalid_argument("UDP NAT probe interval must be between 50 and 5000 ms");
    }

    std::unique_ptr<UdpAesGcm> encryptCrypto;
    UdpCryptoKey master{};
    UdpCryptoSessionSalt sessionSalt{};
    const bool encryptionEnabled = config.encryptionKey.has_value();
    if (encryptionEnabled) {
        master = *config.encryptionKey;
        sessionSalt = GenerateUdpCryptoSessionSalt();
        encryptCrypto = std::make_unique<UdpAesGcm>(DeriveUdpSessionKey(master, sessionSalt));
    }

    std::vector<NatProbeTargetAddress> natProbeTargets;
    for (const auto& target : config.natProbeTargets) {
        if (target.host.empty() || target.port == 0) {
            throw std::invalid_argument("UDP NAT probe target must have host and port");
        }

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        addrinfo* resolved = nullptr;
        const std::string port = std::to_string(target.port);
        const int addressResult = getaddrinfo(target.host.c_str(), port.c_str(), &hints, &resolved);
        if (addressResult != 0 || resolved == nullptr) {
            throw std::runtime_error("getaddrinfo failed for UDP NAT probe target " + target.host + ":" + port);
        }

        NatProbeTargetAddress resolvedTarget;
        resolvedTarget.address.resize(static_cast<size_t>(resolved->ai_addrlen));
        std::memcpy(resolvedTarget.address.data(), resolved->ai_addr, static_cast<size_t>(resolved->ai_addrlen));
        resolvedTarget.addressLength = static_cast<int>(resolved->ai_addrlen);
        resolvedTarget.localEndpoint = target.localEndpoint;
        freeaddrinfo(resolved);

        const auto duplicate = std::find_if(
            natProbeTargets.begin(),
            natProbeTargets.end(),
            [&](const NatProbeTargetAddress& existing) {
                return existing.address == resolvedTarget.address;
            });
        if (duplicate == natProbeTargets.end()) {
            natProbeTargets.push_back(std::move(resolvedTarget));
        }
    }

    const SOCKET udpSocket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET) {
        throw std::runtime_error(WinsockErrorMessage("socket"));
    }

    DisableUdpConnReset(udpSocket);

    if (config.socketReceiveBufferBytes > 0) {
        const int receiveBufferBytes = static_cast<int>(config.socketReceiveBufferBytes);
        static_cast<void>(setsockopt(
            udpSocket,
            SOL_SOCKET,
            SO_RCVBUF,
            reinterpret_cast<const char*>(&receiveBufferBytes),
            sizeof(receiveBufferBytes)));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(config.port);

    if (bind(udpSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        closesocket(udpSocket);
        std::string message = WinsockErrorMessage(
            "bind(receiver 0.0.0.0:" + std::to_string(config.port) + ")",
            error);
        if (error == WSAEADDRINUSE) {
            message += ". The UDP receive port is already in use; close the other process using it or choose "
                       "a different --watch/--udp-recv port.";
        }
        throw std::runtime_error(message);
    }

    socket_ = static_cast<uintptr_t>(udpSocket);
    config_ = config;
    encryptionEnabled_ = encryptionEnabled;
    master_ = master;
    sessionSalt_ = sessionSalt;
    encryptCrypto_ = std::move(encryptCrypto);
    peerDecryptCryptos_.clear();
    nextControlSequence_ = 1;
    datagramBuffer_.assign(config_.maxDatagramBytes, std::byte{});
    feedbackAddress_.clear();
    feedbackAddressLength_ = 0;
    natProbeTargets_ = std::move(natProbeTargets);
    nextNatProbeAt_ = Clock::now();
    nextNatProbeSequence_ = 1;
    delayedDatagrams_.clear();
    completedAudioPackets_.clear();
    pendingFrames_.clear();
    pendingAudioPackets_.clear();
    stats_ = {};
    simulationRng_.seed(config_.simulationSeed);
}

void UdpReceiver::Close()
{
    if (socket_ != 0) {
        closesocket(AsSocket(socket_));
        socket_ = 0;
    }

    datagramBuffer_.clear();
    encryptionEnabled_ = false;
    master_ = {};
    sessionSalt_ = {};
    encryptCrypto_.reset();
    peerDecryptCryptos_.clear();
    nextControlSequence_ = 1;
    feedbackAddress_.clear();
    feedbackAddressLength_ = 0;
    natProbeTargets_.clear();
    nextNatProbeAt_ = {};
    nextNatProbeSequence_ = 1;
    delayedDatagrams_.clear();
    completedAudioPackets_.clear();
    pendingFrames_.clear();
    pendingAudioPackets_.clear();
}

void UdpReceiver::ResetMediaQueues()
{
    completedAudioPackets_.clear();
    pendingFrames_.clear();
    pendingAudioPackets_.clear();
}

bool UdpReceiver::AddNatProbeTarget(const UdpNatProbeTarget& target)
{
    if (!isOpen()) {
        throw std::logic_error("UdpReceiver::Open must be called before AddNatProbeTarget");
    }
    if (target.host.empty() || target.port == 0) {
        throw std::invalid_argument("UDP NAT probe target must have host and port");
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* resolved = nullptr;
    const std::string port = std::to_string(target.port);
    const int addressResult = getaddrinfo(target.host.c_str(), port.c_str(), &hints, &resolved);
    if (addressResult != 0 || resolved == nullptr) {
        throw std::runtime_error("getaddrinfo failed for UDP NAT probe target " + target.host + ":" + port);
    }

    NatProbeTargetAddress resolvedTarget;
    resolvedTarget.address.resize(static_cast<size_t>(resolved->ai_addrlen));
    std::memcpy(resolvedTarget.address.data(), resolved->ai_addr, static_cast<size_t>(resolved->ai_addrlen));
    resolvedTarget.addressLength = static_cast<int>(resolved->ai_addrlen);
    resolvedTarget.localEndpoint = target.localEndpoint;
    freeaddrinfo(resolved);

    const auto duplicate = std::find_if(
        natProbeTargets_.begin(),
        natProbeTargets_.end(),
        [&](const NatProbeTargetAddress& existing) {
            return existing.address == resolvedTarget.address;
        });
    if (duplicate != natProbeTargets_.end()) {
        return false;
    }

    natProbeTargets_.push_back(std::move(resolvedTarget));
    nextNatProbeAt_ = Clock::now();
    return true;
}

std::optional<UdpCompletedFrame> UdpReceiver::ReceiveFrame(std::chrono::milliseconds timeout)
{
    if (!isOpen()) {
        throw std::logic_error("UdpReceiver::Open must be called before ReceiveFrame");
    }

    const auto startedAt = Clock::now();
    const auto deadline = startedAt + timeout;
    DropExpiredFrames(startedAt);

    while (true) {
        const auto now = Clock::now();
        MaybeSendNatProbes(now);
        if (auto frame = ReleaseReadyDelayedDatagram(now)) {
            return frame;
        }

        if (now >= deadline) {
            return std::nullopt;
        }

        auto waitTime = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto delayedWait = WaitUntilNextDelayedDatagram(now);
        if (delayedWait >= std::chrono::milliseconds(0)) {
            waitTime = std::min(waitTime, delayedWait);
        }
        if (!natProbeTargets_.empty() && nextNatProbeAt_ > now) {
            waitTime = std::min(
                waitTime,
                std::chrono::duration_cast<std::chrono::milliseconds>(nextNatProbeAt_ - now));
        }
        waitTime = std::max(std::chrono::milliseconds(1), waitTime);

        if (!WaitForReadable(waitTime)) {
            DropExpiredFrames(Clock::now());
            continue;
        }

        if (auto frame = ReceiveDatagram()) {
            return frame;
        }

        DropExpiredFrames(Clock::now());
    }
}

std::optional<UdpCompletedAudioPacket> UdpReceiver::PopAudioPacket()
{
    if (completedAudioPackets_.empty()) {
        return std::nullopt;
    }

    auto packet = std::move(completedAudioPackets_.front());
    completedAudioPackets_.pop_front();
    return packet;
}

bool UdpReceiver::isOpen() const noexcept
{
    return socket_ != 0;
}

bool UdpReceiver::WaitForReadable(std::chrono::milliseconds timeout)
{
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(AsSocket(socket_), &readSet);

    timeval waitTime{};
    waitTime.tv_sec = static_cast<long>(timeout.count() / 1000);
    waitTime.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);

    const int result = select(0, &readSet, nullptr, nullptr, &waitTime);
    if (result == SOCKET_ERROR) {
        throw std::runtime_error(WinsockErrorMessage("select"));
    }

    return result > 0;
}

std::optional<UdpCompletedFrame> UdpReceiver::ReceiveDatagram()
{
    sockaddr_in senderAddress{};
    int senderAddressLength = sizeof(senderAddress);
    const int received = recvfrom(
        AsSocket(socket_),
        reinterpret_cast<char*>(datagramBuffer_.data()),
        static_cast<int>(datagramBuffer_.size()),
        0,
        reinterpret_cast<sockaddr*>(&senderAddress),
        &senderAddressLength);

    if (received == SOCKET_ERROR) {
        if (WSAGetLastError() == WSAECONNRESET) {
            return std::nullopt;
        }
        throw std::runtime_error(WinsockErrorMessage("recvfrom"));
    }

    if (IsNatProbeDatagram(std::span<const std::byte>(
            datagramBuffer_.data(),
            static_cast<size_t>(received)))) {
        return std::nullopt;
    }

    feedbackAddress_.resize(static_cast<size_t>(senderAddressLength));
    std::memcpy(feedbackAddress_.data(), &senderAddress, static_cast<size_t>(senderAddressLength));
    feedbackAddressLength_ = senderAddressLength;
    currentDatagramEndpoint_ = SocketAddressToString(&senderAddress, senderAddressLength);

    ++stats_.datagramsReceived;
    if (ShouldSimulateLoss()) {
        ++stats_.simulatedDatagramsDropped;
        return std::nullopt;
    }

    if (config_.simulatedJitter.count() > 0) {
        const auto delay = NextSimulatedJitterDelay();
        if (delay.count() > 0) {
            QueueDelayedDatagram(datagramBuffer_.data(), received, Clock::now() + delay);
            ++stats_.simulatedDatagramsDelayed;
            return std::nullopt;
        }
    }

    return ProcessDatagram(datagramBuffer_.data(), received);
}

bool UdpReceiver::SendFeedback(const udp_protocol::FeedbackSnapshot& feedback)
{
    if (!isOpen() || feedbackAddress_.empty() || feedbackAddressLength_ == 0) {
        return false;
    }

    auto datagram = udp_protocol::BuildFeedbackDatagram(feedback);
    const bool encrypted = EncryptFeedbackDatagram(datagram);
    const int sent = sendto(
        AsSocket(socket_),
        reinterpret_cast<const char*>(datagram.data()),
        static_cast<int>(datagram.size()),
        0,
        reinterpret_cast<const sockaddr*>(feedbackAddress_.data()),
        feedbackAddressLength_);

    if (sent == SOCKET_ERROR || sent != static_cast<int>(datagram.size())) {
        ++stats_.feedbackSendErrors;
        return false;
    }

    ++stats_.feedbackPacketsSent;
    if (encrypted) {
        ++stats_.encryptedFeedbackPacketsSent;
    }
    return true;
}

void UdpReceiver::SetControlHandler(std::function<void(const udp_protocol::ControlMessage&)> handler)
{
    onControl_ = std::move(handler);
}

bool UdpReceiver::SendControl(const udp_protocol::ControlMessage& message)
{
    if (!isOpen() || feedbackAddress_.empty() || feedbackAddressLength_ == 0) {
        return false;
    }

    udp_protocol::ControlMessage outgoing = message;
    outgoing.sequence = nextControlSequence_++;
    auto datagram = udp_protocol::BuildControlDatagram(outgoing);
    EncryptControlDatagram(datagram);
    const int sent = sendto(
        AsSocket(socket_),
        reinterpret_cast<const char*>(datagram.data()),
        static_cast<int>(datagram.size()),
        0,
        reinterpret_cast<const sockaddr*>(feedbackAddress_.data()),
        feedbackAddressLength_);
    return sent != SOCKET_ERROR && sent == static_cast<int>(datagram.size());
}

void UdpReceiver::MaybeSendNatProbes(Clock::time_point now)
{
    if (!isOpen() || natProbeTargets_.empty() || now < nextNatProbeAt_) {
        return;
    }

    const auto datagram = BuildNatProbeDatagram(
        nextNatProbeSequence_++,
        config_.natProbeSessionFingerprint,
        config_.accessCodeFingerprint);

    for (const auto& target : natProbeTargets_) {
        const int sent = sendto(
            AsSocket(socket_),
            reinterpret_cast<const char*>(datagram.data()),
            static_cast<int>(datagram.size()),
            0,
            reinterpret_cast<const sockaddr*>(target.address.data()),
            target.addressLength);
        if (sent == SOCKET_ERROR || sent != static_cast<int>(datagram.size())) {
            ++stats_.natProbeSendErrors;
            continue;
        }
        if (target.localEndpoint) {
            ++stats_.natProbeLocalPacketsSent;
        } else {
            ++stats_.natProbePublicPacketsSent;
        }
    }

    nextNatProbeAt_ = now + config_.natProbeInterval;
}

std::optional<UdpCompletedFrame> UdpReceiver::ReleaseReadyDelayedDatagram(Clock::time_point now)
{
    while (!delayedDatagrams_.empty() && delayedDatagrams_.front().releaseAt <= now) {
        auto datagram = std::move(delayedDatagrams_.front().bytes);
        delayedDatagrams_.pop_front();
        if (auto frame = ProcessDatagram(datagram.data(), static_cast<int>(datagram.size()))) {
            return frame;
        }
    }

    return std::nullopt;
}

std::optional<UdpCompletedFrame> UdpReceiver::ProcessDatagram(const std::byte* datagram, int datagramBytes)
{
    if (datagramBytes < static_cast<int>(sizeof(uint32_t))) {
        ++stats_.invalidDatagrams;
        return std::nullopt;
    }
    if (IsNatProbeDatagram(std::span<const std::byte>(datagram, static_cast<size_t>(datagramBytes)))) {
        return std::nullopt;
    }

    uint32_t rawMagic = 0;
    std::memcpy(&rawMagic, datagram, sizeof(rawMagic));
    const uint32_t datagramMagic = udp_protocol::FromNetwork32(rawMagic);
    if (datagramMagic == udp_protocol::AudioMagic) {
        ProcessAudioDatagram(datagram, datagramBytes);
        return std::nullopt;
    }
    if (datagramMagic == udp_protocol::ControlMagic) {
        ProcessControlDatagram(datagram, datagramBytes);
        return std::nullopt;
    }

    if (datagramBytes < static_cast<int>(sizeof(udp_protocol::PacketHeader))) {
        ++stats_.invalidDatagrams;
        return std::nullopt;
    }

    udp_protocol::PacketHeader header;
    std::memcpy(&header, datagram, sizeof(header));

    const uint32_t magic = udp_protocol::FromNetwork32(header.magic);
    const uint16_t version = udp_protocol::FromNetwork16(header.version);
    const uint16_t headerBytes = udp_protocol::FromNetwork16(header.headerBytes);
    const uint64_t frameId = udp_protocol::FromNetwork64(header.frameId);
    const uint64_t timestamp100ns = udp_protocol::FromNetwork64(header.timestamp100ns);
    const uint64_t senderQpc100ns = udp_protocol::FromNetwork64(header.senderQpc100ns);
    const uint64_t accessCodeFingerprint = udp_protocol::FromNetwork64(header.accessCodeFingerprint);
    const uint32_t frameBytes = udp_protocol::FromNetwork32(header.frameBytes);
    const uint32_t fragmentOffset = udp_protocol::FromNetwork32(header.fragmentOffset);
    const uint16_t fragmentIndex = udp_protocol::FromNetwork16(header.fragmentIndex);
    const uint16_t fragmentCount = udp_protocol::FromNetwork16(header.fragmentCount);
    const uint32_t payloadBytes = udp_protocol::FromNetwork32(header.payloadBytes);
    const uint32_t flags = udp_protocol::FromNetwork32(header.flags);

    if (magic != udp_protocol::PacketMagic ||
        version != udp_protocol::PacketVersion ||
        headerBytes != sizeof(udp_protocol::PacketHeader) ||
        datagramBytes < static_cast<int>(headerBytes) ||
        (flags & ~udp_protocol::PacketFlagEncrypted) != 0) {
        ++stats_.invalidDatagrams;
        return std::nullopt;
    }
    if (config_.accessCodeFingerprint != 0 &&
        accessCodeFingerprint != config_.accessCodeFingerprint) {
        ++stats_.accessRejectedDatagrams;
        return std::nullopt;
    }

    const auto actualPayloadBytes = static_cast<uint32_t>(datagramBytes - headerBytes);
    if (frameBytes == 0 ||
        frameBytes > config_.maxFrameBytes ||
        fragmentCount == 0 ||
        fragmentCount > udp_protocol::MaxFragmentsPerFrame ||
        fragmentIndex >= fragmentCount ||
        payloadBytes == 0 ||
        payloadBytes != actualPayloadBytes ||
        fragmentOffset > frameBytes ||
        payloadBytes > frameBytes - fragmentOffset) {
        ++stats_.invalidDatagrams;
        return std::nullopt;
    }
    std::optional<std::vector<std::byte>> decryptedPayload;
    const std::byte* payloadData = datagram + headerBytes;
    if ((flags & udp_protocol::PacketFlagEncrypted) != 0) {
        UdpCryptoSessionSalt salt{};
        std::memcpy(salt.data(), header.sessionSalt, salt.size());
        UdpAesGcm* crypto = DecryptCryptoForSalt(salt);
        if (crypto == nullptr) {
            ++stats_.cryptoRejectedDatagrams;
            return std::nullopt;
        }
        decryptedPayload = DecryptDatagramPayload(
            *crypto,
            datagram,
            datagramBytes,
            headerBytes,
            udp_protocol::PacketHeaderAuthenticatedBytes,
            std::span<const std::byte, UdpCryptoNonceBytes>(header.encryptionNonce, UdpCryptoNonceBytes),
            std::span<const std::byte, UdpCryptoTagBytes>(header.encryptionTag, UdpCryptoTagBytes));
        if (!decryptedPayload) {
            ++stats_.cryptoRejectedDatagrams;
            return std::nullopt;
        }
        payloadData = decryptedPayload->data();
    } else if (encryptionEnabled_) {
        ++stats_.cryptoRejectedDatagrams;
        return std::nullopt;
    }

    const auto now = Clock::now();
    auto [it, inserted] = pendingFrames_.try_emplace(frameId);
    PendingFrame& pending = it->second;
    if (inserted) {
        // Size both buffers before the entry is considered usable. If an
        // allocation throws, erase the half-built entry so a later fragment
        // for the same frame id cannot index empty vectors.
        try {
            pending.frameId = frameId;
            pending.timestamp100ns = timestamp100ns;
            pending.senderQpc100ns = senderQpc100ns;
            pending.frameBytes = frameBytes;
            pending.fragmentCount = fragmentCount;
            pending.bytes.assign(frameBytes, std::byte{});
            pending.fragmentReceived.assign(fragmentCount, 0);
        } catch (...) {
            pendingFrames_.erase(it);
            throw;
        }
    } else if (pending.timestamp100ns != timestamp100ns ||
               pending.senderQpc100ns != senderQpc100ns ||
               pending.frameBytes != frameBytes ||
               pending.fragmentCount != fragmentCount) {
        ++stats_.invalidDatagrams;
        return std::nullopt;
    }

    pending.lastUpdated = now;

    if (pending.fragmentReceived[fragmentIndex] != 0) {
        ++stats_.duplicateFragments;
        return std::nullopt;
    }

    const uint32_t fragmentEnd = fragmentOffset + payloadBytes;
    if (payloadBytes > pending.frameBytes - pending.receivedBytes) {
        ++stats_.invalidDatagrams;
        return std::nullopt;
    }
    // Reject a fragment whose byte range overlaps one already received (a peer
    // lying about fragmentOffset), recording it in sorted order on success.
    if (!InsertDisjointFragmentRange(pending.receivedRanges, fragmentOffset, fragmentEnd)) {
        ++stats_.invalidDatagrams;
        return std::nullopt;
    }

    ++stats_.datagramsAccepted;
    if (!currentDatagramEndpoint_.empty()) {
        stats_.latestMediaEndpoint = currentDatagramEndpoint_;
    }
    stats_.payloadBytesReceived += payloadBytes;

    std::memcpy(pending.bytes.data() + fragmentOffset, payloadData, payloadBytes);
    pending.fragmentReceived[fragmentIndex] = 1;
    ++pending.receivedFragments;
    pending.receivedBytes += payloadBytes;

    if (pending.receivedFragments != pending.fragmentCount || pending.receivedBytes != pending.frameBytes) {
        EnforcePendingFrameLimit();
        return std::nullopt;
    }

    UdpCompletedFrame completed;
    completed.frameId = pending.frameId;
    completed.timestamp100ns = pending.timestamp100ns;
    completed.senderQpc100ns = pending.senderQpc100ns;
    completed.fragmentCount = pending.fragmentCount;
    completed.bytes = std::move(pending.bytes);

    pendingFrames_.erase(it);
    ++stats_.framesCompleted;
    stats_.completedFrameBytes += completed.bytes.size();
    return completed;
}

void UdpReceiver::ProcessAudioDatagram(const std::byte* datagram, int datagramBytes)
{
    if (datagramBytes < static_cast<int>(sizeof(udp_protocol::AudioPacketHeader))) {
        ++stats_.invalidDatagrams;
        return;
    }

    udp_protocol::AudioPacketHeader header;
    std::memcpy(&header, datagram, sizeof(header));

    const uint32_t magic = udp_protocol::FromNetwork32(header.magic);
    const uint16_t version = udp_protocol::FromNetwork16(header.version);
    const uint16_t headerBytes = udp_protocol::FromNetwork16(header.headerBytes);
    const uint64_t packetId = udp_protocol::FromNetwork64(header.packetId);
    const uint64_t devicePosition = udp_protocol::FromNetwork64(header.devicePosition);
    const uint64_t qpcPosition = udp_protocol::FromNetwork64(header.qpcPosition);
    const uint64_t accessCodeFingerprint = udp_protocol::FromNetwork64(header.accessCodeFingerprint);
    const uint32_t sampleRate = udp_protocol::FromNetwork32(header.sampleRate);
    const uint16_t channels = udp_protocol::FromNetwork16(header.channels);
    const uint16_t bitsPerSample = udp_protocol::FromNetwork16(header.bitsPerSample);
    const uint16_t blockAlign = udp_protocol::FromNetwork16(header.blockAlign);
    const auto sampleFormat = static_cast<udp_protocol::AudioSampleFormat>(udp_protocol::FromNetwork16(header.sampleFormat));
    const auto codec = static_cast<udp_protocol::AudioCodec>(udp_protocol::FromNetwork16(header.codec));
    const uint32_t audioFrames = udp_protocol::FromNetwork32(header.audioFrames);
    const uint32_t packetBytes = udp_protocol::FromNetwork32(header.packetBytes);
    const uint32_t fragmentOffset = udp_protocol::FromNetwork32(header.fragmentOffset);
    const uint16_t fragmentIndex = udp_protocol::FromNetwork16(header.fragmentIndex);
    const uint16_t fragmentCount = udp_protocol::FromNetwork16(header.fragmentCount);
    const uint32_t payloadBytes = udp_protocol::FromNetwork32(header.payloadBytes);
    const uint32_t flags = udp_protocol::FromNetwork32(header.flags);
    const uint32_t encryptionFlags = udp_protocol::FromNetwork32(header.encryptionFlags);

    const bool knownSampleFormat =
        sampleFormat == udp_protocol::AudioSampleFormat::Float32 ||
        sampleFormat == udp_protocol::AudioSampleFormat::Pcm16 ||
        sampleFormat == udp_protocol::AudioSampleFormat::Pcm24 ||
        sampleFormat == udp_protocol::AudioSampleFormat::Pcm32;
    const bool knownCodec =
        codec == udp_protocol::AudioCodec::Raw ||
        codec == udp_protocol::AudioCodec::Opus;
    const uint32_t allowedFlags =
        udp_protocol::AudioPacketFlagSilent |
        udp_protocol::AudioPacketFlagDataDiscontinuity |
        udp_protocol::AudioPacketFlagTimestampError;

    if (magic != udp_protocol::AudioMagic ||
        version != udp_protocol::PacketVersion ||
        headerBytes != sizeof(udp_protocol::AudioPacketHeader) ||
        datagramBytes < static_cast<int>(headerBytes) ||
        !knownSampleFormat ||
        !knownCodec ||
        (flags & ~allowedFlags) != 0 ||
        (encryptionFlags & ~udp_protocol::PacketFlagEncrypted) != 0) {
        ++stats_.invalidDatagrams;
        return;
    }
    if (config_.accessCodeFingerprint != 0 &&
        accessCodeFingerprint != config_.accessCodeFingerprint) {
        ++stats_.accessRejectedDatagrams;
        return;
    }

    const auto actualPayloadBytes = static_cast<uint32_t>(datagramBytes - headerBytes);
    if (sampleRate == 0 ||
        channels == 0 ||
        bitsPerSample == 0 ||
        blockAlign == 0 ||
        audioFrames == 0 ||
        packetBytes == 0 ||
        packetBytes > config_.maxAudioPacketBytes ||
        fragmentCount == 0 ||
        fragmentCount > udp_protocol::MaxFragmentsPerFrame ||
        fragmentIndex >= fragmentCount ||
        payloadBytes == 0 ||
        payloadBytes != actualPayloadBytes ||
        fragmentOffset > packetBytes ||
        payloadBytes > packetBytes - fragmentOffset) {
        ++stats_.invalidDatagrams;
        return;
    }
    std::optional<std::vector<std::byte>> decryptedPayload;
    const std::byte* payloadData = datagram + headerBytes;
    if ((encryptionFlags & udp_protocol::PacketFlagEncrypted) != 0) {
        UdpCryptoSessionSalt salt{};
        std::memcpy(salt.data(), header.sessionSalt, salt.size());
        UdpAesGcm* crypto = DecryptCryptoForSalt(salt);
        if (crypto == nullptr) {
            ++stats_.cryptoRejectedDatagrams;
            return;
        }
        decryptedPayload = DecryptDatagramPayload(
            *crypto,
            datagram,
            datagramBytes,
            headerBytes,
            udp_protocol::AudioPacketHeaderAuthenticatedBytes,
            std::span<const std::byte, UdpCryptoNonceBytes>(header.encryptionNonce, UdpCryptoNonceBytes),
            std::span<const std::byte, UdpCryptoTagBytes>(header.encryptionTag, UdpCryptoTagBytes));
        if (!decryptedPayload) {
            ++stats_.cryptoRejectedDatagrams;
            return;
        }
        payloadData = decryptedPayload->data();
    } else if (encryptionEnabled_) {
        ++stats_.cryptoRejectedDatagrams;
        return;
    }
    if (codec == udp_protocol::AudioCodec::Raw &&
        static_cast<uint64_t>(audioFrames) * static_cast<uint64_t>(blockAlign) != packetBytes) {
        ++stats_.invalidDatagrams;
        return;
    }
    if (codec == udp_protocol::AudioCodec::Opus &&
        (sampleRate != 48'000 ||
         channels != 2 ||
         bitsPerSample != 32 ||
         blockAlign != 8 ||
         sampleFormat != udp_protocol::AudioSampleFormat::Float32)) {
        ++stats_.invalidDatagrams;
        return;
    }

    const auto now = Clock::now();
    auto [it, inserted] = pendingAudioPackets_.try_emplace(packetId);
    PendingAudioPacket& pending = it->second;
    if (inserted) {
        // Size both buffers before the entry is considered usable; erase the
        // half-built entry if an allocation throws (see video path above).
        try {
            pending.packetId = packetId;
            pending.devicePosition = devicePosition;
            pending.qpcPosition = qpcPosition;
            pending.sampleRate = sampleRate;
            pending.channels = channels;
            pending.bitsPerSample = bitsPerSample;
            pending.blockAlign = blockAlign;
            pending.sampleFormat = sampleFormat;
            pending.codec = codec;
            pending.audioFrames = audioFrames;
            pending.packetBytes = packetBytes;
            pending.flags = flags;
            pending.fragmentCount = fragmentCount;
            pending.bytes.assign(packetBytes, std::byte{});
            pending.fragmentReceived.assign(fragmentCount, 0);
        } catch (...) {
            pendingAudioPackets_.erase(it);
            throw;
        }
    } else if (pending.devicePosition != devicePosition ||
               pending.qpcPosition != qpcPosition ||
               pending.sampleRate != sampleRate ||
               pending.channels != channels ||
               pending.bitsPerSample != bitsPerSample ||
               pending.blockAlign != blockAlign ||
               pending.sampleFormat != sampleFormat ||
               pending.codec != codec ||
               pending.audioFrames != audioFrames ||
               pending.packetBytes != packetBytes ||
               pending.flags != flags ||
               pending.fragmentCount != fragmentCount) {
        ++stats_.invalidDatagrams;
        return;
    }

    pending.lastUpdated = now;

    if (pending.fragmentReceived[fragmentIndex] != 0) {
        ++stats_.audioDuplicateFragments;
        return;
    }

    const uint32_t fragmentEnd = fragmentOffset + payloadBytes;
    if (payloadBytes > pending.packetBytes - pending.receivedBytes) {
        ++stats_.invalidDatagrams;
        return;
    }
    // Reject a fragment whose byte range overlaps one already received,
    // recording it in sorted order on success (O(log n), see the video path).
    if (!InsertDisjointFragmentRange(pending.receivedRanges, fragmentOffset, fragmentEnd)) {
        ++stats_.invalidDatagrams;
        return;
    }

    ++stats_.audioDatagramsAccepted;
    if (!currentDatagramEndpoint_.empty()) {
        stats_.latestMediaEndpoint = currentDatagramEndpoint_;
    }
    stats_.audioPayloadBytesReceived += payloadBytes;

    std::memcpy(pending.bytes.data() + fragmentOffset, payloadData, payloadBytes);
    pending.fragmentReceived[fragmentIndex] = 1;
    ++pending.receivedFragments;
    pending.receivedBytes += payloadBytes;

    if (pending.receivedFragments != pending.fragmentCount || pending.receivedBytes != pending.packetBytes) {
        EnforcePendingAudioPacketLimit();
        return;
    }

    const bool hadPreviousFormat = stats_.audioPacketsCompleted > 0;
    if (hadPreviousFormat &&
        (stats_.audioSampleRate != pending.sampleRate ||
         stats_.audioChannels != pending.channels ||
         stats_.audioBitsPerSample != pending.bitsPerSample ||
         stats_.audioBlockAlign != pending.blockAlign ||
         stats_.audioSampleFormat != pending.sampleFormat ||
         stats_.audioCodec != pending.codec)) {
        ++stats_.audioFormatChanges;
    }

    ++stats_.audioPacketsCompleted;
    stats_.audioCompletedPacketBytes += pending.packetBytes;
    stats_.audioFramesCompleted += pending.audioFrames;
    if ((pending.flags & udp_protocol::AudioPacketFlagSilent) != 0) {
        ++stats_.audioSilentPackets;
    }
    if ((pending.flags & udp_protocol::AudioPacketFlagDataDiscontinuity) != 0) {
        ++stats_.audioDiscontinuities;
    }
    if ((pending.flags & udp_protocol::AudioPacketFlagTimestampError) != 0) {
        ++stats_.audioTimestampErrors;
    }
    stats_.latestAudioPacketId = pending.packetId;
    stats_.latestAudioDevicePosition = pending.devicePosition;
    stats_.latestAudioQpcPosition = pending.qpcPosition;
    stats_.audioSampleRate = pending.sampleRate;
    stats_.audioChannels = pending.channels;
    stats_.audioBitsPerSample = pending.bitsPerSample;
    stats_.audioBlockAlign = pending.blockAlign;
    stats_.audioSampleFormat = pending.sampleFormat;
    stats_.audioCodec = pending.codec;

    UdpCompletedAudioPacket completed;
    completed.packetId = pending.packetId;
    completed.devicePosition = pending.devicePosition;
    completed.qpcPosition = pending.qpcPosition;
    completed.sampleRate = pending.sampleRate;
    completed.channels = pending.channels;
    completed.bitsPerSample = pending.bitsPerSample;
    completed.blockAlign = pending.blockAlign;
    completed.sampleFormat = pending.sampleFormat;
    completed.codec = pending.codec;
    completed.audioFrames = pending.audioFrames;
    completed.flags = pending.flags;
    completed.fragmentCount = pending.fragmentCount;
    completed.bytes = std::move(pending.bytes);

    pendingAudioPackets_.erase(it);
    completedAudioPackets_.push_back(std::move(completed));
    ++stats_.audioPacketsQueued;
    EnforceCompletedAudioPacketLimit();
}

std::optional<std::vector<std::byte>> UdpReceiver::DecryptDatagramPayload(
    const UdpAesGcm& crypto,
    const std::byte* datagram,
    int datagramBytes,
    size_t headerBytes,
    size_t authenticatedHeaderBytes,
    std::span<const std::byte, UdpCryptoNonceBytes> nonce,
    std::span<const std::byte, UdpCryptoTagBytes> tag)
{
    if (datagramBytes < static_cast<int>(headerBytes)) {
        return std::nullopt;
    }

    return crypto.Decrypt(
        nonce,
        std::span<const std::byte>(datagram, authenticatedHeaderBytes),
        std::span<const std::byte>(datagram + headerBytes, static_cast<size_t>(datagramBytes) - headerBytes),
        tag);
}

UdpAesGcm* UdpReceiver::DecryptCryptoForSalt(const UdpCryptoSessionSalt& salt)
{
    // Receive-thread only, so peerDecryptCryptos_ needs no extra locking.
    if (!encryptionEnabled_) {
        return nullptr;
    }
    for (auto& entry : peerDecryptCryptos_) {
        if (entry.salt == salt) {
            return entry.crypto.get();
        }
    }
    // Bound the cache; a spoofed salt just yields a key whose GCM tag check
    // fails, so evicting the oldest entry is harmless.
    constexpr size_t MaxPeerDecryptCryptos = 16;
    if (peerDecryptCryptos_.size() >= MaxPeerDecryptCryptos) {
        peerDecryptCryptos_.erase(peerDecryptCryptos_.begin());
    }
    PeerDecryptCrypto added;
    added.salt = salt;
    added.crypto = std::make_unique<UdpAesGcm>(DeriveUdpSessionKey(master_, salt));
    peerDecryptCryptos_.push_back(std::move(added));
    return peerDecryptCryptos_.back().crypto.get();
}

bool UdpReceiver::EncryptFeedbackDatagram(std::vector<std::byte>& datagram)
{
    if (!encryptionEnabled_) {
        return false;
    }
    if (datagram.size() != sizeof(udp_protocol::FeedbackPacket)) {
        throw std::runtime_error("Feedback datagram has unexpected size for encryption");
    }

    udp_protocol::FeedbackPacket packet{};
    std::memcpy(&packet, datagram.data(), sizeof(packet));
    packet.flags = udp_protocol::ToNetwork32(udp_protocol::PacketFlagEncrypted);
    WriteUdpCryptoNonce(
        std::span<std::byte, UdpCryptoNonceBytes>(packet.encryptionNonce, UdpCryptoNonceBytes),
        UdpCryptoRole::Feedback,
        udp_protocol::FromNetwork64(packet.sequence),
        0);
    std::memcpy(packet.sessionSalt, sessionSalt_.data(), sizeof(packet.sessionSalt));
    std::memcpy(datagram.data(), &packet, sizeof(packet));

    auto ciphertext = encryptCrypto_->Encrypt(
        std::span<const std::byte, UdpCryptoNonceBytes>(packet.encryptionNonce, UdpCryptoNonceBytes),
        std::span<const std::byte>(datagram.data(), udp_protocol::FeedbackPacketAuthenticatedBytes),
        std::span<const std::byte>(
            datagram.data() + udp_protocol::FeedbackPacketEncryptedPayloadOffset,
            datagram.size() - udp_protocol::FeedbackPacketEncryptedPayloadOffset),
        std::span<std::byte, UdpCryptoTagBytes>(
            datagram.data() + offsetof(udp_protocol::FeedbackPacket, encryptionTag),
            UdpCryptoTagBytes));
    std::memcpy(
        datagram.data() + udp_protocol::FeedbackPacketEncryptedPayloadOffset,
        ciphertext.data(),
        ciphertext.size());
    return true;
}

bool UdpReceiver::EncryptControlDatagram(std::vector<std::byte>& datagram)
{
    if (!encryptionEnabled_) {
        return false;
    }
    if (datagram.size() != sizeof(udp_protocol::ControlPacket)) {
        throw std::runtime_error("Control datagram has unexpected size for encryption");
    }

    udp_protocol::ControlPacket packet{};
    std::memcpy(&packet, datagram.data(), sizeof(packet));
    packet.flags = udp_protocol::ToNetwork32(udp_protocol::PacketFlagEncrypted);
    WriteUdpCryptoNonce(
        std::span<std::byte, UdpCryptoNonceBytes>(packet.encryptionNonce, UdpCryptoNonceBytes),
        UdpCryptoRole::Control,
        udp_protocol::FromNetwork64(packet.sequence),
        0);
    std::memcpy(packet.sessionSalt, sessionSalt_.data(), sizeof(packet.sessionSalt));
    std::memcpy(datagram.data(), &packet, sizeof(packet));

    auto ciphertext = encryptCrypto_->Encrypt(
        std::span<const std::byte, UdpCryptoNonceBytes>(packet.encryptionNonce, UdpCryptoNonceBytes),
        std::span<const std::byte>(datagram.data(), udp_protocol::ControlPacketAuthenticatedBytes),
        std::span<const std::byte>(
            datagram.data() + udp_protocol::ControlPacketEncryptedPayloadOffset,
            datagram.size() - udp_protocol::ControlPacketEncryptedPayloadOffset),
        std::span<std::byte, UdpCryptoTagBytes>(
            datagram.data() + offsetof(udp_protocol::ControlPacket, encryptionTag),
            UdpCryptoTagBytes));
    std::memcpy(
        datagram.data() + udp_protocol::ControlPacketEncryptedPayloadOffset,
        ciphertext.data(),
        ciphertext.size());
    return true;
}

void UdpReceiver::ProcessControlDatagram(const std::byte* datagram, int datagramBytes)
{
    if (datagramBytes != static_cast<int>(sizeof(udp_protocol::ControlPacket))) {
        ++stats_.invalidDatagrams;
        return;
    }

    udp_protocol::ControlPacket header{};
    std::memcpy(&header, datagram, sizeof(header));
    const uint32_t flags = udp_protocol::FromNetwork32(header.flags);

    // Reject unauthenticated plaintext control on an encrypted session. A
    // successful AES-GCM decrypt is the only proof the sender holds the room
    // key; without this guard a forged plaintext grant/revoke would be accepted
    // (mirrors the host-side reject in UdpSender::ProcessControlPacket).
    if ((flags & udp_protocol::PacketFlagEncrypted) == 0 && encryptionEnabled_) {
        ++stats_.cryptoRejectedDatagrams;
        return;
    }

    std::optional<std::vector<std::byte>> payload;
    if ((flags & udp_protocol::PacketFlagEncrypted) != 0) {
        UdpCryptoSessionSalt salt{};
        std::memcpy(salt.data(), header.sessionSalt, salt.size());
        UdpAesGcm* crypto = DecryptCryptoForSalt(salt);
        if (crypto == nullptr) {
            ++stats_.cryptoRejectedDatagrams;
            return;
        }
        payload = DecryptDatagramPayload(
            *crypto,
            datagram,
            datagramBytes,
            udp_protocol::ControlPacketEncryptedPayloadOffset,
            udp_protocol::ControlPacketAuthenticatedBytes,
            std::span<const std::byte, UdpCryptoNonceBytes>(header.encryptionNonce, UdpCryptoNonceBytes),
            std::span<const std::byte, UdpCryptoTagBytes>(header.encryptionTag, UdpCryptoTagBytes));
    } else {
        // Plaintext session: pass the payload region through unchanged.
        payload = std::vector<std::byte>(
            datagram + udp_protocol::ControlPacketEncryptedPayloadOffset,
            datagram + datagramBytes);
    }
    if (!payload ||
        payload->size() != sizeof(udp_protocol::ControlPacket) - udp_protocol::ControlPacketEncryptedPayloadOffset) {
        ++stats_.cryptoRejectedDatagrams;
        return;
    }

    std::vector<std::byte> reassembled(sizeof(udp_protocol::ControlPacket));
    std::memcpy(reassembled.data(), datagram, udp_protocol::ControlPacketEncryptedPayloadOffset);
    std::memcpy(
        reassembled.data() + udp_protocol::ControlPacketEncryptedPayloadOffset,
        payload->data(),
        payload->size());

    auto message = udp_protocol::ParseControlDatagram(reassembled);
    if (!message) {
        ++stats_.invalidDatagrams;
        return;
    }
    // No separate access-code check here: for encrypted sessions a successful
    // AES-GCM decrypt already proves the sender holds the room key, and an
    // explicit fingerprint compare only risks false rejects when the two sides'
    // fingerprints are plumbed inconsistently.
    if (onControl_) {
        onControl_(*message);
    }
}

void UdpReceiver::QueueDelayedDatagram(const std::byte* datagram, int datagramBytes, Clock::time_point releaseAt)
{
    DelayedDatagram delayed;
    delayed.releaseAt = releaseAt;
    delayed.bytes.assign(datagram, datagram + datagramBytes);

    const auto insertion = std::upper_bound(
        delayedDatagrams_.begin(),
        delayedDatagrams_.end(),
        delayed.releaseAt,
        [](Clock::time_point releaseAtValue, const DelayedDatagram& queued) {
            return releaseAtValue < queued.releaseAt;
        });
    delayedDatagrams_.insert(insertion, std::move(delayed));
}

bool UdpReceiver::ShouldSimulateLoss()
{
    if (config_.simulatedLossPercent <= 0.0f) {
        return false;
    }
    if (config_.simulatedLossPercent >= 100.0f) {
        return true;
    }

    std::uniform_real_distribution<float> distribution(0.0f, 100.0f);
    return distribution(simulationRng_) < config_.simulatedLossPercent;
}

std::chrono::milliseconds UdpReceiver::NextSimulatedJitterDelay()
{
    if (config_.simulatedJitter.count() <= 0) {
        return std::chrono::milliseconds(0);
    }

    std::uniform_int_distribution<int64_t> distribution(0, config_.simulatedJitter.count());
    return std::chrono::milliseconds(distribution(simulationRng_));
}

std::chrono::milliseconds UdpReceiver::WaitUntilNextDelayedDatagram(Clock::time_point now) const
{
    if (delayedDatagrams_.empty()) {
        return std::chrono::milliseconds(-1);
    }
    if (delayedDatagrams_.front().releaseAt <= now) {
        return std::chrono::milliseconds(0);
    }

    return std::chrono::duration_cast<std::chrono::milliseconds>(delayedDatagrams_.front().releaseAt - now);
}

void UdpReceiver::DropExpiredFrames(Clock::time_point now)
{
    for (auto it = pendingFrames_.begin(); it != pendingFrames_.end();) {
        if (now - it->second.lastUpdated > config_.frameTimeout) {
            it = pendingFrames_.erase(it);
            ++stats_.incompleteFramesDropped;
        } else {
            ++it;
        }
    }

    for (auto it = pendingAudioPackets_.begin(); it != pendingAudioPackets_.end();) {
        if (now - it->second.lastUpdated > config_.frameTimeout) {
            it = pendingAudioPackets_.erase(it);
            ++stats_.audioIncompletePacketsDropped;
        } else {
            ++it;
        }
    }
}

void UdpReceiver::EnforcePendingFrameLimit()
{
    uint64_t totalBytes = 0;
    for (const auto& entry : pendingFrames_) {
        totalBytes += entry.second.frameBytes;
    }
    while (!pendingFrames_.empty() &&
           (pendingFrames_.size() > config_.maxPendingFrames ||
            totalBytes > config_.maxPendingFrameBytes)) {
        auto oldest = pendingFrames_.begin();
        for (auto it = pendingFrames_.begin(); it != pendingFrames_.end(); ++it) {
            if (it->second.lastUpdated < oldest->second.lastUpdated) {
                oldest = it;
            }
        }

        totalBytes -= oldest->second.frameBytes;
        pendingFrames_.erase(oldest);
        ++stats_.incompleteFramesDropped;
    }
}

void UdpReceiver::EnforcePendingAudioPacketLimit()
{
    uint64_t totalBytes = 0;
    for (const auto& entry : pendingAudioPackets_) {
        totalBytes += entry.second.packetBytes;
    }
    while (!pendingAudioPackets_.empty() &&
           (pendingAudioPackets_.size() > config_.maxPendingAudioPackets ||
            totalBytes > config_.maxPendingAudioBytes)) {
        auto oldest = pendingAudioPackets_.begin();
        for (auto it = pendingAudioPackets_.begin(); it != pendingAudioPackets_.end(); ++it) {
            if (it->second.lastUpdated < oldest->second.lastUpdated) {
                oldest = it;
            }
        }

        totalBytes -= oldest->second.packetBytes;
        pendingAudioPackets_.erase(oldest);
        ++stats_.audioIncompletePacketsDropped;
    }
}

void UdpReceiver::EnforceCompletedAudioPacketLimit()
{
    while (completedAudioPackets_.size() > config_.maxCompletedAudioPackets) {
        completedAudioPackets_.pop_front();
        ++stats_.audioQueuedPacketsDropped;
    }
}

uint16_t ParseUdpReceivePort(const char* value)
{
    char* end = nullptr;
    const long port = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || port <= 0 || port > 65535) {
        throw std::invalid_argument(std::string("Invalid UDP receive port: ") + value);
    }

    return static_cast<uint16_t>(port);
}

} // namespace screenshare
