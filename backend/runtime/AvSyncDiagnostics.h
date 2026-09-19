#pragma once

#include "transport/UdpReceiver.h"

#include <chrono>
#include <cstdint>

namespace screenshare_runtime_internal {

inline constexpr auto AvSyncMediaFreshness = std::chrono::milliseconds(750);

struct AvSyncSnapshot {
    bool hasVideo = false;
    bool hasVideoSenderClock = false;
    bool hasAudio = false;
    bool videoFresh = false;
    bool audioFresh = false;
    bool ready = false;
    bool senderClockReady = false;
    uint64_t videoAgeMs = 0;
    uint64_t audioAgeMs = 0;
    uint64_t videoFrames = 0;
    uint64_t audioPackets = 0;
    uint64_t ignoredVideoFrames = 0;
    uint64_t ignoredAudioPackets = 0;
    uint64_t firstVideoTimestamp100ns = 0;
    uint64_t latestVideoTimestamp100ns = 0;
    uint64_t firstVideoSenderQpc100ns = 0;
    uint64_t latestVideoSenderQpc100ns = 0;
    uint64_t firstAudioQpc100ns = 0;
    uint64_t latestAudioQpc100ns = 0;
    uint64_t latestVideoFrameId = 0;
    uint64_t latestAudioPacketId = 0;
    double videoElapsedMs = 0.0;
    double audioElapsedMs = 0.0;
    double audioAheadMs = 0.0;
    double initialAudioSenderOffsetMs = 0.0;
    double senderClockAudioAheadMs = 0.0;
    double senderTimelineAudioAheadMs = 0.0;
};

class AvSyncDiagnostics {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void Clear()
    {
        hasVideo_ = false;
        hasVideoSenderClock_ = false;
        hasAudio_ = false;
        videoFrames_ = 0;
        audioPackets_ = 0;
        ignoredVideoFrames_ = 0;
        ignoredAudioPackets_ = 0;
        firstVideoTimestamp100ns_ = 0;
        latestVideoTimestamp100ns_ = 0;
        firstVideoSenderQpc100ns_ = 0;
        latestVideoSenderQpc100ns_ = 0;
        firstAudioQpc100ns_ = 0;
        latestAudioQpc100ns_ = 0;
        latestVideoFrameId_ = 0;
        latestAudioPacketId_ = 0;
        latestVideoObservedAt_ = {};
        latestAudioObservedAt_ = {};
    }

    void ObserveVideoFrame(
        const screenshare::UdpCompletedFrame& frame,
        TimePoint observedAt = Clock::now())
    {
        if (hasVideo_ &&
            (frame.frameId <= latestVideoFrameId_ ||
             frame.timestamp100ns < latestVideoTimestamp100ns_ ||
             (frame.senderQpc100ns != 0 &&
              hasVideoSenderClock_ &&
              frame.senderQpc100ns < latestVideoSenderQpc100ns_))) {
            ++ignoredVideoFrames_;
            return;
        }

        if (!hasVideo_) {
            firstVideoTimestamp100ns_ = frame.timestamp100ns;
            hasVideo_ = true;
        }
        if (frame.senderQpc100ns != 0) {
            if (!hasVideoSenderClock_) {
                firstVideoSenderQpc100ns_ = frame.senderQpc100ns;
                hasVideoSenderClock_ = true;
            }
            latestVideoSenderQpc100ns_ = frame.senderQpc100ns;
        }

        latestVideoTimestamp100ns_ = frame.timestamp100ns;
        latestVideoFrameId_ = frame.frameId;
        latestVideoObservedAt_ = observedAt;
        ++videoFrames_;
    }

    void ObserveAudioPacket(
        const screenshare::UdpCompletedAudioPacket& packet,
        TimePoint observedAt = Clock::now())
    {
        const bool timestampError =
            (packet.flags & screenshare::udp_protocol::AudioPacketFlagTimestampError) != 0;
        if (timestampError ||
            packet.qpcPosition == 0 ||
            (hasAudio_ &&
             (packet.packetId <= latestAudioPacketId_ ||
              packet.qpcPosition <= latestAudioQpc100ns_))) {
            ++ignoredAudioPackets_;
            return;
        }

        if (!hasAudio_) {
            firstAudioQpc100ns_ = packet.qpcPosition;
            hasAudio_ = true;
        }

        latestAudioQpc100ns_ = packet.qpcPosition;
        latestAudioPacketId_ = packet.packetId;
        latestAudioObservedAt_ = observedAt;
        ++audioPackets_;
    }

