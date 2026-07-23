#include "transport/UdpSender.h"

#include "transport/NatTraversal.h"
#include "transport/UdpProtocol.h"

#include <winsock2.h>
#include <mstcpip.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
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

    std::unique_ptr<UdpAesGcm> encryptCrypto;
    UdpCryptoKey master{};
    UdpCryptoSessionSalt sessionSalt{};
    const bool encryptionEnabled = config.encryptionKey.has_value();
    if (encryptionEnabled) {
        master = *config.encryptionKey;
        sessionSalt = GenerateUdpCryptoSessionSalt();
        encryptCrypto = std::make_unique<UdpAesGcm>(DeriveUdpSessionKey(master, sessionSalt));
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
        additionalLanes_.clear();
        natProbeAddresses_.clear();
        cachedSendAddresses_.reset();
        feedbackPeers_.clear();
        feedbackPeerReceivedAt_.clear();
        config_ = config;
        stats_ = {};
        stats_.encryptionEnabled = encryptionEnabled;
        frameId_ = 0;
        audioPacketId_ = 0;
        queue_.clear();
        encryptionEnabled_ = encryptionEnabled;
        master_ = master;
        sessionSalt_ = sessionSalt;
        encryptCrypto_ = std::move(encryptCrypto);
        peerDecryptCryptos_.clear();
        nextControlSequence_ = 1;
        controlPeers_.clear();
        nextSendAt_ = Clock::now();
        workerError_.clear();
        stopWorker_ = false;
        primarySuspended_ = false;
        primaryHostDisconnected_ = false;
        primarySuspendReason_.clear();
        datagramInFlight_ = false;
        sendAddressesDirty_ = true;
        for (const auto& target : additionalAddresses_) {
            // Multiple candidates for the primary viewer share the primary
            // queue. Only distinct viewer groups receive their own worker.
            if (target.group != config_.group) {
                StartAdditionalLaneLocked(target.group, target.address);
            }
        }
    }

    freeaddrinfo(resolved);
    worker_ = std::thread(&UdpSender::WorkerLoop, this);
}

void UdpSender::Close()
{
    StopAdditionalLanes();

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
        additionalLanes_.clear();
        cachedSendAddresses_.reset();
        feedbackPeers_.clear();
        feedbackPeerReceivedAt_.clear();
        addressLength_ = 0;
        encryptionEnabled_ = false;
        master_ = {};
        sessionSalt_ = {};
        encryptCrypto_.reset();
        peerDecryptCryptos_.clear();
        nextControlSequence_ = 1;
        controlPeers_.clear();
        workerError_.clear();
        stopWorker_ = false;
        primarySuspended_ = false;
        primaryHostDisconnected_ = false;
        primarySuspendReason_.clear();
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
    const bool alreadyKnown = address == address_ ||
        std::find_if(additionalAddresses_.begin(), additionalAddresses_.end(), matchesAddress) != additionalAddresses_.end() ||
        std::find_if(natProbeAddresses_.begin(), natProbeAddresses_.end(), matchesAddress) != natProbeAddresses_.end();
    if (target.group == config_.group) {
        if (!primaryHostDisconnected_) {
            primarySuspended_ = false;
            primarySuspendReason_.clear();
            queueChanged_.notify_all();
        }
    } else if (address != address_) {
        // Re-adding an already-known endpoint is also the recovery path for a
        // signaling peer that left and rejoined with the same candidate.
        StartAdditionalLaneLocked(target.group, address);
    }
    if (alreadyKnown) {
        return false;
    }

    additionalAddresses_.push_back(GroupedAddress{address, target.group});
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
            packet.isKeyframe,
        });
    }
    const std::vector<PendingDatagram> laneDatagrams = datagrams;

    bool primaryQueued = false;
    {
        std::lock_guard lock(mutex_);
        if (primarySuspended_ || !workerError_.empty()) {
            ++stats_.framesDropped;
            stats_.datagramsDropped += datagrams.size();
            UpdatePendingStatsLocked();
        } else {
            const auto now = Clock::now();
            static_cast<void>(EnforceLiveQueueDelayLocked(now));
            DropQueuedMediaForCapacityLocked(datagrams.size(), PendingDatagramKind::Video);

            if (queue_.size() + datagrams.size() > config_.maxQueuedDatagrams) {
                ++stats_.framesDropped;
                stats_.datagramsDropped += datagrams.size();
                UpdatePendingStatsLocked();
            } else {
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
                primaryQueued = true;
            }
        }
    }
    if (primaryQueued) {
        queueChanged_.notify_one();
    }
    QueueAdditionalDatagrams(laneDatagrams, PendingDatagramKind::Video, frameBytes);
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
            false,
        });
    }
    const std::vector<PendingDatagram> laneDatagrams = datagrams;

    bool primaryQueued = false;
    {
        std::lock_guard lock(mutex_);
        if (primarySuspended_ || !workerError_.empty()) {
            ++stats_.audioPacketsDropped;
            stats_.datagramsDropped += datagrams.size();
            UpdatePendingStatsLocked();
        } else {
            const auto now = Clock::now();
            static_cast<void>(EnforceLiveQueueDelayLocked(now));
            DropQueuedMediaForCapacityLocked(datagrams.size(), PendingDatagramKind::Video);

            if (queue_.size() + datagrams.size() > config_.maxQueuedDatagrams) {
                ++stats_.audioPacketsDropped;
                stats_.datagramsDropped += datagrams.size();
                UpdatePendingStatsLocked();
            } else {
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
                primaryQueued = true;
            }
        }
    }
    if (primaryQueued) {
        queueChanged_.notify_one();
    }
    QueueAdditionalDatagrams(laneDatagrams, PendingDatagramKind::Audio, packetBytes);
}

