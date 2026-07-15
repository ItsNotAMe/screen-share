#include "transport/UdpSender.h"

#include "transport/NatTraversal.h"
#include "transport/UdpProtocol.h"

#include <winsock2.h>
#include <mstcpip.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace screenshare {
namespace {

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
        return "unknown";
    }

    const auto* socketAddress = static_cast<const sockaddr*>(address);
    if (socketAddress->sa_family != AF_INET || addressLength < static_cast<int>(sizeof(sockaddr_in))) {
        return "unknown";
    }

    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
    std::array<char, INET_ADDRSTRLEN> host{};
    if (inet_ntop(AF_INET, &ipv4->sin_addr, host.data(), static_cast<socklen_t>(host.size())) == nullptr) {
        return "unknown";
    }

    return std::string(host.data()) + ":" + std::to_string(ntohs(ipv4->sin_port));
}

std::vector<std::byte> CopySocketAddress(const sockaddr* address, size_t addressLength)
{
    std::vector<std::byte> bytes(addressLength);
    std::memcpy(bytes.data(), address, addressLength);
    return bytes;
}

std::vector<std::byte> ResolveUdpAddress(const std::string& host, uint16_t port)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* resolved = nullptr;
    const std::string portText = std::to_string(port);
    const int addressResult = getaddrinfo(host.c_str(), portText.c_str(), &hints, &resolved);
    if (addressResult != 0 || resolved == nullptr) {
        throw std::runtime_error("getaddrinfo failed for UDP target " + host + ":" + portText);
    }

    std::vector<std::byte> address = CopySocketAddress(resolved->ai_addr, resolved->ai_addrlen);
    freeaddrinfo(resolved);
    return address;
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
    if (config.maxQueuedDatagrams == 0) {
        throw std::invalid_argument("UDP max queued datagrams must be non-zero");
    }
    if (config.maxQueueDelay < std::chrono::milliseconds(0)) {
        throw std::invalid_argument("UDP max queue delay must not be negative");
    }
    if (config.encryptionKey && config.accessCodeFingerprint == 0) {
        throw std::invalid_argument("UDP encryption requires an access code fingerprint");
    }
    if (config.maxNatProbeTargets == 0) {
        throw std::invalid_argument("UDP NAT probe target limit must be non-zero");
    }
    for (const auto& target : config.additionalTargets) {
        if (target.host.empty()) {
            throw std::invalid_argument("UDP additional target host is empty");
        }
        if (target.port == 0) {
            throw std::invalid_argument("UDP additional target port must be non-zero");
        }
    }

    std::unique_ptr<UdpAesGcm> crypto;
    uint32_t videoNoncePrefix = 0;
    uint32_t audioNoncePrefix = 0;
    uint32_t controlNoncePrefix = 0;
    if (config.encryptionKey) {
        crypto = std::make_unique<UdpAesGcm>(*config.encryptionKey);
        videoNoncePrefix = GenerateUdpCryptoNoncePrefix();
        audioNoncePrefix = GenerateUdpCryptoNoncePrefix();
        while (audioNoncePrefix == videoNoncePrefix) {
            audioNoncePrefix = GenerateUdpCryptoNoncePrefix();
        }
        controlNoncePrefix = GenerateUdpCryptoNoncePrefix();
        while (controlNoncePrefix == videoNoncePrefix || controlNoncePrefix == audioNoncePrefix) {
            controlNoncePrefix = GenerateUdpCryptoNoncePrefix();
        }
    }

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

    DisableUdpConnReset(udpSocket);

    if (config.localPort != 0) {
        sockaddr_in bindAddress{};
        bindAddress.sin_family = AF_INET;
        bindAddress.sin_addr.s_addr = htonl(INADDR_ANY);
        bindAddress.sin_port = htons(config.localPort);
        if (bind(udpSocket, reinterpret_cast<const sockaddr*>(&bindAddress), sizeof(bindAddress)) == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            closesocket(udpSocket);
            freeaddrinfo(resolved);
            std::string message = WinsockErrorMessage(
                "bind(sender 0.0.0.0:" + std::to_string(config.localPort) + ")",
                error);
            if (error == WSAEADDRINUSE) {
                message +=
                    ". The UDP local port is already in use; close the other process using it or create this "
                    "side's local invite with a different port, such as Watch on 5000 and Share on 5001 "
                    "when testing both windows on one PC.";
            }
            throw std::runtime_error(message);
        }
    }

    std::vector<GroupedAddress> additionalAddresses;
    additionalAddresses.reserve(config.additionalTargets.size());
    try {
        for (const auto& target : config.additionalTargets) {
            additionalAddresses.push_back(GroupedAddress{
                ResolveUdpAddress(target.host, target.port),
                target.group,
            });
        }
    } catch (...) {
        closesocket(udpSocket);
        freeaddrinfo(resolved);
        throw;
    }

    {
        std::lock_guard lock(mutex_);
        address_.resize(static_cast<size_t>(resolved->ai_addrlen));
        std::memcpy(address_.data(), resolved->ai_addr, static_cast<size_t>(resolved->ai_addrlen));

        socket_ = static_cast<uintptr_t>(udpSocket);
        addressLength_ = static_cast<int>(resolved->ai_addrlen);
        additionalAddresses_ = std::move(additionalAddresses);
        natProbeAddresses_.clear();
        cachedSendAddresses_.reset();
        feedbackPeers_.clear();
        config_ = config;
        stats_ = {};
        stats_.encryptionEnabled = crypto != nullptr;
        frameId_ = 0;
        audioPacketId_ = 0;
        queue_.clear();
        crypto_ = std::move(crypto);
        videoNoncePrefix_ = videoNoncePrefix;
        audioNoncePrefix_ = audioNoncePrefix;
        controlNoncePrefix_ = controlNoncePrefix;
        nextControlSequence_ = 1;
        controlPeers_.clear();
        nextSendAt_ = Clock::now();
        workerError_.clear();
        stopWorker_ = false;
        datagramInFlight_ = false;
        sendAddressesDirty_ = true;
    }

    freeaddrinfo(resolved);
    worker_ = std::thread(&UdpSender::WorkerLoop, this);
}

