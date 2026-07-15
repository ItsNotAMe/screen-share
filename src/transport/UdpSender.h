#pragma once

#include "codec/H264StreamEncoder.h"
#include "transport/NatTraversal.h"
#include "transport/UdpCrypto.h"
#include "transport/UdpProtocol.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace screenshare {

struct UdpSenderEndpoint {
    std::string host;
    uint16_t port = 0;
    uint32_t group = 0;
};

struct UdpSenderConfig {
    std::string host;
    uint16_t port = 0;
    uint32_t group = 0;
    std::vector<UdpSenderEndpoint> additionalTargets;
    uint16_t localPort = 0;
    uint32_t maxPayloadBytes = 1'200;
    uint32_t pacingBitrate = 0;
    uint32_t maxQueuedDatagrams = 4'096;
    std::chrono::milliseconds maxQueueDelay{0};
    uint64_t accessCodeFingerprint = 0;
    std::optional<UdpCryptoKey> encryptionKey;
    bool pacingEnabled = true;
    bool retargetOnNatProbe = false;
    bool collectNatProbeTargets = false;
    bool preferNatProbeTargets = false;
    uint32_t maxNatProbeTargets = 32;
    uint64_t natProbeSessionFingerprint = 0;
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
    uint64_t pendingQueueDelayMs = 0;
    uint64_t peakQueueDelayMs = 0;
    uint64_t feedbackPacketsReceived = 0;
    uint64_t invalidFeedbackPackets = 0;
    uint64_t feedbackAccessRejected = 0;
    uint64_t feedbackCryptoRejected = 0;
    uint64_t controlReplayRejected = 0;
    uint64_t natProbePacketsReceived = 0;
    uint64_t natProbeRetargets = 0;
    uint64_t natProbeRetargetRejected = 0;
    uint64_t natProbeTargetCount = 0;
    bool natProbeRetargetActive = false;
    std::string natProbeRetargetEndpoint;
    bool hasFeedback = false;
    bool encryptionEnabled = false;
    udp_protocol::FeedbackSnapshot latestFeedback;
    struct FeedbackPeer {
        std::string endpoint;
        uint32_t group = 0;
        uint64_t packetsReceived = 0;
        udp_protocol::FeedbackSnapshot latestFeedback;
    };
    std::vector<FeedbackPeer> feedbackPeers;
};

struct UdpAudioPacket {
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
    bool AddAdditionalTarget(const UdpSenderEndpoint& target);
    void SendFrame(const EncodedPacket& packet);
    void SendAudioPacket(const UdpAudioPacket& packet);
    void SetPacingBitrate(uint32_t bitrate);
    void SetPacingEnabled(bool enabled);
    void Flush();
    [[nodiscard]] std::optional<udp_protocol::FeedbackSnapshot> ReceiveFeedback(std::chrono::milliseconds timeout);

    // Remote-control channel (host side). Incoming viewer control packets are
    // surfaced through the handler (decoded during ReceiveFeedback). SendControlTo
    // ships a grant/deny/revoke acknowledgement back to a specific viewer endpoint
    // that has previously sent a control packet.
    void SetControlHandler(std::function<void(const std::string& endpoint, const udp_protocol::ControlMessage&)> handler);
    bool SendControlTo(const std::string& endpoint, const udp_protocol::ControlMessage& message);

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] UdpSenderStats stats() const;

private:
    using Clock = std::chrono::steady_clock;

    enum class PendingDatagramKind {
        Video,
        Audio,
    };

    struct PendingDatagram {
        std::vector<std::byte> bytes;
        Clock::time_point sendAt{};
        PendingDatagramKind kind = PendingDatagramKind::Video;
        uint64_t mediaId = 0;
    };

    struct GroupedAddress {
        std::vector<std::byte> address;
        uint32_t group = 0;
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
    bool EnforceLiveQueueDelayLocked(Clock::time_point now);
    bool DropOldestQueuedMediaLocked(PendingDatagramKind kind);
    void DropQueuedMediaForCapacityLocked(size_t incomingDatagrams, PendingDatagramKind preferredKind);
    void MaybeRetargetFromNatProbe(
        const void* address,
        int addressLength,
        const NatProbeDatagramInfo& probe);
    void RebuildSendAddressesLocked();
    [[nodiscard]] uint32_t EndpointGroupForAddressLocked(const std::vector<std::byte>& address) const;
    // True if this raw socket address has proven possession of the session key
    // by delivering a validly-decrypting feedback/control packet (i.e. it is a
    // known control peer). Used to gate NAT-probe retargeting on encrypted
    // sessions so an unauthenticated probe cannot redirect the media stream.
    [[nodiscard]] bool IsVerifiedEndpointLocked(const std::vector<std::byte>& address) const;
    void RescheduleQueueLocked(Clock::time_point now);
    void CheckWorkerErrorLocked() const;
    void UpdatePendingStatsLocked();
    void EncryptDatagramPayload(
        const UdpAesGcm& crypto,
        std::vector<std::byte>& datagram,
        size_t headerBytes,
        size_t authenticatedHeaderBytes,
        std::span<const std::byte, UdpCryptoNonceBytes> nonce,
        std::span<std::byte, UdpCryptoTagBytes> tag);
    // Resolve (deriving and caching if needed) the AES-GCM context for an inbound
    // packet's per-session salt. Returns nullptr when encryption is off. Used for
    // feedback/control, which arrive from viewers that each carry their own salt.
    [[nodiscard]] UdpAesGcm* DecryptCryptoForSalt(const UdpCryptoSessionSalt& salt);
    [[nodiscard]] std::optional<std::vector<std::byte>> DecryptFeedbackDatagram(std::span<const std::byte> datagram);
    bool EncryptControlDatagram(std::vector<std::byte>& datagram);
    void ProcessControlPacket(const void* address, int addressLength, std::span<const std::byte> datagram);

    struct ControlPeer {
        std::string endpoint;
        std::vector<std::byte> address;
        int addressLength = 0;
        // Anti-replay: the highest control sequence accepted from this peer
        // within the current control session. A new session (different
        // sessionFingerprint) resets the baseline so a legitimate viewer
        // restart is not mistaken for a replay.
        uint64_t controlSessionFingerprint = 0;
        uint64_t lastControlSequence = 0;
        bool hasControlSequence = false;
    };

    uintptr_t socket_ = 0;
    std::vector<std::byte> address_;
    std::vector<GroupedAddress> additionalAddresses_;
    std::vector<GroupedAddress> natProbeAddresses_;
    std::shared_ptr<std::vector<std::vector<std::byte>>> cachedSendAddresses_;
    std::vector<UdpSenderStats::FeedbackPeer> feedbackPeers_;
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
    // Encryption: a per-session key derived from the access-code master and a
    // random salt generated at Open. This sender's outbound video/audio/control
    // all use encryptCrypto_ and stamp sessionSalt_ into each header. Inbound
    // feedback/control from viewers each carry the viewer's own salt, so those
    // keys are derived on demand and cached in peerDecryptCryptos_.
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
    std::function<void(const std::string& endpoint, const udp_protocol::ControlMessage&)> onControl_;
    std::vector<ControlPeer> controlPeers_;
    bool stopWorker_ = false;
    bool datagramInFlight_ = false;
    bool sendAddressesDirty_ = true;
    bool winsockStarted_ = false;
};

UdpSenderConfig ParseUdpSenderTarget(const std::string& target);

} // namespace screenshare
