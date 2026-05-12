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
constexpr uint16_t PacketVersion = 1;
constexpr uint16_t MaxFragmentsPerFrame = 4096;

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

struct FeedbackSnapshot {
    FeedbackHealthState healthState = FeedbackHealthState::Unknown;
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
};

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t headerBytes = 0;
    uint64_t frameId = 0;
    uint64_t timestamp100ns = 0;
    uint64_t senderQpc100ns = 0;
    uint32_t frameBytes = 0;
    uint32_t fragmentOffset = 0;
    uint16_t fragmentIndex = 0;
    uint16_t fragmentCount = 0;
    uint32_t payloadBytes = 0;
};

struct FeedbackPacket {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t packetBytes = 0;
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
};

struct AudioPacketHeader {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t headerBytes = 0;
    uint64_t packetId = 0;
    uint64_t devicePosition = 0;
    uint64_t qpcPosition = 0;
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
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 48);
static_assert(sizeof(FeedbackPacket) == 96);
static_assert(sizeof(AudioPacketHeader) == 72);

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

} // namespace screenshare::udp_protocol