void UdpSender::Close()
{
    if (worker_.joinable()) {
        {
            std::lock_guard lock(mutex_);
            stopWorker_ = true;
            queue_.clear();
            UpdatePendingStatsLocked();
        }
        queueChanged_.notify_all();
        queueDrained_.notify_all();
        worker_.join();
    }

    if (socket_ != 0) {
        closesocket(AsSocket(socket_));
        socket_ = 0;
    }

    {
        std::lock_guard lock(mutex_);
        address_.clear();
        additionalAddresses_.clear();
        natProbeAddresses_.clear();
        cachedSendAddresses_.reset();
        feedbackPeers_.clear();
        addressLength_ = 0;
        crypto_.reset();
        videoNoncePrefix_ = 0;
        audioNoncePrefix_ = 0;
        controlNoncePrefix_ = 0;
        nextControlSequence_ = 1;
        controlPeers_.clear();
        workerError_.clear();
        stopWorker_ = false;
        datagramInFlight_ = false;
        sendAddressesDirty_ = true;
        UpdatePendingStatsLocked();
    }
}

bool UdpSender::AddAdditionalTarget(const UdpSenderEndpoint& target)
{
    if (!isOpen()) {
        throw std::logic_error("UdpSender::Open must be called before AddAdditionalTarget");
    }
    if (target.host.empty()) {
        throw std::invalid_argument("UDP additional target host is empty");
    }
    if (target.port == 0) {
        throw std::invalid_argument("UDP additional target port must be non-zero");
    }

    std::vector<std::byte> address = ResolveUdpAddress(target.host, target.port);
    std::lock_guard lock(mutex_);
    const auto matchesAddress = [&](const GroupedAddress& candidate) {
        return candidate.address == address;
    };
    if (address == address_ ||
        std::find_if(additionalAddresses_.begin(), additionalAddresses_.end(), matchesAddress) != additionalAddresses_.end() ||
        std::find_if(natProbeAddresses_.begin(), natProbeAddresses_.end(), matchesAddress) != natProbeAddresses_.end()) {
        return false;
    }

    additionalAddresses_.push_back(GroupedAddress{std::move(address), target.group});
    config_.additionalTargets.push_back(target);
    sendAddressesDirty_ = true;
    return true;
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

    if (fragmentCount32 > udp_protocol::MaxFragmentsPerFrame) {
        throw std::runtime_error("Encoded frame is too large for UDP fragmentation limit");
    }

    uint64_t frameId = 0;
    {
        std::lock_guard lock(mutex_);
        CheckWorkerErrorLocked();
        frameId = frameId_++;
    }

    const auto fragmentCount = static_cast<uint16_t>(fragmentCount32);
    const auto* data = packet.bytes.data();
    std::vector<PendingDatagram> datagrams;
    datagrams.reserve(fragmentCount);
    for (uint16_t fragmentIndex = 0; fragmentIndex < fragmentCount; ++fragmentIndex) {
        const uint32_t offset = static_cast<uint32_t>(fragmentIndex) * maxPayload;
        const uint32_t payloadBytes = std::min(maxPayload, frameBytes - offset);
        datagrams.push_back(PendingDatagram{
            BuildDatagram(data + offset, payloadBytes, offset, fragmentIndex, fragmentCount, frameId, packet),
            {},
            PendingDatagramKind::Video,
            frameId,
        });
    }

    {
        std::lock_guard lock(mutex_);
        CheckWorkerErrorLocked();

        const auto now = Clock::now();
        static_cast<void>(EnforceLiveQueueDelayLocked(now));
        DropQueuedMediaForCapacityLocked(datagrams.size(), PendingDatagramKind::Video);

        if (queue_.size() + datagrams.size() > config_.maxQueuedDatagrams) {
            ++stats_.framesDropped;
            stats_.datagramsDropped += datagrams.size();
            UpdatePendingStatsLocked();
            return;
        }

        if (!config_.pacingEnabled || config_.pacingBitrate == 0 || nextSendAt_ < now) {
            nextSendAt_ = now;
        }

        for (auto& datagram : datagrams) {
            datagram.sendAt = config_.pacingEnabled ? nextSendAt_ : now;
            if (config_.pacingEnabled) {
                nextSendAt_ += PacingDelayForBytes(datagram.bytes.size());
            }
            queue_.push_back(std::move(datagram));
        }

        ++stats_.framesSent;
        stats_.datagramsQueued += datagrams.size();
        stats_.payloadBytesSent += frameBytes;
        UpdatePendingStatsLocked();
    }
    queueChanged_.notify_one();
}

