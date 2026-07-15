#pragma once

#include "transport/UdpCrypto.h"
#include "transport/UdpProtocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace screenshare {

struct UdpNatProbeTarget {
    std::string host;
    uint16_t port = 0;
    bool localEndpoint = false;
};

struct UdpReceiverConfig {
    uint16_t port = 0;
    uint32_t maxDatagramBytes = 65'507;
    uint32_t maxFrameBytes = 16 * 1024 * 1024;
    uint32_t socketReceiveBufferBytes = 4 * 1024 * 1024;
    size_t maxPendingFrames = 256;
    // Aggregate cap on memory held by in-progress (not-yet-complete) video
    // reassembly. Bounds a memory-exhaustion DoS where a peer opens many
    // frame ids that each claim a large size but never complete. Independent
    // of maxPendingFrames so small-frame and large-frame floods are both
    // bounded.
    uint64_t maxPendingFrameBytes = 96ull * 1024 * 1024;
    size_t maxPendingAudioPackets = 512;
    size_t maxCompletedAudioPackets = 512;
    uint32_t maxAudioPacketBytes = 2 * 1024 * 1024;
    // Aggregate cap on memory held by in-progress audio reassembly, same
    // rationale as maxPendingFrameBytes.
    uint64_t maxPendingAudioBytes = 32ull * 1024 * 1024;
    std::chrono::milliseconds frameTimeout = std::chrono::seconds(5);
    float simulatedLossPercent = 0.0f;
    std::chrono::milliseconds simulatedJitter = std::chrono::milliseconds(0);
    uint32_t simulationSeed = 1;
    uint64_t accessCodeFingerprint = 0;
    std::optional<UdpCryptoKey> encryptionKey;
    std::vector<UdpNatProbeTarget> natProbeTargets;
    std::chrono::milliseconds natProbeInterval{250};
    uint64_t natProbeSessionFingerprint = 0;
};

struct UdpCompletedFrame {
    uint64_t frameId = 0;
    uint64_t timestamp100ns = 0;
    uint64_t senderQpc100ns = 0;
    uint16_t fragmentCount = 0;
    std::vector<std::byte> bytes;
};

struct UdpCompletedAudioPacket {
    uint64_t packetId = 0;
    uint64_t devicePosition = 0;
    uint64_t qpcPosition = 0;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    uint16_t blockAlign = 0;
    udp_protocol::AudioSampleFormat sampleFormat = udp_protocol::AudioSampleFormat::Unknown;
    udp_protocol::AudioCodec codec = udp_protocol::AudioCodec::Raw;
    uint32_t audioFrames = 0;
    uint32_t flags = 0;
    uint16_t fragmentCount = 0;
    std::vector<std::byte> bytes;
};

struct UdpReceiverStats {
    uint64_t datagramsReceived = 0;
    uint64_t datagramsAccepted = 0;
    uint64_t invalidDatagrams = 0;
    uint64_t accessRejectedDatagrams = 0;
    uint64_t cryptoRejectedDatagrams = 0;
    uint64_t duplicateFragments = 0;
    uint64_t framesCompleted = 0;
    uint64_t incompleteFramesDropped = 0;
    uint64_t payloadBytesReceived = 0;
    uint64_t completedFrameBytes = 0;
    uint64_t simulatedDatagramsDropped = 0;
    uint64_t simulatedDatagramsDelayed = 0;
    uint64_t feedbackPacketsSent = 0;
    uint64_t feedbackSendErrors = 0;
    uint64_t encryptedFeedbackPacketsSent = 0;
    uint64_t audioDatagramsAccepted = 0;
    uint64_t audioDuplicateFragments = 0;
    uint64_t audioPacketsCompleted = 0;
    uint64_t audioPacketsQueued = 0;
    uint64_t audioQueuedPacketsDropped = 0;
    uint64_t audioIncompletePacketsDropped = 0;
    uint64_t audioPayloadBytesReceived = 0;
    uint64_t audioCompletedPacketBytes = 0;
    uint64_t audioFramesCompleted = 0;
    uint64_t audioSilentPackets = 0;
    uint64_t audioDiscontinuities = 0;
    uint64_t audioTimestampErrors = 0;
    uint64_t audioFormatChanges = 0;
    uint64_t natProbePublicPacketsSent = 0;
    uint64_t natProbeLocalPacketsSent = 0;
    uint64_t natProbeSendErrors = 0;
    uint64_t latestAudioPacketId = 0;
    uint64_t latestAudioDevicePosition = 0;
    uint64_t latestAudioQpcPosition = 0;
    std::string latestMediaEndpoint;
    uint32_t audioSampleRate = 0;
    uint16_t audioChannels = 0;
    uint16_t audioBitsPerSample = 0;
    uint16_t audioBlockAlign = 0;
    udp_protocol::AudioSampleFormat audioSampleFormat = udp_protocol::AudioSampleFormat::Unknown;
    udp_protocol::AudioCodec audioCodec = udp_protocol::AudioCodec::Raw;
};

class UdpReceiver {
public:
    UdpReceiver();
    ~UdpReceiver();

    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    void Open(const UdpReceiverConfig& config);
    void Close();
    void ResetMediaQueues();
    bool AddNatProbeTarget(const UdpNatProbeTarget& target);
    [[nodiscard]] std::optional<UdpCompletedFrame> ReceiveFrame(std::chrono::milliseconds timeout);
    [[nodiscard]] std::optional<UdpCompletedAudioPacket> PopAudioPacket();
    bool SendFeedback(const udp_protocol::FeedbackSnapshot& feedback);

