#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace screenshare::udp_protocol {

constexpr uint32_t PacketMagic = 0x53535631; // "SSV1"
constexpr uint32_t FeedbackMagic = 0x53534631; // "SSF1"
constexpr uint32_t AudioMagic = 0x53534131; // "SSA1"
constexpr uint32_t ControlMagic = 0x53534331; // "SSC1"
constexpr uint16_t PacketVersion = 4;
constexpr uint16_t LegacyPacketVersion = 1;
constexpr uint16_t AccessCodePacketVersion = 2;
constexpr uint16_t MaxFragmentsPerFrame = 4096;
constexpr size_t CryptoNonceBytes = 12;
constexpr size_t CryptoTagBytes = 16;
constexpr size_t CryptoSessionSaltBytes = 16;

enum PacketFlags : uint32_t {
    PacketFlagEncrypted = 1U << 0,
};

enum class FeedbackHealthState : uint16_t {
    Unknown = 0,
    Waiting = 1,
    Ok = 2,
    Loss = 3,
    Recovering = 4,
    Buffering = 5,
    PreviewDrop = 6,
};

enum class AudioSampleFormat : uint16_t {
    Unknown = 0,
    Float32 = 1,
    Pcm16 = 2,
    Pcm24 = 3,
    Pcm32 = 4,
};

enum class AudioCodec : uint16_t {
    Raw = 0,
    Opus = 1,
};

enum AudioPacketFlags : uint32_t {
    AudioPacketFlagSilent = 1U << 0,
    AudioPacketFlagDataDiscontinuity = 1U << 1,
    AudioPacketFlagTimestampError = 1U << 2,
};

// Remote-control channel. Viewer -> host: input events + RequestControl/ReleaseControl.
// Host -> viewer: GrantControl/DenyControl/RevokeControl acknowledgements.
enum class ControlCommandType : uint16_t {
    Unknown = 0,
    RequestControl = 1,
    ReleaseControl = 2,
    GrantControl = 3,
    DenyControl = 4,
    RevokeControl = 5,
    MouseMove = 16,
    MouseButton = 17,
    MouseScroll = 18,
    KeyEvent = 19,
};

// Per-input-type permission bitmask. The host grants each independently.
enum ControlCapabilityFlags : uint32_t {
    ControlCapabilityMouse = 1U << 0,
    ControlCapabilityKeyboard = 1U << 1,
    ControlCapabilityGamepad = 1U << 2, // reserved for v2 (gamepad injection)
};

// Mouse button identifiers used by MouseButton commands.
enum class ControlMouseButton : uint16_t {
    Left = 0,
    Right = 1,
    Middle = 2,
    X1 = 3,
    X2 = 4,
};

// Parsed, host-order representation of a control packet.
struct ControlMessage {
    ControlCommandType command = ControlCommandType::Unknown;
    uint64_t sequence = 0;
    uint64_t sessionFingerprint = 0;
    uint64_t accessCodeFingerprint = 0;
    uint32_t capabilities = 0; // for GrantControl/RevokeControl
    float mouseX = 0.0f;       // normalized [0..1] across the captured surface
    float mouseY = 0.0f;
    int32_t scrollX = 0;       // wheel deltas (WHEEL_DELTA units)
    int32_t scrollY = 0;
    uint16_t button = 0;       // ControlMouseButton for MouseButton commands
    uint16_t key = 0;          // virtual-key code for KeyEvent
    uint16_t scancode = 0;     // hardware scancode for KeyEvent
    uint16_t modifiers = 0;    // reserved modifier bitmask
    bool pressed = false;      // down (true) vs up (false) for button/key
};

