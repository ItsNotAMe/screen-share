#pragma once

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

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] const UdpReceiverStats& stats() const noexcept { return stats_; }
    [[nodiscard]] size_t pendingFrameCount() const noexcept { return pendingFrames_.size(); }
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

    [[nodiscard]] bool WaitForReadable(std::chrono::milliseconds timeout);
    [[nodiscard]] std::optional<UdpCompletedFrame> ReceiveDatagram();
    [[nodiscard]] std::optional<UdpCompletedFrame> ReleaseReadyDelayedDatagram(Clock::time_point now);
    [[nodiscard]] std::optional<UdpCompletedFrame> ProcessDatagram(const std::byte* datagram, int datagramBytes);
    void QueueDelayedDatagram(const std::byte* datagram, int datagramBytes, Clock::time_point releaseAt);
    [[nodiscard]] bool ShouldSimulateLoss();
    [[nodiscard]] std::chrono::milliseconds NextSimulatedJitterDelay();
    [[nodiscard]] std::chrono::milliseconds WaitUntilNextDelayedDatagram(Clock::time_point now) const;
    void DropExpiredFrames(Clock::time_point now);
    void EnforcePendingFrameLimit();

    uintptr_t socket_ = 0;
    UdpReceiverConfig config_{};
    UdpReceiverStats stats_{};
    std::vector<std::byte> datagramBuffer_;
    std::deque<DelayedDatagram> delayedDatagrams_;
    std::unordered_map<uint64_t, PendingFrame> pendingFrames_;
    std::mt19937 simulationRng_{1};
    bool winsockStarted_ = false;
};

uint16_t ParseUdpReceivePort(const char* value);

} // namespace screenshare