void UdpSender::SendAudioPacket(const UdpAudioPacket& packet)
{
    if (!isOpen()) {
        throw std::logic_error("UdpSender::Open must be called before SendAudioPacket");
    }
    if (packet.audioFrames == 0 || packet.bytes.empty()) {
        return;
    }
    if (packet.sampleRate == 0 || packet.channels == 0 || packet.bitsPerSample == 0 || packet.blockAlign == 0) {
        throw std::invalid_argument("Audio packet format is incomplete");
    }
    if (packet.sampleFormat == udp_protocol::AudioSampleFormat::Unknown) {
        throw std::invalid_argument("Audio packet sample format is unsupported");
    }
    if (packet.codec != udp_protocol::AudioCodec::Raw &&
        packet.codec != udp_protocol::AudioCodec::Opus) {
        throw std::invalid_argument("Audio packet codec is unsupported");
    }
    if (packet.bytes.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Audio packet is too large for UDP sender");
    }
    if (packet.codec == udp_protocol::AudioCodec::Raw &&
        static_cast<uint64_t>(packet.audioFrames) * static_cast<uint64_t>(packet.blockAlign) != packet.bytes.size()) {
        throw std::invalid_argument("Audio packet byte count does not match frame count and block alignment");
    }

    const uint32_t maxPayload = config_.maxPayloadBytes;
    const uint32_t packetBytes = static_cast<uint32_t>(packet.bytes.size());
    const uint32_t fragmentCount32 = (packetBytes + maxPayload - 1) / maxPayload;
    if (fragmentCount32 == 0) {
        return;
    }
    if (fragmentCount32 > udp_protocol::MaxFragmentsPerFrame) {
        throw std::runtime_error("Audio packet is too large for UDP fragmentation limit");
    }

    uint64_t packetId = 0;
    {
        std::lock_guard lock(mutex_);
        CheckWorkerErrorLocked();
        packetId = audioPacketId_++;
    }

    const auto fragmentCount = static_cast<uint16_t>(fragmentCount32);
    const auto* data = packet.bytes.data();
    std::vector<PendingDatagram> datagrams;
    datagrams.reserve(fragmentCount);
    for (uint16_t fragmentIndex = 0; fragmentIndex < fragmentCount; ++fragmentIndex) {
        const uint32_t offset = static_cast<uint32_t>(fragmentIndex) * maxPayload;
        const uint32_t payloadBytes = std::min(maxPayload, packetBytes - offset);
        datagrams.push_back(PendingDatagram{
            BuildAudioDatagram(data + offset, payloadBytes, offset, fragmentIndex, fragmentCount, packetId, packet),
            {},
            PendingDatagramKind::Audio,
            packetId,
        });
    }

    {
        std::lock_guard lock(mutex_);
        CheckWorkerErrorLocked();

        const auto now = Clock::now();
        static_cast<void>(EnforceLiveQueueDelayLocked(now));
        DropQueuedMediaForCapacityLocked(datagrams.size(), PendingDatagramKind::Video);

        if (queue_.size() + datagrams.size() > config_.maxQueuedDatagrams) {
            ++stats_.audioPacketsDropped;
            stats_.datagramsDropped += datagrams.size();
            UpdatePendingStatsLocked();
            return;
        }

        if (!config_.pacingEnabled || config_.pacingBitrate == 0 || nextSendAt_ < now) {
            nextSendAt_ = now;
        }

        for (auto& datagram : datagrams) {
            datagram.sendAt = config_.pacingEnabled ? nextSendAt_ : now;
            if (config_.pacingEnabled) {
                nextSendAt_ += PacingDelayForBytes(datagram.bytes.size());
            }
            queue_.push_back(std::move(datagram));
        }

        ++stats_.audioPacketsSent;
        stats_.audioFramesSent += packet.audioFrames;
        stats_.audioDatagramsQueued += datagrams.size();
        stats_.audioPayloadBytesSent += packetBytes;
        stats_.datagramsQueued += datagrams.size();
        stats_.payloadBytesSent += packetBytes;
        UpdatePendingStatsLocked();
    }
    queueChanged_.notify_one();
}

void UdpSender::SetPacingBitrate(uint32_t bitrate)
{
    bool notify = false;
    {
        std::lock_guard lock(mutex_);
        CheckWorkerErrorLocked();
        const bool bitrateChanged = config_.pacingBitrate != bitrate;
        config_.pacingBitrate = bitrate;
        const auto now = Clock::now();
        if (config_.pacingEnabled && !queue_.empty()) {
            if (bitrateChanged) {
                RescheduleQueueLocked(now);
            }
            notify = bitrateChanged || EnforceLiveQueueDelayLocked(now);
        } else if (config_.pacingEnabled && nextSendAt_ < now) {
            nextSendAt_ = now;
            UpdatePendingStatsLocked();
        }
    }
    if (notify) {
        queueChanged_.notify_one();
    }
}

void UdpSender::SetPacingEnabled(bool enabled)
{
    bool notify = false;
    {
        std::lock_guard lock(mutex_);
        CheckWorkerErrorLocked();
        if (config_.pacingEnabled == enabled) {
            return;
        }
        config_.pacingEnabled = enabled;
        const auto now = Clock::now();
        if (!enabled) {
            // Pacing off: release any held datagrams to send immediately.
            nextSendAt_ = now;
            if (!queue_.empty()) {
                RescheduleQueueLocked(now);
                notify = true;
            }
        }
    }
    if (notify) {
        queueChanged_.notify_one();
    }
}

bool UdpSender::isOpen() const noexcept
{
    return socket_ != 0 && !address_.empty();
}

UdpSenderStats UdpSender::stats() const
{
    std::lock_guard lock(mutex_);
    UdpSenderStats snapshot = stats_;
    snapshot.feedbackPeers = feedbackPeers_;
    snapshot.natProbeTargetCount = static_cast<uint64_t>(natProbeAddresses_.size());
    snapshot.pendingDatagrams =
        static_cast<uint64_t>(queue_.size()) + (datagramInFlight_ ? 1ULL : 0ULL);
    const auto now = Clock::now();
    if (config_.pacingEnabled && !queue_.empty() && nextSendAt_ > now) {
        snapshot.pendingQueueDelayMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(nextSendAt_ - now).count());
    } else {
        snapshot.pendingQueueDelayMs = 0;
    }
    return snapshot;
}

void UdpSender::Flush()
{
    std::unique_lock lock(mutex_);
    queueDrained_.wait(lock, [&] {
        return !workerError_.empty() || (queue_.empty() && !datagramInFlight_);
    });
    CheckWorkerErrorLocked();
}