struct FeedbackSnapshot {
    FeedbackHealthState healthState = FeedbackHealthState::Unknown;
    uint64_t sequence = 0;
    uint64_t sessionFingerprint = 0;
    uint64_t accessCodeFingerprint = 0;
    uint64_t completedFrames = 0;
    uint64_t droppedDatagrams = 0;
    uint64_t invalidDatagrams = 0;
    uint64_t incompleteFramesDropped = 0;
    uint64_t decodeResyncs = 0;
    uint64_t decodeSkippedPackets = 0;
    uint64_t previewLateDrops = 0;
    uint64_t previewOverflowDrops = 0;
    uint32_t pendingFrames = 0;
    uint32_t pendingDecodePackets = 0;
    uint32_t previewQueuedFrames = 0;
};

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t headerBytes = 0;
    uint64_t frameId = 0;
    uint64_t timestamp100ns = 0;
    uint64_t senderQpc100ns = 0;
    uint64_t accessCodeFingerprint = 0;
    uint32_t frameBytes = 0;
    uint32_t fragmentOffset = 0;
    uint16_t fragmentIndex = 0;
    uint16_t fragmentCount = 0;
    uint32_t payloadBytes = 0;
    uint32_t flags = 0;
    std::byte encryptionNonce[CryptoNonceBytes]{};
    std::byte sessionSalt[CryptoSessionSaltBytes]{};
    std::byte encryptionTag[CryptoTagBytes]{};
};

struct FeedbackPacket {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t packetBytes = 0;
    uint32_t flags = 0;
    std::byte encryptionNonce[CryptoNonceBytes]{};
    std::byte sessionSalt[CryptoSessionSaltBytes]{};
    std::byte encryptionTag[CryptoTagBytes]{};
    uint64_t sequence = 0;
    uint64_t completedFrames = 0;
    uint64_t droppedDatagrams = 0;
    uint64_t invalidDatagrams = 0;
    uint64_t incompleteFramesDropped = 0;
    uint64_t decodeResyncs = 0;
    uint64_t decodeSkippedPackets = 0;
    uint64_t previewLateDrops = 0;
    uint64_t previewOverflowDrops = 0;
    uint32_t pendingFrames = 0;
    uint32_t pendingDecodePackets = 0;
    uint32_t previewQueuedFrames = 0;
    uint16_t healthState = 0;
    uint16_t reserved = 0;
    uint64_t sessionFingerprint = 0;
    uint64_t accessCodeFingerprint = 0;
};

struct AudioPacketHeader {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t headerBytes = 0;
    uint64_t packetId = 0;
    uint64_t devicePosition = 0;
    uint64_t qpcPosition = 0;
    uint64_t accessCodeFingerprint = 0;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    uint16_t blockAlign = 0;
    uint16_t sampleFormat = 0;
    uint16_t codec = 0;
    uint16_t reserved = 0;
    uint32_t audioFrames = 0;
    uint32_t packetBytes = 0;
    uint32_t fragmentOffset = 0;
    uint16_t fragmentIndex = 0;
    uint16_t fragmentCount = 0;
    uint32_t payloadBytes = 0;
    uint32_t flags = 0;
    uint32_t encryptionFlags = 0;
    std::byte encryptionNonce[CryptoNonceBytes]{};
    std::byte sessionSalt[CryptoSessionSaltBytes]{};
    std::byte encryptionTag[CryptoTagBytes]{};
};

struct ControlPacket {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t packetBytes = 0;
    uint32_t flags = 0;
    std::byte encryptionNonce[CryptoNonceBytes]{};
    std::byte sessionSalt[CryptoSessionSaltBytes]{};
    std::byte encryptionTag[CryptoTagBytes]{};
    // Encrypted region begins here (offsetof sequence).
    uint64_t sequence = 0;
    uint64_t sessionFingerprint = 0;
    uint64_t accessCodeFingerprint = 0;
    uint16_t command = 0;
    uint16_t button = 0;
    uint16_t key = 0;
    uint16_t scancode = 0;
    uint16_t modifiers = 0;
    uint16_t reserved = 0;
    uint32_t capabilities = 0;
    uint32_t mouseX = 0; // normalized [0..1] stored as fixed-point (x * 1e6)
    uint32_t mouseY = 0;
    int32_t scrollX = 0;
    int32_t scrollY = 0;
    uint32_t pressed = 0;
    uint32_t reserved2 = 0; // pads to an 8-byte multiple so size is layout-invariant
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 104);
static_assert(sizeof(FeedbackPacket) == 160);
static_assert(sizeof(AudioPacketHeader) == 128);
static_assert(sizeof(ControlPacket) == 120);