    [[nodiscard]] AvSyncSnapshot snapshot(TimePoint now = Clock::now()) const
    {
        AvSyncSnapshot result;
        result.hasVideo = hasVideo_;
        result.hasVideoSenderClock = hasVideoSenderClock_;
        result.hasAudio = hasAudio_;
        result.videoAgeMs = ObservationAgeMs(hasVideo_, latestVideoObservedAt_, now);
        result.audioAgeMs = ObservationAgeMs(hasAudio_, latestAudioObservedAt_, now);
        result.videoFresh = hasVideo_ && result.videoAgeMs <=
            static_cast<uint64_t>(AvSyncMediaFreshness.count());
        result.audioFresh = hasAudio_ && result.audioAgeMs <=
            static_cast<uint64_t>(AvSyncMediaFreshness.count());
        result.ready = result.videoFresh && result.audioFresh;
        result.senderClockReady =
            hasVideoSenderClock_ && result.videoFresh && result.audioFresh;
        result.videoFrames = videoFrames_;
        result.audioPackets = audioPackets_;
        result.ignoredVideoFrames = ignoredVideoFrames_;
        result.ignoredAudioPackets = ignoredAudioPackets_;
        result.firstVideoTimestamp100ns = firstVideoTimestamp100ns_;
        result.latestVideoTimestamp100ns = latestVideoTimestamp100ns_;
        result.firstVideoSenderQpc100ns = firstVideoSenderQpc100ns_;
        result.latestVideoSenderQpc100ns = latestVideoSenderQpc100ns_;
        result.firstAudioQpc100ns = firstAudioQpc100ns_;
        result.latestAudioQpc100ns = latestAudioQpc100ns_;
        result.latestVideoFrameId = latestVideoFrameId_;
        result.latestAudioPacketId = latestAudioPacketId_;

        if (hasVideo_ && hasAudio_) {
            const uint64_t videoElapsed100ns =
                latestVideoTimestamp100ns_ >= firstVideoTimestamp100ns_
                    ? latestVideoTimestamp100ns_ - firstVideoTimestamp100ns_
                    : 0;
            const uint64_t audioElapsed100ns =
                latestAudioQpc100ns_ >= firstAudioQpc100ns_
                    ? latestAudioQpc100ns_ - firstAudioQpc100ns_
                    : 0;
            result.videoElapsedMs = Ticks100nsToMs(videoElapsed100ns);
            result.audioElapsedMs = Ticks100nsToMs(audioElapsed100ns);
            result.audioAheadMs = result.audioElapsedMs - result.videoElapsedMs;
        }
        if (hasVideoSenderClock_ && hasAudio_) {
            result.initialAudioSenderOffsetMs =
                SignedTicks100nsToMs(
                    static_cast<int64_t>(firstAudioQpc100ns_) -
                    static_cast<int64_t>(firstVideoSenderQpc100ns_));
            result.senderClockAudioAheadMs =
                SignedTicks100nsToMs(
                    static_cast<int64_t>(latestAudioQpc100ns_) -
                    static_cast<int64_t>(latestVideoSenderQpc100ns_));
            result.senderTimelineAudioAheadMs =
                SignedTicks100nsToMs(
                    static_cast<int64_t>(latestAudioQpc100ns_) -
                    static_cast<int64_t>(firstVideoSenderQpc100ns_) -
                    (static_cast<int64_t>(latestVideoTimestamp100ns_) -
                     static_cast<int64_t>(firstVideoTimestamp100ns_)));
        }

        return result;
    }

private:
    static uint64_t ObservationAgeMs(bool observed, TimePoint then, TimePoint now) noexcept
    {
        if (!observed || now <= then) {
            return 0;
        }
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - then).count());
    }

    static double Ticks100nsToMs(uint64_t ticks) noexcept
    {
        return static_cast<double>(ticks) / 10'000.0;
    }

    static double SignedTicks100nsToMs(int64_t ticks) noexcept
    {
        return static_cast<double>(ticks) / 10'000.0;
    }

    bool hasVideo_ = false;
    bool hasVideoSenderClock_ = false;
    bool hasAudio_ = false;
    uint64_t videoFrames_ = 0;
    uint64_t audioPackets_ = 0;
    uint64_t ignoredVideoFrames_ = 0;
    uint64_t ignoredAudioPackets_ = 0;
    uint64_t firstVideoTimestamp100ns_ = 0;
    uint64_t latestVideoTimestamp100ns_ = 0;
    uint64_t firstVideoSenderQpc100ns_ = 0;
    uint64_t latestVideoSenderQpc100ns_ = 0;
    uint64_t firstAudioQpc100ns_ = 0;
    uint64_t latestAudioQpc100ns_ = 0;
    uint64_t latestVideoFrameId_ = 0;
    uint64_t latestAudioPacketId_ = 0;
    TimePoint latestVideoObservedAt_{};
    TimePoint latestAudioObservedAt_{};
};

} // namespace screenshare_runtime_internal