std::optional<udp_protocol::FeedbackSnapshot> UdpSender::ReceiveFeedback(std::chrono::milliseconds timeout)
{
    if (!isOpen()) {
        return std::nullopt;
    }

    constexpr int MaxDatagramsToInspect = 1024;
    for (int inspected = 0; inspected < MaxDatagramsToInspect; ++inspected) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(AsSocket(socket_), &readSet);

        const auto wait = inspected == 0 ? timeout : std::chrono::milliseconds(0);
        timeval waitTime{};
        waitTime.tv_sec = static_cast<long>(wait.count() / 1000);
        waitTime.tv_usec = static_cast<long>((wait.count() % 1000) * 1000);

        const int ready = select(0, &readSet, nullptr, nullptr, &waitTime);
        if (ready == SOCKET_ERROR) {
            throw std::runtime_error(WinsockErrorMessage("select(feedback)"));
        }
        if (ready == 0) {
            return std::nullopt;
        }

        std::array<std::byte, 512> buffer{};
        sockaddr_storage senderAddress{};
        int senderAddressLength = sizeof(senderAddress);
        const int received = recvfrom(
            AsSocket(socket_),
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&senderAddress),
            &senderAddressLength);
        if (received == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAECONNRESET) {
                return std::nullopt;
            }
            throw std::runtime_error(WinsockErrorMessage("recvfrom(feedback)"));
        }

        const std::span<const std::byte> receivedDatagram(
            buffer.data(),
            static_cast<size_t>(received));
        if (const auto probe = ParseNatProbeDatagram(receivedDatagram)) {
            MaybeRetargetFromNatProbe(&senderAddress, senderAddressLength, *probe);
            continue;
        }
        if (receivedDatagram.size() >= sizeof(uint32_t)) {
            uint32_t rawMagic = 0;
            std::memcpy(&rawMagic, receivedDatagram.data(), sizeof(rawMagic));
            if (udp_protocol::FromNetwork32(rawMagic) == udp_protocol::ControlMagic) {
                ProcessControlPacket(&senderAddress, senderAddressLength, receivedDatagram);
                continue;
            }
        }

        std::optional<std::vector<std::byte>> decryptedFeedbackDatagram;
        std::span<const std::byte> parseDatagram = receivedDatagram;
        bool cryptoRejected = false;
        if (receivedDatagram.size() == sizeof(udp_protocol::FeedbackPacket)) {
            udp_protocol::FeedbackPacket packet{};
            std::memcpy(&packet, receivedDatagram.data(), sizeof(packet));
            const uint32_t flags = udp_protocol::FromNetwork32(packet.flags);
            if ((flags & udp_protocol::PacketFlagEncrypted) != 0) {
                if (!crypto_) {
                    cryptoRejected = true;
                } else {
                    decryptedFeedbackDatagram = DecryptFeedbackDatagram(receivedDatagram);
                    if (decryptedFeedbackDatagram) {
                        parseDatagram = std::span<const std::byte>(
                            decryptedFeedbackDatagram->data(),
                            decryptedFeedbackDatagram->size());
                    } else {
                        cryptoRejected = true;
                    }
                }
            } else if (crypto_) {
                cryptoRejected = true;
            }
        } else if (crypto_) {
            cryptoRejected = true;
        }

        std::lock_guard lock(mutex_);
        if (cryptoRejected) {
            ++stats_.feedbackCryptoRejected;
            continue;
        }

        const auto feedback = udp_protocol::ParseFeedbackDatagram(parseDatagram);
        if (!feedback) {
            ++stats_.invalidFeedbackPackets;
            continue;
        }
        if (config_.accessCodeFingerprint != 0 &&
            feedback->accessCodeFingerprint != config_.accessCodeFingerprint) {
            ++stats_.feedbackAccessRejected;
            continue;
        }

        std::vector<std::byte> feedbackAddress(static_cast<size_t>(senderAddressLength));
        std::memcpy(feedbackAddress.data(), &senderAddress, static_cast<size_t>(senderAddressLength));
        const uint32_t feedbackGroup = EndpointGroupForAddressLocked(feedbackAddress);
        const std::string feedbackEndpoint = SocketAddressToString(&senderAddress, senderAddressLength);

        // Feedback and control arrive from the same viewer socket, so record this
        // endpoint as a control-ack target now. That lets the host send a grant
        // (SendControlTo) immediately when it toggles a permission, without
        // waiting for the viewer to first send a "request control" packet.
        {
            auto controlPeer = std::find_if(
                controlPeers_.begin(),
                controlPeers_.end(),
                [&](const ControlPeer& candidate) { return candidate.endpoint == feedbackEndpoint; });
            if (controlPeer == controlPeers_.end()) {
                ControlPeer added;
                added.endpoint = feedbackEndpoint;
                added.address = feedbackAddress;
                added.addressLength = senderAddressLength;
                controlPeers_.push_back(std::move(added));
            } else {
                controlPeer->address = feedbackAddress;
                controlPeer->addressLength = senderAddressLength;
            }
        }

        ++stats_.feedbackPacketsReceived;
        stats_.hasFeedback = true;
        stats_.latestFeedback = *feedback;
        auto peer = std::find_if(
            feedbackPeers_.begin(),
            feedbackPeers_.end(),
            [&](const UdpSenderStats::FeedbackPeer& candidate) {
                return candidate.endpoint == feedbackEndpoint;
            });
        if (peer == feedbackPeers_.end()) {
            feedbackPeers_.push_back(UdpSenderStats::FeedbackPeer{
                feedbackEndpoint,
                feedbackGroup,
                1,
                *feedback,
            });
        } else {
            peer->group = feedbackGroup;
            ++peer->packetsReceived;
            peer->latestFeedback = *feedback;
        }
        return feedback;
    }

    return std::nullopt;
}