inline constexpr size_t PacketHeaderAuthenticatedBytes = offsetof(PacketHeader, encryptionTag);
inline constexpr size_t AudioPacketHeaderAuthenticatedBytes = offsetof(AudioPacketHeader, encryptionTag);
inline constexpr size_t FeedbackPacketAuthenticatedBytes = offsetof(FeedbackPacket, encryptionTag);
inline constexpr size_t FeedbackPacketEncryptedPayloadOffset =
    offsetof(FeedbackPacket, sequence);
inline constexpr size_t ControlPacketAuthenticatedBytes = offsetof(ControlPacket, encryptionTag);
inline constexpr size_t ControlPacketEncryptedPayloadOffset = offsetof(ControlPacket, sequence);

constexpr uint16_t ByteSwap16(uint16_t value) noexcept
{
    return static_cast<uint16_t>((value >> 8) | (value << 8));
}

constexpr uint32_t ByteSwap32(uint32_t value) noexcept
{
    return ((value & 0x000000FFU) << 24) |
           ((value & 0x0000FF00U) << 8) |
           ((value & 0x00FF0000U) >> 8) |
           ((value & 0xFF000000U) >> 24);
}

constexpr uint64_t ByteSwap64(uint64_t value) noexcept
{
    return ((value & 0x00000000000000FFULL) << 56) |
           ((value & 0x000000000000FF00ULL) << 40) |
           ((value & 0x0000000000FF0000ULL) << 24) |
           ((value & 0x00000000FF000000ULL) << 8) |
           ((value & 0x000000FF00000000ULL) >> 8) |
           ((value & 0x0000FF0000000000ULL) >> 24) |
           ((value & 0x00FF000000000000ULL) >> 40) |
           ((value & 0xFF00000000000000ULL) >> 56);
}

constexpr uint16_t ToNetwork16(uint16_t value) noexcept
{
    if constexpr (std::endian::native == std::endian::little) {
        return ByteSwap16(value);
    }
    return value;
}

constexpr uint32_t ToNetwork32(uint32_t value) noexcept
{
    if constexpr (std::endian::native == std::endian::little) {
        return ByteSwap32(value);
    }
    return value;
}

constexpr uint64_t ToNetwork64(uint64_t value) noexcept
{
    if constexpr (std::endian::native == std::endian::little) {
        return ByteSwap64(value);
    }
    return value;
}

constexpr uint16_t FromNetwork16(uint16_t value) noexcept
{
    return ToNetwork16(value);
}

constexpr uint32_t FromNetwork32(uint32_t value) noexcept
{
    return ToNetwork32(value);
}

constexpr uint64_t FromNetwork64(uint64_t value) noexcept
{
    return ToNetwork64(value);
}

const char* FeedbackHealthStateName(FeedbackHealthState state);
const char* AudioSampleFormatName(AudioSampleFormat format);
const char* AudioCodecName(AudioCodec codec);
std::vector<std::byte> BuildFeedbackDatagram(const FeedbackSnapshot& feedback);
std::optional<FeedbackSnapshot> ParseFeedbackDatagram(std::span<const std::byte> datagram);
std::vector<std::byte> BuildControlDatagram(const ControlMessage& message);
std::optional<ControlMessage> ParseControlDatagram(std::span<const std::byte> datagram);
const char* ControlCommandName(ControlCommandType command);

} // namespace screenshare::udp_protocol
