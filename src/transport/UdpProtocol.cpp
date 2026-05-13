#include "transport/UdpProtocol.h"

#include <cstring>

namespace screenshare::udp_protocol {
namespace {

constexpr size_t LegacyFeedbackPacketBytes = 96;

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
    packet.sequence = ToNetwork64(feedback.sequence);
    packet.sessionFingerprint = ToNetwork64(feedback.sessionFingerprint);
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
        datagram.size() != sizeof(FeedbackPacket)) {
        return std::nullopt;
    }

    FeedbackPacket packet{};
    std::memcpy(&packet, datagram.data(), datagram.size());

    const uint32_t magic = FromNetwork32(packet.magic);
    const uint16_t version = FromNetwork16(packet.version);
    const uint16_t packetBytes = FromNetwork16(packet.packetBytes);
    const uint16_t healthState = FromNetwork16(packet.healthState);
    if (magic != FeedbackMagic ||
        version != PacketVersion ||
        packetBytes != datagram.size() ||
        !IsKnownFeedbackHealthState(healthState)) {
        return std::nullopt;
    }

    FeedbackSnapshot feedback;
    feedback.healthState = static_cast<FeedbackHealthState>(healthState);
    feedback.sequence = FromNetwork64(packet.sequence);
    if (datagram.size() >= sizeof(FeedbackPacket)) {
        feedback.sessionFingerprint = FromNetwork64(packet.sessionFingerprint);
    }
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
