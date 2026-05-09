#include "transport/UdpReceiver.h"

#include "transport/UdpProtocol.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

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
    if (config.socketReceiveBufferBytes > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("UDP socket receive buffer is too large");
    }
    if (config.simulatedLossPercent < 0.0f || config.simulatedLossPercent > 100.0f) {
        throw std::invalid_argument("UDP simulated loss percent must be between 0 and 100");
    }
    if (config.simulatedJitter.count() < 0 || config.simulatedJitter > std::chrono::seconds(5)) {
        throw std::invalid_argument("UDP simulated jitter must be between 0 and 5000 ms");
    }

    const SOCKET udpSocket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET) {
        throw std::runtime_error(WinsockErrorMessage("socket"));
    }

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
        closesocket(udpSocket);
        throw std::runtime_error(WinsockErrorMessage("bind"));
    }

    socket_ = static_cast<uintptr_t>(udpSocket);
    config_ = config;
    datagramBuffer_.assign(config_.maxDatagramBytes, std::byte{});
    feedbackAddress_.clear();
    feedbackAddressLength_ = 0;
    delayedDatagrams_.clear();
    pendingFrames_.clear();
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
    feedbackAddress_.clear();
    feedbackAddressLength_ = 0;
    delayedDatagrams_.clear();
    pendingFrames_.clear();
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
        throw std::runtime_error(WinsockErrorMessage("recvfrom"));
    }

    feedbackAddress_.resize(static_cast<size_t>(senderAddressLength));
    std::memcpy(feedbackAddress_.data(), &senderAddress, static_cast<size_t>(senderAddressLength));
    feedbackAddressLength_ = senderAddressLength;

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

    const auto datagram = udp_protocol::BuildFeedbackDatagram(feedback);
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
    return true;
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
    const uint32_t frameBytes = udp_protocol::FromNetwork32(header.frameBytes);
    const uint32_t fragmentOffset = udp_protocol::FromNetwork32(header.fragmentOffset);
    const uint16_t fragmentIndex = udp_protocol::FromNetwork16(header.fragmentIndex);
    const uint16_t fragmentCount = udp_protocol::FromNetwork16(header.fragmentCount);
    const uint32_t payloadBytes = udp_protocol::FromNetwork32(header.payloadBytes);

    if (magic != udp_protocol::PacketMagic ||
        version != udp_protocol::PacketVersion ||
        headerBytes != sizeof(udp_protocol::PacketHeader) ||
        datagramBytes < static_cast<int>(headerBytes)) {
        ++stats_.invalidDatagrams;
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

    const auto now = Clock::now();
    auto [it, inserted] = pendingFrames_.try_emplace(frameId);
    PendingFrame& pending = it->second;
    if (inserted) {
        pending.frameId = frameId;
        pending.timestamp100ns = timestamp100ns;
        pending.frameBytes = frameBytes;
        pending.fragmentCount = fragmentCount;
        pending.bytes.assign(frameBytes, std::byte{});
        pending.fragmentReceived.assign(fragmentCount, 0);
    } else if (pending.timestamp100ns != timestamp100ns ||
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
    for (const auto& range : pending.receivedRanges) {
        if (fragmentOffset < range.end && fragmentEnd > range.begin) {
            ++stats_.invalidDatagrams;
            return std::nullopt;
        }
    }
    if (payloadBytes > pending.frameBytes - pending.receivedBytes) {
        ++stats_.invalidDatagrams;
        return std::nullopt;
    }

    ++stats_.datagramsAccepted;
    stats_.payloadBytesReceived += payloadBytes;

    std::memcpy(pending.bytes.data() + fragmentOffset, datagram + headerBytes, payloadBytes);
    pending.fragmentReceived[fragmentIndex] = 1;
    pending.receivedRanges.push_back({fragmentOffset, fragmentEnd});
    ++pending.receivedFragments;
    pending.receivedBytes += payloadBytes;

    if (pending.receivedFragments != pending.fragmentCount || pending.receivedBytes != pending.frameBytes) {
        EnforcePendingFrameLimit();
        return std::nullopt;
    }

    UdpCompletedFrame completed;
    completed.frameId = pending.frameId;
    completed.timestamp100ns = pending.timestamp100ns;
    completed.fragmentCount = pending.fragmentCount;
    completed.bytes = std::move(pending.bytes);

    pendingFrames_.erase(it);
    ++stats_.framesCompleted;
    stats_.completedFrameBytes += completed.bytes.size();
    return completed;
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
}

void UdpReceiver::EnforcePendingFrameLimit()
{
    while (pendingFrames_.size() > config_.maxPendingFrames) {
        auto oldest = pendingFrames_.begin();
        for (auto it = pendingFrames_.begin(); it != pendingFrames_.end(); ++it) {
            if (it->second.lastUpdated < oldest->second.lastUpdated) {
                oldest = it;
            }
        }

        pendingFrames_.erase(oldest);
        ++stats_.incompleteFramesDropped;
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