void UdpSender::SetPacingBitrate(uint32_t bitrate)
{
    bool notify = false;
    std::vector<AdditionalLane*> lanesToNotify;
    {
        std::lock_guard lock(mutex_);
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
        if (bitrateChanged) {
            for (const auto& lane : additionalLanes_) {
                std::lock_guard laneLock(lane->mutex);
                if (config_.pacingEnabled && !lane->queue.empty()) {
                    RescheduleAdditionalQueueLocked(*lane, now);
                    lanesToNotify.push_back(lane.get());
                } else if (config_.pacingEnabled && lane->nextSendAt < now) {
                    lane->nextSendAt = now;
                    UpdateAdditionalLaneStatsLocked(*lane, now);
                }
            }
        }
    }
    if (notify) {
        queueChanged_.notify_one();
    }
    for (AdditionalLane* lane : lanesToNotify) {
        lane->changed.notify_one();
    }
}

void UdpSender::SetPacingEnabled(bool enabled)
{
    bool notify = false;
    std::vector<AdditionalLane*> lanesToNotify;
    {
        std::lock_guard lock(mutex_);
        if (config_.pacingEnabled == enabled) {
            return;
        }
        config_.pacingEnabled = enabled;
        const auto now = Clock::now();
        if (!queue_.empty()) {
            // Disabling pacing releases held datagrams immediately. Enabling it
            // rebuilds the schedule from now instead of preserving a burst of
            // timestamps created while pacing was off.
            RescheduleQueueLocked(now);
            notify = true;
        } else {
            nextSendAt_ = now;
            UpdatePendingStatsLocked();
        }
        for (const auto& lane : additionalLanes_) {
            std::lock_guard laneLock(lane->mutex);
            if (!lane->queue.empty()) {
                RescheduleAdditionalQueueLocked(*lane, now);
                lanesToNotify.push_back(lane.get());
            } else {
                lane->nextSendAt = now;
                UpdateAdditionalLaneStatsLocked(*lane, now);
            }
        }
    }
    if (notify) {
        queueChanged_.notify_one();
    }
    for (AdditionalLane* lane : lanesToNotify) {
        lane->changed.notify_one();
    }
}