std::vector<std::byte> UdpSender::BuildDatagram(
    const std::byte* payload,
    uint32_t payloadBytes,
    uint32_t fragmentOffset,
    uint16_t fragmentIndex,
    uint16_t fragmentCount,
    uint64_t frameId,
    const EncodedPacket& packet)
{
    udp_protocol::PacketHeader header;
    header.magic = udp_protocol::ToNetwork32(udp_protocol::PacketMagic);
    header.version = udp_protocol::ToNetwork16(udp_protocol::PacketVersion);
    header.headerBytes = udp_protocol::ToNetwork16(static_cast<uint16_t>(sizeof(udp_protocol::PacketHeader)));
    header.frameId = udp_protocol::ToNetwork64(frameId);
    header.timestamp100ns = udp_protocol::ToNetwork64(static_cast<uint64_t>(packet.timestamp100ns));
    header.senderQpc100ns = udp_protocol::ToNetwork64(static_cast<uint64_t>(packet.senderQpc100ns));
    header.accessCodeFingerprint = udp_protocol::ToNetwork64(config_.accessCodeFingerprint);
    header.frameBytes = udp_protocol::ToNetwork32(static_cast<uint32_t>(packet.bytes.size()));
    header.fragmentOffset = udp_protocol::ToNetwork32(fragmentOffset);
    header.fragmentIndex = udp_protocol::ToNetwork16(fragmentIndex);
    header.fragmentCount = udp_protocol::ToNetwork16(fragmentCount);
    header.payloadBytes = udp_protocol::ToNetwork32(payloadBytes);
    if (crypto_) {
        header.flags = udp_protocol::ToNetwork32(udp_protocol::PacketFlagEncrypted);
        WriteUdpCryptoNonce(
            std::span<std::byte, UdpCryptoNonceBytes>(header.encryptionNonce, UdpCryptoNonceBytes),
            videoNoncePrefix_,
            frameId,
            fragmentIndex);
    }

    std::vector<std::byte> datagram(sizeof(udp_protocol::PacketHeader) + payloadBytes);
    std::memcpy(datagram.data(), &header, sizeof(header));
    std::memcpy(datagram.data() + sizeof(header), payload, payloadBytes);
    if (crypto_) {
        EncryptDatagramPayload(
            datagram,
            sizeof(udp_protocol::PacketHeader),
            udp_protocol::PacketHeaderAuthenticatedBytes,
            std::span<const std::byte, UdpCryptoNonceBytes>(header.encryptionNonce, UdpCryptoNonceBytes),
            std::span<std::byte, UdpCryptoTagBytes>(
                datagram.data() + offsetof(udp_protocol::PacketHeader, encryptionTag),
                UdpCryptoTagBytes));
    }

    return datagram;
}

std::vector<std::byte> UdpSender::BuildAudioDatagram(
    const std::byte* payload,
    uint32_t payloadBytes,
    uint32_t fragmentOffset,
    uint16_t fragmentIndex,
    uint16_t fragmentCount,
    uint64_t packetId,
    const UdpAudioPacket& packet)
{
    udp_protocol::AudioPacketHeader header;
    header.magic = udp_protocol::ToNetwork32(udp_protocol::AudioMagic);
    header.version = udp_protocol::ToNetwork16(udp_protocol::PacketVersion);
    header.headerBytes = udp_protocol::ToNetwork16(static_cast<uint16_t>(sizeof(udp_protocol::AudioPacketHeader)));
    header.packetId = udp_protocol::ToNetwork64(packetId);
    header.devicePosition = udp_protocol::ToNetwork64(packet.devicePosition);
    header.qpcPosition = udp_protocol::ToNetwork64(packet.qpcPosition);
    header.accessCodeFingerprint = udp_protocol::ToNetwork64(config_.accessCodeFingerprint);
    header.sampleRate = udp_protocol::ToNetwork32(packet.sampleRate);
    header.channels = udp_protocol::ToNetwork16(packet.channels);
    header.bitsPerSample = udp_protocol::ToNetwork16(packet.bitsPerSample);
    header.blockAlign = udp_protocol::ToNetwork16(packet.blockAlign);
    header.sampleFormat = udp_protocol::ToNetwork16(static_cast<uint16_t>(packet.sampleFormat));
    header.codec = udp_protocol::ToNetwork16(static_cast<uint16_t>(packet.codec));
    header.audioFrames = udp_protocol::ToNetwork32(packet.audioFrames);
    header.packetBytes = udp_protocol::ToNetwork32(static_cast<uint32_t>(packet.bytes.size()));
    header.fragmentOffset = udp_protocol::ToNetwork32(fragmentOffset);
    header.fragmentIndex = udp_protocol::ToNetwork16(fragmentIndex);
    header.fragmentCount = udp_protocol::ToNetwork16(fragmentCount);
    header.payloadBytes = udp_protocol::ToNetwork32(payloadBytes);
    header.flags = udp_protocol::ToNetwork32(packet.flags);
    if (crypto_) {
        header.encryptionFlags = udp_protocol::ToNetwork32(udp_protocol::PacketFlagEncrypted);
        WriteUdpCryptoNonce(
            std::span<std::byte, UdpCryptoNonceBytes>(header.encryptionNonce, UdpCryptoNonceBytes),
            audioNoncePrefix_,
            packetId,
            fragmentIndex);
    }

    std::vector<std::byte> datagram(sizeof(udp_protocol::AudioPacketHeader) + payloadBytes);
    std::memcpy(datagram.data(), &header, sizeof(header));
    std::memcpy(datagram.data() + sizeof(header), payload, payloadBytes);
    if (crypto_) {
        EncryptDatagramPayload(
            datagram,
            sizeof(udp_protocol::AudioPacketHeader),
            udp_protocol::AudioPacketHeaderAuthenticatedBytes,
            std::span<const std::byte, UdpCryptoNonceBytes>(header.encryptionNonce, UdpCryptoNonceBytes),
            std::span<std::byte, UdpCryptoTagBytes>(
                datagram.data() + offsetof(udp_protocol::AudioPacketHeader, encryptionTag),
                UdpCryptoTagBytes));
    }

    return datagram;
}

void UdpSender::EncryptDatagramPayload(
    std::vector<std::byte>& datagram,
    size_t headerBytes,
    size_t authenticatedHeaderBytes,
    std::span<const std::byte, UdpCryptoNonceBytes> nonce,
    std::span<std::byte, UdpCryptoTagBytes> tag)
{
    if (!crypto_) {
        return;
    }
    const auto ciphertext = crypto_->Encrypt(
        nonce,
        std::span<const std::byte>(datagram.data(), authenticatedHeaderBytes),
        std::span<const std::byte>(datagram.data() + headerBytes, datagram.size() - headerBytes),
        tag);
    std::memcpy(datagram.data() + headerBytes, ciphertext.data(), ciphertext.size());
}

