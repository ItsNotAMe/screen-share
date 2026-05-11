#pragma once

#include "codec/H264StreamEncoder.h"
#include "transport/UdpProtocol.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace screenshare {

struct UdpSenderConfig {
    std::string host;
    uint16_t port = 0;
    uint32_t maxPayloadBytes = 1'200;
    uint32_t pacingBitrate = 0;
    uint32_t maxQueuedDatagrams = 4'096;
    bool pacingEnabled = true;
};

struct UdpSenderStats {
    uint64_t framesSent = 0;
    uint64_t framesDropped = 0;
    uint64_t audioPacketsSent = 0;
    uint64_t audioPacketsDropped = 0;
    uint64_t audioFramesSent = 0;
    uint64_t audioDatagramsQueued = 0;
    uint64_t audioPayloadBytesSent = 0;
    uint64_t datagramsQueued = 0;
    uint64_t datagramsSent = 0;
    uint64_t datagramsDropped = 0;
    uint64_t payloadBytesSent = 0;
    uint64_t wireBytesSent = 0;
    uint64_t pendingDatagrams = 0;
    uint64_t peakPendingDatagrams = 0;
    uint64_t feedbackPacketsReceived = 0;
    uint64_t invalidFeedbackPackets = 0;
    bool hasFeedback = false;
    udp_protocol::FeedbackSnapshot latestFeedback;
};

struct UdpAudioPacket {
    uint64_t devicePosition = 0;
    uint64_t qpcPosition = 0;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    uint16_t blockAlign = 0;
    udp_protocol::AudioSampleFormat sampleFormat = udp_protocol::AudioSampleFormat::Unknown;
    uint32_t audioFrames = 0;
    uint32_t flags = 0;
    std::vector<std::byte> bytes;
};

class UdpSender {
public:
    UdpSender();
    ~UdpSender();

    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    void Open(const UdpSenderConfig& config);
    void Close();
    void SendFrame(const EncodedPacket& packet);
    void SendAudioPacket(const UdpAudioPacket& packet);
    void SetPacingBitrate(uint32_t bitrate);
    void Flush();
    [[nodiscard]] std::optional<udp_protocol::FeedbackSnapshot> ReceiveFeedback(std::chrono::milliseconds timeout);

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] UdpSenderStats stats() const;

private:
    using Clock = std::chrono::steady_clock;

    struct PendingDatagram {
        std::vector<std::byte> bytes;
        Clock::time_point sendAt{};
    };

    std::vector<std::byte> BuildDatagram(
        const std::byte* payload,
        uint32_t payloadBytes,
        uint32_t fragmentOffset,
        uint16_t fragmentIndex,
        uint16_t fragmentCount,
        uint64_t frameId,
        const EncodedPacket& packet);
    std::vector<std::byte> BuildAudioDatagram(
        const std::byte* payload,
        uint32_t payloadBytes,
        uint32_t fragmentOffset,
        uint16_t fragmentIndex,
        uint16_t fragmentCount,
        uint64_t packetId,
        const UdpAudioPacket& packet);
    void WorkerLoop();
    void SendDatagramBytes(const std::vector<std::byte>& datagram);
    [[nodiscard]] Clock::duration PacingDelayForBytes(uint64_t wireBytes) const;
    void CheckWorkerErrorLocked() const;
    void UpdatePendingStatsLocked();

    uintptr_t socket_ = 0;
    std::vector<std::byte> address_;
    int addressLength_ = 0;
    UdpSenderConfig config_{};
    UdpSenderStats stats_{};
    uint64_t frameId_ = 0;
    uint64_t audioPacketId_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable queueChanged_;
    std::condition_variable queueDrained_;
    std::deque<PendingDatagram> queue_;
    std::thread worker_;
    Clock::time_point nextSendAt_{};
    std::string workerError_;
    bool stopWorker_ = false;
    bool datagramInFlight_ = false;
    bool winsockStarted_ = false;
};

UdpSenderConfig ParseUdpSenderTarget(const std::string& target);

} // namespace screenshare