    // Remote-control channel (viewer side). SendControl ships an input event or
    // RequestControl/ReleaseControl to the host over the same socket as feedback.
    // The handler is invoked for host->viewer control messages (grant/deny/revoke).
    bool SendControl(const udp_protocol::ControlMessage& message);
    void SetControlHandler(std::function<void(const udp_protocol::ControlMessage&)> handler);

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] const UdpReceiverStats& stats() const noexcept { return stats_; }
    [[nodiscard]] size_t pendingFrameCount() const noexcept { return pendingFrames_.size(); }
    [[nodiscard]] size_t pendingAudioPacketCount() const noexcept { return pendingAudioPackets_.size(); }
    [[nodiscard]] size_t completedAudioPacketCount() const noexcept { return completedAudioPackets_.size(); }
    [[nodiscard]] size_t delayedDatagramCount() const noexcept { return delayedDatagrams_.size(); }

private:
    using Clock = std::chrono::steady_clock;

    struct DelayedDatagram {
        Clock::time_point releaseAt;
        std::vector<std::byte> bytes;
    };

    struct NatProbeTargetAddress {
        std::vector<std::byte> address;
        int addressLength = 0;
        bool localEndpoint = false;
    };

    struct PendingFrame {
        struct FragmentRange {
            uint32_t begin = 0;
            uint32_t end = 0;
        };

        uint64_t frameId = 0;
        uint64_t timestamp100ns = 0;
        uint64_t senderQpc100ns = 0;
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
        udp_protocol::AudioCodec codec = udp_protocol::AudioCodec::Raw;
        uint32_t audioFrames = 0;
        uint32_t packetBytes = 0;
        uint32_t flags = 0;
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
    void ProcessAudioDatagram(const std::byte* datagram, int datagramBytes);
    void ProcessControlDatagram(const std::byte* datagram, int datagramBytes);
    void QueueDelayedDatagram(const std::byte* datagram, int datagramBytes, Clock::time_point releaseAt);
    [[nodiscard]] bool ShouldSimulateLoss();
    [[nodiscard]] std::chrono::milliseconds NextSimulatedJitterDelay();
    [[nodiscard]] std::chrono::milliseconds WaitUntilNextDelayedDatagram(Clock::time_point now) const;
    void DropExpiredFrames(Clock::time_point now);
    void EnforcePendingFrameLimit();
    void EnforcePendingAudioPacketLimit();
    void EnforceCompletedAudioPacketLimit();
    [[nodiscard]] std::optional<std::vector<std::byte>> DecryptDatagramPayload(
        const UdpAesGcm& crypto,
        const std::byte* datagram,
        int datagramBytes,
        size_t headerBytes,
        size_t authenticatedHeaderBytes,
        std::span<const std::byte, UdpCryptoNonceBytes> nonce,
        std::span<const std::byte, UdpCryptoTagBytes> tag);
    // Resolve (deriving and caching) the AES-GCM context for an inbound packet's
    // per-session salt. Returns nullptr when encryption is off. Inbound video/
    // audio/control-ack all carry the sender's salt.
    [[nodiscard]] UdpAesGcm* DecryptCryptoForSalt(const UdpCryptoSessionSalt& salt);
    bool EncryptFeedbackDatagram(std::vector<std::byte>& datagram);
    bool EncryptControlDatagram(std::vector<std::byte>& datagram);
    void MaybeSendNatProbes(Clock::time_point now);

    uintptr_t socket_ = 0;
    UdpReceiverConfig config_{};
    UdpReceiverStats stats_{};
    std::vector<std::byte> datagramBuffer_;
    std::vector<std::byte> feedbackAddress_;
    int feedbackAddressLength_ = 0;
    std::string currentDatagramEndpoint_;
    std::deque<DelayedDatagram> delayedDatagrams_;
    std::vector<NatProbeTargetAddress> natProbeTargets_;
    std::deque<UdpCompletedAudioPacket> completedAudioPackets_;
    std::unordered_map<uint64_t, PendingFrame> pendingFrames_;
    std::unordered_map<uint64_t, PendingAudioPacket> pendingAudioPackets_;
    std::mt19937 simulationRng_{1};
    // Encryption: a per-session key derived from the access-code master and a
    // random salt generated at Open. Outbound feedback/control use encryptCrypto_
    // and stamp sessionSalt_ into each header; inbound video/audio/control-ack
    // carry the sender's salt, so those keys are derived on demand and cached.
    bool encryptionEnabled_ = false;
    UdpCryptoKey master_{};
    UdpCryptoSessionSalt sessionSalt_{};
    std::unique_ptr<UdpAesGcm> encryptCrypto_;
    struct PeerDecryptCrypto {
        UdpCryptoSessionSalt salt{};
        std::unique_ptr<UdpAesGcm> crypto;
    };
    std::vector<PeerDecryptCrypto> peerDecryptCryptos_;
    uint64_t nextControlSequence_ = 1;
    std::function<void(const udp_protocol::ControlMessage&)> onControl_;
    Clock::time_point nextNatProbeAt_{};
    uint64_t nextNatProbeSequence_ = 1;
    bool winsockStarted_ = false;
};

uint16_t ParseUdpReceivePort(const char* value);

} // namespace screenshare