std::optional<std::vector<std::byte>> UdpSender::DecryptFeedbackDatagram(std::span<const std::byte> datagram)
{
    if (!crypto_ || datagram.size() != sizeof(udp_protocol::FeedbackPacket)) {
        return std::nullopt;
    }

    udp_protocol::FeedbackPacket packet{};
    std::memcpy(&packet, datagram.data(), sizeof(packet));
    if ((udp_protocol::FromNetwork32(packet.flags) & udp_protocol::PacketFlagEncrypted) == 0) {
        return std::vector<std::byte>(datagram.begin(), datagram.end());
    }

    auto plaintext = crypto_->Decrypt(
        std::span<const std::byte, UdpCryptoNonceBytes>(packet.encryptionNonce, UdpCryptoNonceBytes),
        std::span<const std::byte>(datagram.data(), udp_protocol::FeedbackPacketAuthenticatedBytes),
        std::span<const std::byte>(
            datagram.data() + udp_protocol::FeedbackPacketEncryptedPayloadOffset,
            datagram.size() - udp_protocol::FeedbackPacketEncryptedPayloadOffset),
        std::span<const std::byte, UdpCryptoTagBytes>(packet.encryptionTag, UdpCryptoTagBytes));
    if (!plaintext) {
        return std::nullopt;
    }

    std::vector<std::byte> decrypted(datagram.begin(), datagram.end());
    std::memcpy(
        decrypted.data() + udp_protocol::FeedbackPacketEncryptedPayloadOffset,
        plaintext->data(),
        plaintext->size());
    return decrypted;
}

void UdpSender::SetControlHandler(
    std::function<void(const std::string& endpoint, const udp_protocol::ControlMessage&)> handler)
{
    onControl_ = std::move(handler);
}

bool UdpSender::EncryptControlDatagram(std::vector<std::byte>& datagram)
{
    if (!crypto_) {
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
        controlNoncePrefix_,
        udp_protocol::FromNetwork64(packet.sequence),
        0);
    std::memcpy(datagram.data(), &packet, sizeof(packet));

    EncryptDatagramPayload(
        datagram,
        udp_protocol::ControlPacketEncryptedPayloadOffset,
        udp_protocol::ControlPacketAuthenticatedBytes,
        std::span<const std::byte, UdpCryptoNonceBytes>(packet.encryptionNonce, UdpCryptoNonceBytes),
        std::span<std::byte, UdpCryptoTagBytes>(
            datagram.data() + offsetof(udp_protocol::ControlPacket, encryptionTag),
            UdpCryptoTagBytes));
    return true;
}

void UdpSender::ProcessControlPacket(const void* address, int addressLength, std::span<const std::byte> datagram)
{
    if (datagram.size() != sizeof(udp_protocol::ControlPacket)) {
        return;
    }

    udp_protocol::ControlPacket header{};
    std::memcpy(&header, datagram.data(), sizeof(header));
    const uint32_t flags = udp_protocol::FromNetwork32(header.flags);

    std::vector<std::byte> reassembled(datagram.begin(), datagram.end());
    if ((flags & udp_protocol::PacketFlagEncrypted) != 0) {
        if (!crypto_) {
            return;
        }
        auto plaintext = crypto_->Decrypt(
            std::span<const std::byte, UdpCryptoNonceBytes>(header.encryptionNonce, UdpCryptoNonceBytes),
            std::span<const std::byte>(datagram.data(), udp_protocol::ControlPacketAuthenticatedBytes),
            std::span<const std::byte>(
                datagram.data() + udp_protocol::ControlPacketEncryptedPayloadOffset,
                datagram.size() - udp_protocol::ControlPacketEncryptedPayloadOffset),
            std::span<const std::byte, UdpCryptoTagBytes>(header.encryptionTag, UdpCryptoTagBytes));
        if (!plaintext) {
            return;
        }
        std::memcpy(
            reassembled.data() + udp_protocol::ControlPacketEncryptedPayloadOffset,
            plaintext->data(),
            plaintext->size());
    } else if (crypto_) {
        return; // reject plaintext control when the session is encrypted
    }

    auto message = udp_protocol::ParseControlDatagram(reassembled);
    if (!message) {
        return;
    }
    // A successful decrypt already authenticates the sender for encrypted
    // sessions; skip a redundant access-code fingerprint compare that can falsely
    // reject when the two sides plumb the fingerprint differently.

    const std::string endpoint = SocketAddressToString(address, addressLength);
    bool accepted = false;
    {
        std::lock_guard lock(mutex_);
        auto peer = std::find_if(
            controlPeers_.begin(),
            controlPeers_.end(),
            [&](const ControlPeer& candidate) { return candidate.endpoint == endpoint; });
        const auto* addressBytes = static_cast<const std::byte*>(address);
        if (peer == controlPeers_.end()) {
            controlPeers_.push_back(ControlPeer{});
            peer = std::prev(controlPeers_.end());
            peer->endpoint = endpoint;
        }

        // Anti-replay. Within one control session (same sessionFingerprint) the
        // sequence must strictly increase, so a captured control datagram cannot
        // be replayed to re-inject input. A different sessionFingerprint marks a
        // new session (e.g. the viewer restarted) and rebaselines the counter.
        const bool newSession =
            !peer->hasControlSequence ||
            message->sessionFingerprint != peer->controlSessionFingerprint;
        if (newSession || message->sequence > peer->lastControlSequence) {
            peer->address.assign(addressBytes, addressBytes + addressLength);
            peer->addressLength = addressLength;
            peer->controlSessionFingerprint = message->sessionFingerprint;
            peer->lastControlSequence = message->sequence;
            peer->hasControlSequence = true;
            accepted = true;
        } else {
            ++stats_.controlReplayRejected;
        }
    }

    if (!accepted) {
        return; // stale or replayed control datagram
    }

    if (onControl_) {
        onControl_(endpoint, *message);
    }
}