void UdpSender::SetMaxQueueDelay(std::chrono::milliseconds delay)
{
    if (delay < std::chrono::milliseconds(0)) {
        throw std::invalid_argument("UDP max queue delay must not be negative");
    }
    std::lock_guard lock(mutex_);
    config_.maxQueueDelay = delay;
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
    const auto now = Clock::now();
    for (size_t index = 0; index < snapshot.feedbackPeers.size() && index < feedbackPeerReceivedAt_.size(); ++index) {
        snapshot.feedbackPeers[index].feedbackAgeMs = static_cast<uint64_t>(std::max<int64_t>(
            0,
            std::chrono::duration_cast<std::chrono::milliseconds>(now - feedbackPeerReceivedAt_[index]).count()));
    }
    snapshot.natProbeTargetCount = static_cast<uint64_t>(natProbeAddresses_.size());
    snapshot.pendingDatagrams =
        static_cast<uint64_t>(queue_.size()) + (datagramInFlight_ ? 1ULL : 0ULL);
    if (config_.pacingEnabled && !queue_.empty() && nextSendAt_ > now) {
        snapshot.pendingQueueDelayMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(nextSendAt_ - now).count());
    } else {
        snapshot.pendingQueueDelayMs = 0;
    }
    UdpSenderStats::ViewerLane primary;
    primary.group = config_.group;
    primary.endpoint = address_.empty() ? std::string() : SocketAddressToString(address_.data(), addressLength_);
    primary.datagramsSent = stats_.datagramsSent;
    primary.wireBytesSent = stats_.wireBytesSent;
    primary.datagramsDropped = stats_.datagramsDropped;
    primary.pendingDatagrams = snapshot.pendingDatagrams;
    primary.peakPendingDatagrams = stats_.peakPendingDatagrams;
    primary.pendingQueueDelayMs = snapshot.pendingQueueDelayMs;
    primary.peakQueueDelayMs = stats_.peakQueueDelayMs;
    primary.failed = primarySuspended_ || !workerError_.empty();
    primary.error = !primarySuspendReason_.empty() ? primarySuspendReason_ : workerError_;
    snapshot.viewerLanes.push_back(std::move(primary));
    for (const auto& lane : additionalLanes_) {
        std::lock_guard laneLock(lane->mutex);
        auto laneStats = lane->stats;
        const_cast<UdpSender*>(this)->UpdateAdditionalLaneStatsLocked(*lane, now);
        laneStats = lane->stats;
        snapshot.datagramsSent += laneStats.datagramsSent;
        snapshot.wireBytesSent += laneStats.wireBytesSent;
        snapshot.datagramsDropped += laneStats.datagramsDropped;
        snapshot.pendingDatagrams += laneStats.pendingDatagrams;
        snapshot.peakPendingDatagrams += laneStats.peakPendingDatagrams;
        snapshot.pendingQueueDelayMs = std::max(snapshot.pendingQueueDelayMs, laneStats.pendingQueueDelayMs);
        snapshot.peakQueueDelayMs = std::max(snapshot.peakQueueDelayMs, laneStats.peakQueueDelayMs);
        snapshot.viewerLanes.push_back(std::move(laneStats));
    }
    return snapshot;
}

