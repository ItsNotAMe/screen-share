#include "transport/UdpSender.h"

#include "transport/UdpProtocol.h"

#include <winsock2.h>
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

namespace screenshare {
namespace {

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
    if (config.maxQueuedDatagrams == 0) {
        throw std::invalid_argument("UDP max queued datagrams must be non-zero");
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

    {
        std::lock_guard lock(mutex_);
        address_.resize(static_cast<size_t>(resolved->ai_addrlen));
        std::memcpy(address_.data(), resolved->ai_addr, static_cast<size_t>(resolved->ai_addrlen));

        socket_ = static_cast<uintptr_t>(udpSocket);
        addressLength_ = static_cast<int>(resolved->ai_addrlen);
        config_ = config;
        stats_ = {};
        frameId_ = 0;
        audioPacketId_ = 0;
        queue_.clear();
        nextSendAt_ = Clock::now();
        workerError_.clear();
        stopWorker_ = false;
        datagramInFlight_ = false;
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
        addressLength_ = 0;
        workerError_.clear();
        stopWorker_ = false;
        datagramInFlight_ = false;
        UpdatePendingStatsLocked();
    }
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
        });
    }

    {
        std::lock_guard lock(mutex_);
        CheckWorkerErrorLocked();

        if (queue_.size() + datagrams.size() > config_.maxQueuedDatagrams) {
            ++stats_.framesDropped;
            stats_.datagramsDropped += datagrams.size();
            UpdatePendingStatsLocked();
            return;
        }

        const auto now = Clock::now();
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
    if (packet.bytes.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Audio packet is too large for UDP sender");
    }
    if (static_cast<uint64_t>(packet.audioFrames) * static_cast<uint64_t>(packet.blockAlign) != packet.bytes.size()) {
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
        });
    }

    {
        std::lock_guard lock(mutex_);
        CheckWorkerErrorLocked();

        if (queue_.size() + datagrams.size() > config_.maxQueuedDatagrams) {
            ++stats_.audioPacketsDropped;
            stats_.datagramsDropped += datagrams.size();
            UpdatePendingStatsLocked();
            return;
        }

        const auto now = Clock::now();
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
    std::lock_guard lock(mutex_);
    CheckWorkerErrorLocked();
    config_.pacingBitrate = bitrate;
    if (config_.pacingEnabled && nextSendAt_ < Clock::now()) {
        nextSendAt_ = Clock::now();
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
    snapshot.pendingDatagrams =
        static_cast<uint64_t>(queue_.size()) + (datagramInFlight_ ? 1ULL : 0ULL);
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

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(AsSocket(socket_), &readSet);

    timeval waitTime{};
    waitTime.tv_sec = static_cast<long>(timeout.count() / 1000);
    waitTime.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);

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

    const auto feedback = udp_protocol::ParseFeedbackDatagram(std::span<const std::byte>(
        buffer.data(),
        static_cast<size_t>(received)));

    std::lock_guard lock(mutex_);
    if (!feedback) {
        ++stats_.invalidFeedbackPackets;
        return std::nullopt;
    }

    ++stats_.feedbackPacketsReceived;
    stats_.hasFeedback = true;
    stats_.latestFeedback = *feedback;
    return feedback;
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
    header.frameBytes = udp_protocol::ToNetwork32(static_cast<uint32_t>(packet.bytes.size()));
    header.fragmentOffset = udp_protocol::ToNetwork32(fragmentOffset);
    header.fragmentIndex = udp_protocol::ToNetwork16(fragmentIndex);
    header.fragmentCount = udp_protocol::ToNetwork16(fragmentCount);
    header.payloadBytes = udp_protocol::ToNetwork32(payloadBytes);

    std::vector<std::byte> datagram(sizeof(udp_protocol::PacketHeader) + payloadBytes);
    std::memcpy(datagram.data(), &header, sizeof(header));
    std::memcpy(datagram.data() + sizeof(header), payload, payloadBytes);

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
    header.sampleRate = udp_protocol::ToNetwork32(packet.sampleRate);
    header.channels = udp_protocol::ToNetwork16(packet.channels);
    header.bitsPerSample = udp_protocol::ToNetwork16(packet.bitsPerSample);
    header.blockAlign = udp_protocol::ToNetwork16(packet.blockAlign);
    header.sampleFormat = udp_protocol::ToNetwork16(static_cast<uint16_t>(packet.sampleFormat));
    header.audioFrames = udp_protocol::ToNetwork32(packet.audioFrames);
    header.packetBytes = udp_protocol::ToNetwork32(static_cast<uint32_t>(packet.bytes.size()));
    header.fragmentOffset = udp_protocol::ToNetwork32(fragmentOffset);
    header.fragmentIndex = udp_protocol::ToNetwork16(fragmentIndex);
    header.fragmentCount = udp_protocol::ToNetwork16(fragmentCount);
    header.payloadBytes = udp_protocol::ToNetwork32(payloadBytes);
    header.flags = udp_protocol::ToNetwork32(packet.flags);

    std::vector<std::byte> datagram(sizeof(udp_protocol::AudioPacketHeader) + payloadBytes);
    std::memcpy(datagram.data(), &header, sizeof(header));
    std::memcpy(datagram.data() + sizeof(header), payload, payloadBytes);

    return datagram;
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

    std::lock_guard lock(mutex_);
    ++stats_.datagramsSent;
    stats_.wireBytesSent += static_cast<uint64_t>(sent);
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