bool UdpSender::SendControlTo(const std::string& endpoint, const udp_protocol::ControlMessage& message)
{
    if (!isOpen()) {
        return false;
    }

    std::vector<std::byte> targetAddress;
    int targetAddressLength = 0;
    udp_protocol::ControlMessage outgoing = message;
    {
        std::lock_guard lock(mutex_);
        auto peer = std::find_if(
            controlPeers_.begin(),
            controlPeers_.end(),
            [&](const ControlPeer& candidate) { return candidate.endpoint == endpoint; });
        if (peer == controlPeers_.end()) {
            return false;
        }
        targetAddress = peer->address;
        targetAddressLength = peer->addressLength;
        outgoing.sequence = nextControlSequence_++;
    }

    auto datagram = udp_protocol::BuildControlDatagram(outgoing);
    EncryptControlDatagram(datagram);
    const int sent = sendto(
        AsSocket(socket_),
        reinterpret_cast<const char*>(datagram.data()),
        static_cast<int>(datagram.size()),
        0,
        reinterpret_cast<const sockaddr*>(targetAddress.data()),
        targetAddressLength);
    return sent != SOCKET_ERROR && sent == static_cast<int>(datagram.size());
}

void UdpSender::WorkerLoop()
{
    for (;;) {
        std::vector<std::byte> datagram;
        {
            std::unique_lock lock(mutex_);
            queueChanged_.wait(lock, [&] {
                return stopWorker_ || !workerError_.empty() || !queue_.empty();
            });

            if ((stopWorker_ || !workerError_.empty()) && queue_.empty()) {
                break;
            }

            const auto now = Clock::now();
            const auto sendAt = queue_.front().sendAt;
            if (sendAt > now) {
                queueChanged_.wait_until(lock, sendAt, [&] {
                    return stopWorker_ || !workerError_.empty();
                });
                continue;
            }

            datagram = std::move(queue_.front().bytes);
            queue_.pop_front();
            datagramInFlight_ = true;
            UpdatePendingStatsLocked();
        }

        try {
            SendDatagramBytes(datagram);
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex_);
            workerError_ = error.what();
            queue_.clear();
            datagramInFlight_ = false;
            UpdatePendingStatsLocked();
            queueDrained_.notify_all();
            queueChanged_.notify_all();
            break;
        }

        {
            std::lock_guard lock(mutex_);
            datagramInFlight_ = false;
            UpdatePendingStatsLocked();
            if (queue_.empty()) {
                queueDrained_.notify_all();
            }
        }
    }
}

void UdpSender::SendDatagramBytes(const std::vector<std::byte>& datagram)
{
    std::shared_ptr<const std::vector<std::vector<std::byte>>> addresses;
    {
        std::lock_guard lock(mutex_);
        if (address_.empty() || addressLength_ <= 0) {
            throw std::runtime_error("UDP sender target address is not available");
        }
        if (sendAddressesDirty_ || !cachedSendAddresses_) {
            RebuildSendAddressesLocked();
        }
        addresses = cachedSendAddresses_;
    }

    if (!addresses || addresses->empty()) {
        throw std::runtime_error("UDP sender target address is not available");
    }

    uint64_t sentDatagrams = 0;
    uint64_t sentBytes = 0;
    for (const auto& address : *addresses) {
        const int sent = sendto(
            AsSocket(socket_),
            reinterpret_cast<const char*>(datagram.data()),
            static_cast<int>(datagram.size()),
            0,
            reinterpret_cast<const sockaddr*>(address.data()),
            static_cast<int>(address.size()));

        if (sent == SOCKET_ERROR) {
            throw std::runtime_error(WinsockErrorMessage("sendto"));
        }

        ++sentDatagrams;
        sentBytes += static_cast<uint64_t>(sent);
    }

    std::lock_guard lock(mutex_);
    stats_.datagramsSent += sentDatagrams;
    stats_.wireBytesSent += sentBytes;
}

void UdpSender::RebuildSendAddressesLocked()
{
    struct SendGroup {
        uint32_t group = 0;
        std::vector<std::vector<std::byte>> staticAddresses;
        std::vector<std::vector<std::byte>> natProbeAddresses;
    };

    std::vector<SendGroup> groups;
    auto groupFor = [&](uint32_t groupId) -> SendGroup& {
        auto existing = std::find_if(groups.begin(), groups.end(), [&](const SendGroup& group) {
            return group.group == groupId;
        });
        if (existing != groups.end()) {
            return *existing;
        }
        groups.push_back(SendGroup{groupId});
        return groups.back();
    };
    auto addUnique = [](std::vector<std::vector<std::byte>>& list, const std::vector<std::byte>& address) {
        if (std::find(list.begin(), list.end(), address) == list.end()) {
            list.push_back(address);
        }
    };

    std::vector<std::vector<std::byte>> addresses;
    groupFor(config_.group).staticAddresses.push_back(address_);
    for (const auto& address : additionalAddresses_) {
        addUnique(groupFor(address.group).staticAddresses, address.address);
    }
    for (const auto& address : natProbeAddresses_) {
        addUnique(groupFor(address.group).natProbeAddresses, address.address);
    }

    for (const auto& group : groups) {
        const bool useStaticTargets =
            !config_.preferNatProbeTargets || group.natProbeAddresses.empty();
        if (useStaticTargets) {
            for (const auto& address : group.staticAddresses) {
                addUnique(addresses, address);
            }
        }
        for (const auto& address : group.natProbeAddresses) {
            addUnique(addresses, address);
        }
    }

    cachedSendAddresses_ =
        std::make_shared<std::vector<std::vector<std::byte>>>(std::move(addresses));
    sendAddressesDirty_ = false;
}

