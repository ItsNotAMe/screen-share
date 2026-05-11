#pragma once

#include "transport/UdpProtocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

namespace screenshare {

struct UdpReceiverConfig {
    uint16_t port = 0;
    uint32_t maxDatagramBytes = 65'507;
    uint32_t maxFrameBytes = 64 * 1024 * 1024;
    uint32_t socketReceiveBufferBytes = 4 * 1024 * 1024;
    size_t maxPendingFrames = 256;
    size_t maxPendingAudioPackets = 512;
    uint32_t maxAudioPacketBytes = 2 * 1024 * 1024;
    std::chrono::milliseconds frameTimeout = std::chrono::seconds(5);
    float simulatedLossPercent = 0.0f;
    std::chrono::milliseconds simulatedJitter = std::chrono::milliseconds(0);
    uint32_t simulationSeed = 1;
};

struct UdpCompletedFrame {
    uint64_t frameId = 0;
    uint64_t timestamp100ns = 0;
    uint16_t fragmentCount = 0;
    std::vector<std::byte> bytes;
};

struct UdpReceiverStats {
    uint64_t datagramsReceived = 0;
    uint64_t datagramsAccepted = 0;
    uint64_t invalidDatagrams = 0;
    uint64_t duplicateFragments = 0;
    uint64_t framesCompleted = 0;
    uint64_t incompleteFramesDropped = 0;
    uint64_t payloadBytesReceived = 0;
    uint64_t completedFrameBytes = 0;
    uint64_t simulatedDatagramsDropped = 0;
    uint64_t simulatedDatagramsDelayed = 0;
    uint64_t feedbackPacketsSent = 0;
    uint64_t feedbackSendErrors = 0;
    uint64_t audioDatagramsAccepted = 0;
    uint64_t audioDuplicateFragments = 0;
    uint64_t audioPacketsCompleted = 0;
    uint64_t audioIncompletePacketsDropped = 0;
    uint64_t audioPayloadBytesReceived = 0;
    uint64_t audioCompletedPacketBytes = 0;
    uint64_t audioFramesCompleted = 0;
    uint64_t audioSilentPackets = 0;
    uint64_t audioDiscontinuities = 0;
    uint64_t audioTimestampErrors = 0;
    uint64_t audioFormatChanges = 0;
    uint64_t latestAudioPacketId = 0;
    uint64_t latestAudioDevicePosition = 0;
    uint64_t latestAudioQpcPosition = 0;
    uint32_t audioSampleRate = 0;
    uint16_t audioChannels = 0;
    uint16_t audioBitsPerSample = 0;
    uint16_t audioBlockAlign = 0;
    udp_protocol::AudioSampleFormat audioSampleFormat = udp_protocol::AudioSampleFormat::Unknown;
};

class UdpReceiver {
public:
    UdpReceiver();
    ~UdpReceiver();

    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    void Open(const UdpReceiverConfig& config);
    void Close();
    [[nodiscard]] std::optional<UdpCompletedFrame> ReceiveFrame(std::chrono::milliseconds timeout);
    bool SendFeedback(const udp_protocol::FeedbackSnapshot& feedback);

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] const UdpReceiverStats& stats() const noexcept { return stats_; }
    [[nodiscard]] size_t pendingFrameCount() const noexcept { return pendingFrames_.size(); }
    [[nodiscard]] size_t pendingAudioPacketCount() const noexcept { return pendingAudioPackets_.size(); }
    [[nodiscard]] size_t delayedDatagramCount() const noexcept { return delayedDatagrams_.size(); }

private:
    using Clock = std::chrono::steady_clock;

    struct DelayedDatagram {
        Clock::time_point releaseAt;
        std::vector<std::byte> bytes;
    };

    struct PendingFrame {
        struct FragmentRange {
            uint32_t begin = 0;
            uint32_t end = 0;
        };

        uint64_t frameId = 0;
        uint64_t timestamp100ns = 0;
        uint32_t frameBytes = 0;
        uint16_t fragmentCount = 0;
        uint16_t receivedFragments = 0;
        uint32_t receivedBytes = 0;
        std::vector<std::byte> bytes;
        std::vector<uint8_t> fragmentReceived;
        std::vector<FragmentRange> receivedRanges;
        Clock::time_point lastUpdated;
    };

    struct PendingAudioPacket {
        struct FragmentRange {
            uint32_t begin = 0;
            uint32_t end = 0;
        };

        uint64_t packetId = 0;
        uint64_t devicePosition = 0;
        uint64_t qpcPosition = 0;
        uint32_t sampleRate = 0;
        uint16_t channels = 0;
        uint16_t bitsPerSample = 0;
        uint16_t blockAlign = 0;
        udp_protocol::AudioSampleFormat sampleFormat = udp_protocol::AudioSampleFormat::Unknown;
        uint32_t audioFrames = 0;
        uint32_t packetBytes = 0;
        uint32_t flags = 0;
        uint16_t fragmentCount = 0;
        uint16_t receivedFragments = 0;
        uint32_t receivedBytes = 0;
        std::vector<uint8_t> fragmentReceived;
        std::vector<FragmentRange> receivedRanges;
        Clock::time_point lastUpdated;
    };

    [[nodiscard]] bool WaitForReadable(std::chrono::milliseconds timeout);
    [[nodiscard]] std::optional<UdpCompletedFrame> ReceiveDatagram();
    [[nodiscard]] std::optional<UdpCompletedFrame> ReleaseReadyDelayedDatagram(Clock::time_point now);
    [[nodiscard]] std::optional<UdpCompletedFrame> ProcessDatagram(const std::byte* datagram, int datagramBytes);
    void ProcessAudioDatagram(const std::byte* datagram, int datagramBytes);
    void QueueDelayedDatagram(const std::byte* datagram, int datagramBytes, Clock::time_point releaseAt);
    [[nodiscard]] bool ShouldSimulateLoss();
    [[nodiscard]] std::chrono::milliseconds NextSimulatedJitterDelay();
    [[nodiscard]] std::chrono::milliseconds WaitUntilNextDelayedDatagram(Clock::time_point now) const;
    void DropExpiredFrames(Clock::time_point now);
    void EnforcePendingFrameLimit();
    void EnforcePendingAudioPacketLimit();

    uintptr_t socket_ = 0;
    UdpReceiverConfig config_{};
    UdpReceiverStats stats_{};
    std::vector<std::byte> datagramBuffer_;
    std::vector<std::byte> feedbackAddress_;
    int feedbackAddressLength_ = 0;
    std::deque<DelayedDatagram> delayedDatagrams_;
    std::unordered_map<uint64_t, PendingFrame> pendingFrames_;
    std::unordered_map<uint64_t, PendingAudioPacket> pendingAudioPackets_;
    std::mt19937 simulationRng_{1};
    bool winsockStarted_ = false;
};

uint16_t ParseUdpReceivePort(const char* value);

} // namespace screenshare
