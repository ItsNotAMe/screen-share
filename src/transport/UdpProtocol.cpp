#include "transport/UdpProtocol.h"

#include <cstring>

namespace screenshare::udp_protocol {
namespace {

constexpr size_t LegacyFeedbackPacketBytes = 96;
constexpr size_t SessionFeedbackPacketBytes = 104;
constexpr size_t AccessCodeFeedbackPacketBytes = 112;

#pragma pack(push, 1)
struct LegacyFeedbackPacket {
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

struct SessionFeedbackPacket {
    LegacyFeedbackPacket base;
    uint64_t sessionFingerprint = 0;
};

struct AccessCodeFeedbackPacket {
    SessionFeedbackPacket base;
    uint64_t accessCodeFingerprint = 0;
};
#pragma pack(pop)

static_assert(sizeof(LegacyFeedbackPacket) == LegacyFeedbackPacketBytes);
static_assert(sizeof(SessionFeedbackPacket) == SessionFeedbackPacketBytes);
static_assert(sizeof(AccessCodeFeedbackPacket) == AccessCodeFeedbackPacketBytes);

bool IsKnownFeedbackHealthState(uint16_t state)
{
    switch (static_cast<FeedbackHealthState>(state)) {
    case FeedbackHealthState::Unknown:
    case FeedbackHealthState::Waiting:
    case FeedbackHealthState::Ok:
    case FeedbackHealthState::Loss:
    case FeedbackHealthState::Recovering:
    case FeedbackHealthState::Buffering:
    case FeedbackHealthState::PreviewDrop:
        return true;
    default:
        return false;
    }
}

FeedbackSnapshot ParseLegacyFeedbackPacket(const LegacyFeedbackPacket& packet)
{
    FeedbackSnapshot feedback;
    feedback.healthState = static_cast<FeedbackHealthState>(FromNetwork16(packet.healthState));
    feedback.sequence = FromNetwork64(packet.sequence);
    feedback.completedFrames = FromNetwork64(packet.completedFrames);
    feedback.droppedDatagrams = FromNetwork64(packet.droppedDatagrams);
    feedback.invalidDatagrams = FromNetwork64(packet.invalidDatagrams);
    feedback.incompleteFramesDropped = FromNetwork64(packet.incompleteFramesDropped);
    feedback.decodeResyncs = FromNetwork64(packet.decodeResyncs);
    feedback.decodeSkippedPackets = FromNetwork64(packet.decodeSkippedPackets);
    feedback.previewLateDrops = FromNetwork64(packet.previewLateDrops);
    feedback.previewOverflowDrops = FromNetwork64(packet.previewOverflowDrops);
    feedback.pendingFrames = FromNetwork32(packet.pendingFrames);
    feedback.pendingDecodePackets = FromNetwork32(packet.pendingDecodePackets);
    feedback.previewQueuedFrames = FromNetwork32(packet.previewQueuedFrames);
    return feedback;
}

} // namespace

const char* FeedbackHealthStateName(FeedbackHealthState state)
{
    switch (state) {
    case FeedbackHealthState::Waiting:
        return "waiting";
    case FeedbackHealthState::Ok:
        return "ok";
    case FeedbackHealthState::Loss:
        return "loss";
    case FeedbackHealthState::Recovering:
        return "recovering";
    case FeedbackHealthState::Buffering:
        return "buffering";
    case FeedbackHealthState::PreviewDrop:
        return "preview-drop";
    case FeedbackHealthState::Unknown:
    default:
        return "unknown";
    }
}

const char* AudioSampleFormatName(AudioSampleFormat format)
{
    switch (format) {
    case AudioSampleFormat::Float32:
        return "float32";
    case AudioSampleFormat::Pcm16:
        return "pcm16";
    case AudioSampleFormat::Pcm24:
        return "pcm24";
    case AudioSampleFormat::Pcm32:
        return "pcm32";
    case AudioSampleFormat::Unknown:
    default:
        return "unknown";
    }
}

const char* AudioCodecName(AudioCodec codec)
{
    switch (codec) {
    case AudioCodec::Raw:
        return "raw";
    case AudioCodec::Opus:
        return "opus";
    default:
        return "unknown";
    }
}

std::vector<std::byte> BuildFeedbackDatagram(const FeedbackSnapshot& feedback)
{
    FeedbackPacket packet;
    packet.magic = ToNetwork32(FeedbackMagic);
    packet.version = ToNetwork16(PacketVersion);
    packet.packetBytes = ToNetwork16(static_cast<uint16_t>(sizeof(FeedbackPacket)));
    packet.flags = 0;
    packet.sequence = ToNetwork64(feedback.sequence);
    packet.sessionFingerprint = ToNetwork64(feedback.sessionFingerprint);
    packet.accessCodeFingerprint = ToNetwork64(feedback.accessCodeFingerprint);
    packet.completedFrames = ToNetwork64(feedback.completedFrames);
    packet.droppedDatagrams = ToNetwork64(feedback.droppedDatagrams);
    packet.invalidDatagrams = ToNetwork64(feedback.invalidDatagrams);
    packet.incompleteFramesDropped = ToNetwork64(feedback.incompleteFramesDropped);
    packet.decodeResyncs = ToNetwork64(feedback.decodeResyncs);
    packet.decodeSkippedPackets = ToNetwork64(feedback.decodeSkippedPackets);
    packet.previewLateDrops = ToNetwork64(feedback.previewLateDrops);
    packet.previewOverflowDrops = ToNetwork64(feedback.previewOverflowDrops);
    packet.pendingFrames = ToNetwork32(feedback.pendingFrames);
    packet.pendingDecodePackets = ToNetwork32(feedback.pendingDecodePackets);
    packet.previewQueuedFrames = ToNetwork32(feedback.previewQueuedFrames);
    packet.healthState = ToNetwork16(static_cast<uint16_t>(feedback.healthState));

    std::vector<std::byte> datagram(sizeof(packet));
    std::memcpy(datagram.data(), &packet, sizeof(packet));
    return datagram;
}

std::optional<FeedbackSnapshot> ParseFeedbackDatagram(std::span<const std::byte> datagram)
{
    if (datagram.size() != LegacyFeedbackPacketBytes &&
        datagram.size() != SessionFeedbackPacketBytes &&
        datagram.size() != AccessCodeFeedbackPacketBytes &&
        datagram.size() != sizeof(FeedbackPacket)) {
        return std::nullopt;
    }

    if (datagram.size() != sizeof(FeedbackPacket)) {
        AccessCodeFeedbackPacket packet{};
        std::memcpy(&packet, datagram.data(), datagram.size());

        const uint32_t magic = FromNetwork32(packet.base.base.magic);
        const uint16_t version = FromNetwork16(packet.base.base.version);
        const uint16_t packetBytes = FromNetwork16(packet.base.base.packetBytes);
        const uint16_t healthState = FromNetwork16(packet.base.base.healthState);
        if (magic != FeedbackMagic ||
            (version != LegacyPacketVersion && version != AccessCodePacketVersion) ||
            packetBytes != datagram.size() ||
            !IsKnownFeedbackHealthState(healthState)) {
            return std::nullopt;
        }
        if (version == AccessCodePacketVersion && datagram.size() != AccessCodeFeedbackPacketBytes) {
            return std::nullopt;
        }
        if (version == LegacyPacketVersion && datagram.size() == AccessCodeFeedbackPacketBytes) {
            return std::nullopt;
        }

        FeedbackSnapshot feedback = ParseLegacyFeedbackPacket(packet.base.base);
        if (datagram.size() >= SessionFeedbackPacketBytes) {
            feedback.sessionFingerprint = FromNetwork64(packet.base.sessionFingerprint);
        }
        if (datagram.size() >= AccessCodeFeedbackPacketBytes) {
            feedback.accessCodeFingerprint = FromNetwork64(packet.accessCodeFingerprint);
        }
        return feedback;
    }

    FeedbackPacket packet{};
    std::memcpy(&packet, datagram.data(), sizeof(packet));
    const uint32_t magic = FromNetwork32(packet.magic);
    const uint16_t version = FromNetwork16(packet.version);
    const uint16_t packetBytes = FromNetwork16(packet.packetBytes);
    const uint32_t flags = FromNetwork32(packet.flags);
    const uint16_t healthState = FromNetwork16(packet.healthState);
    if (magic != FeedbackMagic ||
        version != PacketVersion ||
        packetBytes != datagram.size() ||
        (flags & ~PacketFlagEncrypted) != 0 ||
        !IsKnownFeedbackHealthState(healthState)) {
        return std::nullopt;
    }

    FeedbackSnapshot feedback;
    feedback.healthState = static_cast<FeedbackHealthState>(healthState);
    feedback.sequence = FromNetwork64(packet.sequence);
    feedback.sessionFingerprint = FromNetwork64(packet.sessionFingerprint);
    feedback.accessCodeFingerprint = FromNetwork64(packet.accessCodeFingerprint);
    feedback.completedFrames = FromNetwork64(packet.completedFrames);
    feedback.droppedDatagrams = FromNetwork64(packet.droppedDatagrams);
    feedback.invalidDatagrams = FromNetwork64(packet.invalidDatagrams);
    feedback.incompleteFramesDropped = FromNetwork64(packet.incompleteFramesDropped);
    feedback.decodeResyncs = FromNetwork64(packet.decodeResyncs);
    feedback.decodeSkippedPackets = FromNetwork64(packet.decodeSkippedPackets);
    feedback.previewLateDrops = FromNetwork64(packet.previewLateDrops);
    feedback.previewOverflowDrops = FromNetwork64(packet.previewOverflowDrops);
    feedback.pendingFrames = FromNetwork32(packet.pendingFrames);
    feedback.pendingDecodePackets = FromNetwork32(packet.pendingDecodePackets);
    feedback.previewQueuedFrames = FromNetwork32(packet.previewQueuedFrames);
    return feedback;
}

} // namespace screenshare::udp_protocol