void UdpSender::MaybeRetargetFromNatProbe(
    const void* address,
    int addressLength,
    const NatProbeDatagramInfo& probe)
{
    std::lock_guard lock(mutex_);
    ++stats_.natProbePacketsReceived;

    if (!config_.retargetOnNatProbe) {
        return;
    }
    if (config_.natProbeSessionFingerprint != 0 &&
        probe.sessionFingerprint != config_.natProbeSessionFingerprint) {
        ++stats_.natProbeRetargetRejected;
        return;
    }
    if (config_.accessCodeFingerprint != 0 &&
        probe.accessCodeFingerprint != config_.accessCodeFingerprint) {
        ++stats_.natProbeRetargetRejected;
        return;
    }
    if (address == nullptr || addressLength <= 0) {
        ++stats_.natProbeRetargetRejected;
        return;
    }

    std::vector<std::byte> candidate(static_cast<size_t>(addressLength));
    std::memcpy(candidate.data(), address, static_cast<size_t>(addressLength));
    if (config_.collectNatProbeTargets) {
        const uint32_t group = EndpointGroupForAddressLocked(candidate);
        const bool alreadyKnown = std::find_if(
            natProbeAddresses_.begin(),
            natProbeAddresses_.end(),
            [&](const GroupedAddress& existing) {
                return existing.address == candidate;
            }) != natProbeAddresses_.end();
        if (!alreadyKnown) {
            if (natProbeAddresses_.size() >= config_.maxNatProbeTargets) {
                ++stats_.natProbeRetargetRejected;
                return;
            }
            natProbeAddresses_.push_back(GroupedAddress{std::move(candidate), group});
            sendAddressesDirty_ = true;
            ++stats_.natProbeRetargets;
        }

        stats_.natProbeTargetCount = static_cast<uint64_t>(natProbeAddresses_.size());
        stats_.natProbeRetargetActive = true;
        stats_.natProbeRetargetEndpoint = SocketAddressToString(address, addressLength);
        return;
    }

    const bool changed = candidate != address_;
    if (changed) {
        address_ = std::move(candidate);
        addressLength_ = addressLength;
        sendAddressesDirty_ = true;
        ++stats_.natProbeRetargets;
    }

    stats_.natProbeRetargetActive = true;
    stats_.natProbeRetargetEndpoint = SocketAddressToString(address, addressLength);
}

uint32_t UdpSender::EndpointGroupForAddressLocked(const std::vector<std::byte>& address) const
{
    if (address == address_) {
        return config_.group;
    }
    for (const auto& candidate : additionalAddresses_) {
        if (candidate.address == address) {
            return candidate.group;
        }
    }
    for (const auto& candidate : natProbeAddresses_) {
        if (candidate.address == address) {
            return candidate.group;
        }
    }
    return 0;
}

UdpSender::Clock::duration UdpSender::PacingDelayForBytes(uint64_t wireBytes) const
{
    if (!config_.pacingEnabled || config_.pacingBitrate == 0) {
        return Clock::duration::zero();
    }

    const double seconds =
        static_cast<double>(wireBytes) * 8.0 / static_cast<double>(config_.pacingBitrate);
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

bool UdpSender::EnforceLiveQueueDelayLocked(Clock::time_point now)
{
    if (!config_.pacingEnabled ||
        config_.pacingBitrate == 0 ||
        config_.maxQueueDelay.count() == 0 ||
        queue_.empty()) {
        return false;
    }

    bool dropped = false;
    while (!queue_.empty() && nextSendAt_ > now && nextSendAt_ - now > config_.maxQueueDelay) {
        if (!DropOldestQueuedMediaLocked(PendingDatagramKind::Video) &&
            !DropOldestQueuedMediaLocked(PendingDatagramKind::Audio)) {
            break;
        }
        dropped = true;
        RescheduleQueueLocked(now);
    }

    if (dropped) {
        UpdatePendingStatsLocked();
    }
    return dropped;
}

bool UdpSender::DropOldestQueuedMediaLocked(PendingDatagramKind kind)
{
    const auto first = std::find_if(queue_.begin(), queue_.end(), [kind](const PendingDatagram& datagram) {
        return datagram.kind == kind;
    });
    if (first == queue_.end()) {
        return false;
    }

    const uint64_t mediaId = first->mediaId;
    size_t removed = 0;
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->kind == kind && it->mediaId == mediaId) {
            it = queue_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }

    if (kind == PendingDatagramKind::Video) {
        ++stats_.framesDropped;
    } else {
        ++stats_.audioPacketsDropped;
    }
    stats_.datagramsDropped += removed;
    return true;
}

void UdpSender::DropQueuedMediaForCapacityLocked(size_t incomingDatagrams, PendingDatagramKind preferredKind)
{
    if (incomingDatagrams > config_.maxQueuedDatagrams) {
        return;
    }

    bool dropped = false;
    while (queue_.size() + incomingDatagrams > config_.maxQueuedDatagrams) {
        const bool droppedPreferred = DropOldestQueuedMediaLocked(preferredKind);
        const bool droppedFallback =
            droppedPreferred ||
            DropOldestQueuedMediaLocked(
                preferredKind == PendingDatagramKind::Video ?
                    PendingDatagramKind::Audio :
                    PendingDatagramKind::Video);
        if (!droppedFallback) {
            break;
        }
        dropped = true;
    }

    if (dropped) {
        RescheduleQueueLocked(Clock::now());
    }
}

void UdpSender::RescheduleQueueLocked(Clock::time_point now)
{
    if (!config_.pacingEnabled || config_.pacingBitrate == 0) {
        for (auto& datagram : queue_) {
            datagram.sendAt = now;
        }
        nextSendAt_ = now;
        UpdatePendingStatsLocked();
        return;
    }

    auto sendAt = now;
    for (auto& datagram : queue_) {
        datagram.sendAt = sendAt;
        sendAt += PacingDelayForBytes(datagram.bytes.size());
    }
    nextSendAt_ = sendAt;
    UpdatePendingStatsLocked();
}

void UdpSender::CheckWorkerErrorLocked() const
{
    if (!workerError_.empty()) {
        throw std::runtime_error("UDP sender worker failed: " + workerError_);
    }
}

void UdpSender::UpdatePendingStatsLocked()
{
    stats_.pendingDatagrams =
        static_cast<uint64_t>(queue_.size()) + (datagramInFlight_ ? 1ULL : 0ULL);
    stats_.peakPendingDatagrams = std::max(stats_.peakPendingDatagrams, stats_.pendingDatagrams);
    uint64_t queueDelayMs = 0;
    const auto now = Clock::now();
    if (config_.pacingEnabled && !queue_.empty() && nextSendAt_ > now) {
        queueDelayMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(nextSendAt_ - now).count());
    }
    stats_.pendingQueueDelayMs = queueDelayMs;
    stats_.peakQueueDelayMs = std::max(stats_.peakQueueDelayMs, queueDelayMs);
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