void UdpSender::Flush()
{
    std::vector<AdditionalLane*> lanes;
    {
        std::unique_lock lock(mutex_);
        queueDrained_.wait(lock, [&] {
            return !workerError_.empty() || (queue_.empty() && !datagramInFlight_);
        });
        for (const auto& lane : additionalLanes_) {
            lanes.push_back(lane.get());
        }
        if (!workerError_.empty() && lanes.empty() && !primarySuspended_) {
            CheckWorkerErrorLocked();
        }
    }
    for (AdditionalLane* lane : lanes) {
        std::unique_lock laneLock(lane->mutex);
        lane->drained.wait(laneLock, [&] {
            return lane->stop || lane->suspended || (lane->queue.empty() && !lane->inFlight);
        });
    }
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
                if (!encryptionEnabled_) {
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
            } else if (encryptionEnabled_) {
                cryptoRejected = true;
            }
        } else if (encryptionEnabled_) {
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
        uint32_t feedbackGroup = EndpointGroupForAddressLocked(feedbackAddress);
        if (feedbackGroup == 0 && feedback->sessionFingerprint != 0) {
            const auto previousSession = std::find_if(
                feedbackPeers_.begin(),
                feedbackPeers_.end(),
                [&](const UdpSenderStats::FeedbackPeer& candidate) {
                    return candidate.latestFeedback.sessionFingerprint == feedback->sessionFingerprint && candidate.group != 0;
                });
            if (previousSession != feedbackPeers_.end()) {
                feedbackGroup = previousSession->group;
            }
        }
        const std::string feedbackEndpoint = SocketAddressToString(&senderAddress, senderAddressLength);
        if (feedbackGroup == config_.group && primarySuspended_ && !primaryHostDisconnected_) {
            primarySuspended_ = false;
            primarySuspendReason_.clear();
            queueChanged_.notify_all();
        }
        if (feedbackGroup != 0 && feedbackGroup != config_.group) {
            auto lane = std::find_if(additionalLanes_.begin(), additionalLanes_.end(), [&](const auto& candidate) {
                return candidate->group == feedbackGroup;
            });
            if (lane != additionalLanes_.end()) {
                std::lock_guard laneLock((*lane)->mutex);
                if (std::find((*lane)->natProbeAddresses.begin(), (*lane)->natProbeAddresses.end(), feedbackAddress) ==
                    (*lane)->natProbeAddresses.end()) {
                    (*lane)->natProbeAddresses.push_back(feedbackAddress);
                }
                if (!(*lane)->hostDisconnected) {
                    (*lane)->suspended = false;
                    (*lane)->consecutiveErrors = 0;
                    (*lane)->stats.failed = false;
                    (*lane)->stats.error.clear();
                }
                (*lane)->stats.endpoint = feedbackEndpoint;
                (*lane)->changed.notify_all();
            }
        }

        // Feedback and control arrive from the same viewer socket, so record this
        // endpoint as a control-ack target now. That lets the host send a grant
        // (SendControlTo) immediately when it toggles a permission, without
        // waiting for the viewer to first send a "request control" packet.
        static_cast<void>(UpsertControlPeerLocked(
            feedbackEndpoint,
            &senderAddress,
            senderAddressLength));

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
                0,
                *feedback,
            });
            feedbackPeerReceivedAt_.push_back(Clock::now());
        } else {
            const size_t peerIndex = static_cast<size_t>(std::distance(feedbackPeers_.begin(), peer));
            peer->group = feedbackGroup;
            ++peer->packetsReceived;
            peer->latestFeedback = *feedback;
            if (peerIndex < feedbackPeerReceivedAt_.size()) {
                feedbackPeerReceivedAt_[peerIndex] = Clock::now();
            }
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
    if (encryptionEnabled_) {
        header.flags = udp_protocol::ToNetwork32(udp_protocol::PacketFlagEncrypted);
        WriteUdpCryptoNonce(
            std::span<std::byte, UdpCryptoNonceBytes>(header.encryptionNonce, UdpCryptoNonceBytes),
            UdpCryptoRole::Video,
            frameId,
            fragmentIndex);
        std::memcpy(header.sessionSalt, sessionSalt_.data(), sizeof(header.sessionSalt));
    }

    std::vector<std::byte> datagram(sizeof(udp_protocol::PacketHeader) + payloadBytes);
    std::memcpy(datagram.data(), &header, sizeof(header));
    std::memcpy(datagram.data() + sizeof(header), payload, payloadBytes);
    if (encryptionEnabled_) {
        EncryptDatagramPayload(
            *encryptCrypto_,
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
    if (encryptionEnabled_) {
        header.encryptionFlags = udp_protocol::ToNetwork32(udp_protocol::PacketFlagEncrypted);
        WriteUdpCryptoNonce(
            std::span<std::byte, UdpCryptoNonceBytes>(header.encryptionNonce, UdpCryptoNonceBytes),
            UdpCryptoRole::Audio,
            packetId,
            fragmentIndex);
        std::memcpy(header.sessionSalt, sessionSalt_.data(), sizeof(header.sessionSalt));
    }

    std::vector<std::byte> datagram(sizeof(udp_protocol::AudioPacketHeader) + payloadBytes);
    std::memcpy(datagram.data(), &header, sizeof(header));
    std::memcpy(datagram.data() + sizeof(header), payload, payloadBytes);
    if (encryptionEnabled_) {
        EncryptDatagramPayload(
            *encryptCrypto_,
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
    const UdpAesGcm& crypto,
    std::vector<std::byte>& datagram,
    size_t headerBytes,
    size_t authenticatedHeaderBytes,
    std::span<const std::byte, UdpCryptoNonceBytes> nonce,
    std::span<std::byte, UdpCryptoTagBytes> tag)
{
    const auto ciphertext = crypto.Encrypt(
        nonce,
        std::span<const std::byte>(datagram.data(), authenticatedHeaderBytes),
        std::span<const std::byte>(datagram.data() + headerBytes, datagram.size() - headerBytes),
        tag);
    std::memcpy(datagram.data() + headerBytes, ciphertext.data(), ciphertext.size());
}

UdpAesGcm* UdpSender::DecryptCryptoForSalt(const UdpCryptoSessionSalt& salt)
{
    // Worker-thread only (feedback + control both decrypt on the receive loop),
    // so peerDecryptCryptos_ needs no extra locking.
    if (!encryptionEnabled_) {
        return nullptr;
    }
    for (auto& entry : peerDecryptCryptos_) {
        if (entry.salt == salt) {
            return entry.crypto.get();
        }
    }
    // Bound the cache so a peer that floods distinct (unauthenticated) salts
    // cannot grow it without limit; a wrong salt just yields a key whose GCM
    // tag check fails, so eviction is harmless.
    constexpr size_t MaxPeerDecryptCryptos = 64;
    if (peerDecryptCryptos_.size() >= MaxPeerDecryptCryptos) {
        peerDecryptCryptos_.erase(peerDecryptCryptos_.begin());
    }
    PeerDecryptCrypto added;
    added.salt = salt;
    added.crypto = std::make_unique<UdpAesGcm>(DeriveUdpSessionKey(master_, salt));
    peerDecryptCryptos_.push_back(std::move(added));
    return peerDecryptCryptos_.back().crypto.get();
}

std::optional<std::vector<std::byte>> UdpSender::DecryptFeedbackDatagram(std::span<const std::byte> datagram)
{
    if (!encryptionEnabled_ || datagram.size() != sizeof(udp_protocol::FeedbackPacket)) {
        return std::nullopt;
    }

    udp_protocol::FeedbackPacket packet{};
    std::memcpy(&packet, datagram.data(), sizeof(packet));
    if ((udp_protocol::FromNetwork32(packet.flags) & udp_protocol::PacketFlagEncrypted) == 0) {
        return std::vector<std::byte>(datagram.begin(), datagram.end());
    }

    UdpCryptoSessionSalt salt{};
    std::memcpy(salt.data(), packet.sessionSalt, salt.size());
    UdpAesGcm* crypto = DecryptCryptoForSalt(salt);
    if (crypto == nullptr) {
        return std::nullopt;
    }

    auto plaintext = crypto->Decrypt(
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

    EncryptDatagramPayload(
        *encryptCrypto_,
        datagram,
        udp_protocol::ControlPacketEncryptedPayloadOffset,
        udp_protocol::ControlPacketAuthenticatedBytes,
        std::span<const std::byte, UdpCryptoNonceBytes>(packet.encryptionNonce, UdpCryptoNonceBytes),
        std::span<std::byte, UdpCryptoTagBytes>(
            datagram.data() + offsetof(udp_protocol::ControlPacket, encryptionTag),
            UdpCryptoTagBytes));
    return true;
}

UdpSender::ControlPeer& UdpSender::UpsertControlPeerLocked(
    const std::string& endpoint,
    const void* address,
    int addressLength)
{
    auto peer = std::find_if(
        controlPeers_.begin(),
        controlPeers_.end(),
        [&](const ControlPeer& candidate) { return candidate.endpoint == endpoint; });
    if (peer == controlPeers_.end()) {
        controlPeers_.push_back(ControlPeer{});
        peer = std::prev(controlPeers_.end());
        peer->endpoint = endpoint;
    }
    const auto* addressBytes = static_cast<const std::byte*>(address);
    peer->address.assign(addressBytes, addressBytes + addressLength);
    peer->addressLength = addressLength;
    return *peer;
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
        if (!encryptionEnabled_) {
            return;
        }
        UdpCryptoSessionSalt salt{};
        std::memcpy(salt.data(), header.sessionSalt, salt.size());
        UdpAesGcm* crypto = DecryptCryptoForSalt(salt);
        if (crypto == nullptr) {
            return;
        }
        auto plaintext = crypto->Decrypt(
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
    } else if (encryptionEnabled_) {
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
        const auto peer = std::find_if(
            controlPeers_.begin(),
            controlPeers_.end(),
            [&](const ControlPeer& candidate) { return candidate.endpoint == endpoint; });

        // Anti-replay. Within one control session (same sessionFingerprint) the
        // sequence must strictly increase, so a captured control datagram cannot
        // be replayed to re-inject input. A different sessionFingerprint marks a
        // new session (e.g. the viewer restarted) and rebaselines the counter.
        const bool newSession =
            peer == controlPeers_.end() ||
            !peer->hasControlSequence ||
            message->sessionFingerprint != peer->controlSessionFingerprint;
        if (newSession || message->sequence > peer->lastControlSequence) {
            ControlPeer& acceptedPeer = UpsertControlPeerLocked(endpoint, address, addressLength);
            acceptedPeer.controlSessionFingerprint = message->sessionFingerprint;
            acceptedPeer.lastControlSequence = message->sequence;
            acceptedPeer.hasControlSequence = true;
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

void UdpSender::StartAdditionalLaneLocked(uint32_t group, const std::vector<std::byte>& address)
{
    auto existing = std::find_if(additionalLanes_.begin(), additionalLanes_.end(), [group](const auto& lane) {
        return lane->group == group;
    });
    if (existing != additionalLanes_.end()) {
        auto& lane = **existing;
        std::lock_guard laneLock(lane.mutex);
        if (std::find(lane.staticAddresses.begin(), lane.staticAddresses.end(), address) == lane.staticAddresses.end()) {
            lane.staticAddresses.push_back(address);
        }
        if (!lane.hostDisconnected) {
            lane.suspended = false;
            lane.stats.failed = false;
            lane.stats.error.clear();
        }
        lane.changed.notify_all();
        return;
    }

    auto lane = std::make_unique<AdditionalLane>();
    lane->group = group;
    lane->staticAddresses.push_back(address);
    lane->nextSendAt = Clock::now();
    lane->stats.group = group;
    lane->stats.endpoint = SocketAddressToString(address.data(), static_cast<int>(address.size()));
    AdditionalLane* rawLane = lane.get();
    additionalLanes_.push_back(std::move(lane));
    rawLane->worker = std::thread(&UdpSender::AdditionalWorkerLoop, this, rawLane);
}

void UdpSender::StopAdditionalLanes()
{
    std::vector<std::unique_ptr<AdditionalLane>> lanes;
    {
        std::lock_guard lock(mutex_);
        lanes.swap(additionalLanes_);
    }
    for (auto& lane : lanes) {
        {
            std::lock_guard laneLock(lane->mutex);
            lane->stop = true;
            lane->queue.clear();
            UpdateAdditionalLaneStatsLocked(*lane, Clock::now());
        }
        lane->changed.notify_all();
        lane->drained.notify_all();
    }
    for (auto& lane : lanes) {
        if (lane->worker.joinable()) {
            lane->worker.join();
        }
    }
}

void UdpSender::UpdateAdditionalLaneStatsLocked(AdditionalLane& lane, Clock::time_point now)
{
    lane.stats.pendingDatagrams = static_cast<uint64_t>(lane.queue.size()) + (lane.inFlight ? 1ULL : 0ULL);
    lane.stats.peakPendingDatagrams = std::max(lane.stats.peakPendingDatagrams, lane.stats.pendingDatagrams);
    if (!lane.queue.empty() && lane.nextSendAt > now) {
        lane.stats.pendingQueueDelayMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(lane.nextSendAt - now).count());
    } else {
        lane.stats.pendingQueueDelayMs = 0;
    }
    lane.stats.peakQueueDelayMs = std::max(lane.stats.peakQueueDelayMs, lane.stats.pendingQueueDelayMs);
}

void UdpSender::RescheduleAdditionalQueueLocked(AdditionalLane& lane, Clock::time_point now)
{
    if (!config_.pacingEnabled || config_.pacingBitrate == 0) {
        for (auto& datagram : lane.queue) {
            datagram.sendAt = now;
        }
        lane.nextSendAt = now;
        UpdateAdditionalLaneStatsLocked(lane, now);
        return;
    }

    auto sendAt = now;
    for (auto& datagram : lane.queue) {
        datagram.sendAt = sendAt;
        sendAt += PacingDelayForBytes(datagram.bytes.size());
    }
    lane.nextSendAt = sendAt;
    UpdateAdditionalLaneStatsLocked(lane, now);
}

void UdpSender::QueueAdditionalDatagrams(
    const std::vector<PendingDatagram>& datagrams,
    PendingDatagramKind kind,
    uint64_t)
{
    std::lock_guard senderLock(mutex_);
    const auto now = Clock::now();
    for (auto& lanePointer : additionalLanes_) {
        AdditionalLane& lane = *lanePointer;
        std::lock_guard laneLock(lane.mutex);
        if (lane.stop || lane.suspended) {
            continue;
        }

        auto dropOldestMedia = [&](PendingDatagramKind preferred) {
            auto first = std::find_if(lane.queue.begin(), lane.queue.end(), [preferred](const PendingDatagram& queued) {
                return queued.kind == preferred;
            });
            if (first == lane.queue.end()) {
                return false;
            }
            const uint64_t mediaId = first->mediaId;
            size_t removed = 0;
            for (auto iterator = lane.queue.begin(); iterator != lane.queue.end();) {
                if (iterator->kind == preferred && iterator->mediaId == mediaId) {
                    iterator = lane.queue.erase(iterator);
                    ++removed;
                } else {
                    ++iterator;
                }
            }
            lane.stats.datagramsDropped += removed;
            if (preferred == PendingDatagramKind::Video) {
                lane.awaitingKeyframe = true;
            }
            return true;
        };

        if (kind == PendingDatagramKind::Video && !datagrams.empty()) {
            if (lane.awaitingKeyframe && !datagrams.front().keyframe) {
                lane.stats.datagramsDropped += datagrams.size();
                UpdateAdditionalLaneStatsLocked(lane, now);
                continue;
            }
            if (lane.awaitingKeyframe && datagrams.front().keyframe) {
                for (auto iterator = lane.queue.begin(); iterator != lane.queue.end();) {
                    if (iterator->kind == PendingDatagramKind::Video) {
                        iterator = lane.queue.erase(iterator);
                        ++lane.stats.datagramsDropped;
                    } else {
                        ++iterator;
                    }
                }
                lane.awaitingKeyframe = false;
            }
        }

        while (!lane.queue.empty() && config_.maxQueueDelay.count() > 0 &&
               lane.nextSendAt > now && lane.nextSendAt - now > config_.maxQueueDelay) {
            if (!dropOldestMedia(PendingDatagramKind::Video) && !dropOldestMedia(PendingDatagramKind::Audio)) {
                break;
            }
            lane.nextSendAt = now;
            for (auto& queued : lane.queue) {
                queued.sendAt = lane.nextSendAt;
                if (config_.pacingEnabled) {
                    lane.nextSendAt += PacingDelayForBytes(queued.bytes.size());
                }
            }
        }
        while (lane.queue.size() + datagrams.size() > config_.maxQueuedDatagrams) {
            if (!dropOldestMedia(kind) && !dropOldestMedia(
                    kind == PendingDatagramKind::Video ? PendingDatagramKind::Audio : PendingDatagramKind::Video)) {
                break;
            }
        }
        if (lane.queue.size() + datagrams.size() > config_.maxQueuedDatagrams) {
            lane.stats.datagramsDropped += datagrams.size();
            UpdateAdditionalLaneStatsLocked(lane, now);
            continue;
        }

        if (!config_.pacingEnabled || config_.pacingBitrate == 0 || lane.nextSendAt < now) {
            lane.nextSendAt = now;
        }
        for (const auto& source : datagrams) {
            PendingDatagram queued = source;
            queued.sendAt = config_.pacingEnabled ? lane.nextSendAt : now;
            if (config_.pacingEnabled) {
                lane.nextSendAt += PacingDelayForBytes(queued.bytes.size());
            }
            lane.queue.push_back(std::move(queued));
        }
        UpdateAdditionalLaneStatsLocked(lane, now);
        lane.changed.notify_one();
    }
}

void UdpSender::AdditionalWorkerLoop(AdditionalLane* lane)
{
    for (;;) {
        std::vector<std::byte> datagram;
        {
            std::unique_lock lock(lane->mutex);
            lane->changed.wait(lock, [&] { return lane->stop || !lane->queue.empty(); });
            if (lane->stop && lane->queue.empty()) {
                break;
            }
            const auto now = Clock::now();
            if (lane->queue.front().sendAt > now) {
                // Re-evaluate the front deadline after every notification. A
                // runtime Low Latency change can move it from the future to now.
                lane->changed.wait_until(lock, lane->queue.front().sendAt);
                continue;
            }
            datagram = std::move(lane->queue.front().bytes);
            lane->queue.pop_front();
            lane->inFlight = true;
            UpdateAdditionalLaneStatsLocked(*lane, now);
        }

        static_cast<void>(SendAdditionalDatagramBytes(*lane, datagram));

        {
            std::lock_guard lock(lane->mutex);
            lane->inFlight = false;
            UpdateAdditionalLaneStatsLocked(*lane, Clock::now());
            if (lane->queue.empty()) {
                lane->drained.notify_all();
            }
        }
    }
}

bool UdpSender::SendAdditionalDatagramBytes(AdditionalLane& lane, const std::vector<std::byte>& datagram)
{
    std::vector<std::vector<std::byte>> addresses;
    {
        std::lock_guard lock(lane.mutex);
        const bool useStatic = !config_.preferNatProbeTargets || lane.natProbeAddresses.empty();
        if (useStatic) {
            addresses = lane.staticAddresses;
        }
        for (const auto& address : lane.natProbeAddresses) {
            if (std::find(addresses.begin(), addresses.end(), address) == addresses.end()) {
                addresses.push_back(address);
            }
        }
    }

    uint64_t sentCount = 0;
    uint64_t sentBytes = 0;
    std::string lastError;
    for (const auto& target : addresses) {
        const int sent = sendto(
            AsSocket(socket_),
            reinterpret_cast<const char*>(datagram.data()),
            static_cast<int>(datagram.size()),
            0,
            reinterpret_cast<const sockaddr*>(target.data()),
            static_cast<int>(target.size()));
        if (sent == SOCKET_ERROR) {
            lastError = WinsockErrorMessage("sendto(viewer lane)");
            continue;
        }
        ++sentCount;
        sentBytes += static_cast<uint64_t>(sent);
    }

    std::lock_guard lock(lane.mutex);
    if (sentCount == 0) {
        ++lane.stats.socketErrors;
        ++lane.consecutiveErrors;
        lane.stats.error = lastError.empty() ? "viewer lane has no usable endpoint" : lastError;
        if (lane.consecutiveErrors >= 5) {
            lane.suspended = true;
            lane.stats.failed = true;
            lane.queue.clear();
        }
        return false;
    }
    lane.consecutiveErrors = 0;
    lane.stats.failed = false;
    lane.stats.error.clear();
    lane.stats.datagramsSent += sentCount;
    lane.stats.wireBytesSent += sentBytes;
    return true;
}

bool UdpSender::SuspendTargetGroup(uint32_t group, std::string reason, bool permanent)
{
    std::lock_guard lock(mutex_);
    for (size_t index = 0; index < feedbackPeers_.size();) {
        if (feedbackPeers_[index].group == group) {
            feedbackPeers_.erase(feedbackPeers_.begin() + static_cast<std::ptrdiff_t>(index));
            if (index < feedbackPeerReceivedAt_.size()) {
                feedbackPeerReceivedAt_.erase(
                    feedbackPeerReceivedAt_.begin() + static_cast<std::ptrdiff_t>(index));
            }
        } else {
            ++index;
        }
    }
    if (group == config_.group) {
        primarySuspended_ = true;
        primaryHostDisconnected_ = permanent;
        queue_.clear();
        primarySuspendReason_ = reason.empty() ? "disconnected" : std::move(reason);
        UpdatePendingStatsLocked();
        queueChanged_.notify_all();
        queueDrained_.notify_all();
        return true;
    }
    const auto lane = std::find_if(additionalLanes_.begin(), additionalLanes_.end(), [group](const auto& item) {
        return item->group == group;
    });
    if (lane == additionalLanes_.end()) {
        return false;
    }
    std::lock_guard laneLock((*lane)->mutex);
    (*lane)->hostDisconnected = permanent;
    (*lane)->suspended = true;
    (*lane)->queue.clear();
    (*lane)->stats.failed = true;
    (*lane)->stats.error = reason.empty() ? "host_disconnected" : std::move(reason);
    UpdateAdditionalLaneStatsLocked(**lane, Clock::now());
    (*lane)->changed.notify_all();
    (*lane)->drained.notify_all();
    return true;
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
                // Re-evaluate the front deadline after every notification. A
                // runtime Low Latency change can move it from the future to now.
                queueChanged_.wait_until(lock, sendAt);
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
    auto addUnique = [](std::vector<std::vector<std::byte>>& list, const std::vector<std::byte>& address) {
        if (std::find(list.begin(), list.end(), address) == list.end()) {
            list.push_back(address);
        }
    };

    std::vector<std::vector<std::byte>> addresses;
    std::vector<std::vector<std::byte>> primaryNatAddresses;
    for (const auto& address : natProbeAddresses_) {
        if (address.group == config_.group) {
            addUnique(primaryNatAddresses, address.address);
        }
    }
    if (!config_.preferNatProbeTargets || primaryNatAddresses.empty()) {
        addUnique(addresses, address_);
        for (const auto& candidate : additionalAddresses_) {
            if (candidate.group == config_.group) {
                addUnique(addresses, candidate.address);
            }
        }
    }
    for (const auto& address : primaryNatAddresses) {
        addUnique(addresses, address);
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

    // When the session is encrypted, only retarget to an endpoint that has
    // already proven possession of the session key by delivering a
    // validly-decrypting feedback/control packet. The NAT probe itself is
    // unauthenticated (its fingerprints are observable on the wire), so without
    // this gate an attacker spoofing a probe from an arbitrary source address
    // could redirect the encrypted media stream to that source (hijack / DoS).
    // A genuine NAT rebind re-registers the receiver's new address via its
    // decrypting feedback before the matching probe arrives, so this only adds a
    // brief, self-healing delay for legitimate peers. Plaintext sessions have no
    // key to prove and keep the existing fingerprint-only behavior.
    if (encryptionEnabled_ && !IsVerifiedEndpointLocked(candidate)) {
        ++stats_.natProbeRetargetRejected;
        return;
    }

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
            if (group != 0 && group != config_.group) {
                const auto lane = std::find_if(additionalLanes_.begin(), additionalLanes_.end(), [group](const auto& item) {
                    return item->group == group;
                });
                if (lane != additionalLanes_.end()) {
                    std::lock_guard laneLock((*lane)->mutex);
                    const auto& laneAddress = natProbeAddresses_.back().address;
                    if (std::find((*lane)->natProbeAddresses.begin(), (*lane)->natProbeAddresses.end(), laneAddress) ==
                        (*lane)->natProbeAddresses.end()) {
                        (*lane)->natProbeAddresses.push_back(laneAddress);
                    }
                    if (!(*lane)->hostDisconnected) {
                        (*lane)->suspended = false;
                        (*lane)->stats.failed = false;
                        (*lane)->stats.error.clear();
                    }
                    (*lane)->changed.notify_all();
                }
            }
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

bool UdpSender::IsVerifiedEndpointLocked(const std::vector<std::byte>& address) const
{
    // controlPeers_ holds the raw address of every peer that has delivered a
    // feedback/control packet. On an encrypted session those entries are only
    // created after a successful AES-GCM decrypt, so membership here proves the
    // endpoint possesses the session key. The address is also the current send
    // target itself (already trusted) — allow that so a no-op probe is accepted.
    if (!address.empty() && address == address_) {
        return true;
    }
    for (const auto& peer : controlPeers_) {
        if (!peer.address.empty() && peer.address == address) {
            return true;
        }
    }
    return false;
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
