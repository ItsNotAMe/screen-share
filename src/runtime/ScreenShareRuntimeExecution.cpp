#include "runtime/ScreenShareRuntimeInternal.h"

#include "audio/OpusCodec.h"
#include "audio/WasapiCapture.h"
#include "audio/WasapiRenderer.h"
#include "capture/DesktopCapturer.h"
#include "codec/H264Bitstream.h"
#include "codec/H264FileEncoder.h"
#include "codec/H264StreamDecoder.h"
#include "codec/H264StreamEncoder.h"
#include "core/SessionRuntimeControl.h"
#include "render/ReceiverPreviewWindow.h"
#include "transport/NatTraversal.h"
#include "transport/SignalingClient.h"
#include "transport/StunClient.h"
#include "transport/UdpReceiver.h"
#include "transport/UdpSender.h"
#include "video/Nv12Convert.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace screenshare_runtime_internal;

void WriteBgraBmp(const std::filesystem::path& path, const uint8_t* pixels, int width, int height, uint32_t stride)
{
    if (pixels == nullptr) {
        throw std::runtime_error("BMP pixels are missing");
    }
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("BMP frame dimensions are not available");
    }

    const uint32_t outputStride = static_cast<uint32_t>(width) * 4;
    if (stride < outputStride) {
        throw std::runtime_error("BMP frame stride is too small");
    }

    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to open BMP file: " + path.string());
    }

    auto writeU16 = [&](uint16_t value) {
        const char bytes[2] = {
            static_cast<char>(value & 0xFF),
            static_cast<char>((value >> 8) & 0xFF),
        };
        output.write(bytes, sizeof(bytes));
    };
    auto writeU32 = [&](uint32_t value) {
        const char bytes[4] = {
            static_cast<char>(value & 0xFF),
            static_cast<char>((value >> 8) & 0xFF),
            static_cast<char>((value >> 16) & 0xFF),
            static_cast<char>((value >> 24) & 0xFF),
        };
        output.write(bytes, sizeof(bytes));
    };
    auto writeI32 = [&](int32_t value) {
        writeU32(static_cast<uint32_t>(value));
    };

    constexpr uint32_t fileHeaderBytes = 14;
    constexpr uint32_t infoHeaderBytes = 40;
    constexpr uint32_t pixelOffset = fileHeaderBytes + infoHeaderBytes;
    const uint64_t pixelBytes64 = static_cast<uint64_t>(outputStride) * static_cast<uint64_t>(height);
    if (pixelBytes64 > std::numeric_limits<uint32_t>::max() - pixelOffset) {
        throw std::runtime_error("BMP frame is too large to write");
    }

    const uint32_t pixelBytes = static_cast<uint32_t>(pixelBytes64);
    const uint32_t fileBytes = pixelOffset + pixelBytes;

    writeU16(0x4D42);
    writeU32(fileBytes);
    writeU16(0);
    writeU16(0);
    writeU32(pixelOffset);

    writeU32(infoHeaderBytes);
    writeI32(width);
    writeI32(-height);
    writeU16(1);
    writeU16(32);
    writeU32(0);
    writeU32(pixelBytes);
    writeI32(2835);
    writeI32(2835);
    writeU32(0);
    writeU32(0);

    for (int y = 0; y < height; ++y) {
        output.write(
            reinterpret_cast<const char*>(pixels + static_cast<size_t>(stride) * y),
            outputStride);
    }
    if (!output) {
        throw std::runtime_error("Failed to write BMP file: " + path.string());
    }
}

void WriteDecodedFrameBmp(const std::filesystem::path& path, const screenshare::DecodedFrameInfo& frame)
{
    if (frame.width <= 0 || frame.height <= 0) {
        throw std::runtime_error("Decoded frame dimensions are not available for BMP dump");
    }
    if ((frame.width % 2) != 0 || (frame.height % 2) != 0) {
        throw std::runtime_error("Decoded NV12 frame dimensions must be even for BMP dump");
    }

    const auto bgra = screenshare::ConvertNv12ToBgra(frame.data.data(), frame.data.size(), frame.width, frame.height);
    WriteBgraBmp(path, bgra.data(), frame.width, frame.height, static_cast<uint32_t>(frame.width) * 4);
}

void WriteCapturedFrameBmp(const std::filesystem::path& path, const screenshare::CapturedFrame& frame)
{
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) {
        throw std::runtime_error("Captured frame is not available for BMP dump");
    }

    const bool isBgra =
        frame.format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        frame.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        frame.format == DXGI_FORMAT_B8G8R8X8_UNORM ||
        frame.format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    if (!isBgra) {
        throw std::runtime_error(
            "Captured BMP dump expects BGRA8, got DXGI format " +
            std::to_string(static_cast<int>(frame.format)));
    }

    WriteBgraBmp(path, reinterpret_cast<const uint8_t*>(frame.pixels.data()), frame.width, frame.height, frame.rowPitch);
}

int EvenDimension(int value)
{
    return std::max(2, value & ~1);
}

int H264AlignedDimension(int value)
{
    return std::max(16, ((value + 8) / 16) * 16);
}

struct ResolutionTier {
    int width = 0;
    int height = 0;
    double scale = 1.0;
};

std::vector<ResolutionTier> BuildResolutionTiers(int baseWidth, int baseHeight, float minScale)
{
    std::vector<ResolutionTier> tiers;
    const std::array<double, 3> candidateScales{0.75, 0.5, 0.375};
    const double clampedMinScale = std::clamp(static_cast<double>(minScale), 0.25, 1.0);
    const int baseTierWidth = EvenDimension(baseWidth);
    const int baseTierHeight = EvenDimension(baseHeight);

    tiers.push_back(ResolutionTier{baseTierWidth, baseTierHeight, 1.0});

    for (const double scale : candidateScales) {
        if (scale + 0.0001 < clampedMinScale) {
            continue;
        }

        ResolutionTier tier;
        tier.width = std::min(baseTierWidth, H264AlignedDimension(static_cast<int>(std::round(static_cast<double>(baseWidth) * scale))));
        tier.height = std::min(baseTierHeight, H264AlignedDimension(static_cast<int>(std::round(static_cast<double>(baseHeight) * scale))));
        tier.scale = scale;
        if (tiers.empty() || tiers.back().width != tier.width || tiers.back().height != tier.height) {
            tiers.push_back(tier);
        }
    }

    return tiers;
}

const char* StreamEncoderPreferenceName(StreamEncoderPreference preference)
{
    switch (preference) {
    case StreamEncoderPreference::Auto:
        return "auto";
    case StreamEncoderPreference::Software:
        return "software";
    case StreamEncoderPreference::Hardware:
        return "hardware";
    default:
        return "unknown";
    }
}

screenshare::udp_protocol::AudioSampleFormat ToUdpAudioSampleFormat(const screenshare::AudioCaptureFormat& format);
screenshare::UdpAudioPacket BuildRawUdpAudioPacket(
    const screenshare::CapturedAudioPacket& packet,
    const screenshare::AudioCaptureFormat& format,
    screenshare::udp_protocol::AudioSampleFormat sampleFormat);
screenshare::UdpAudioPacket BuildOpusUdpAudioPacket(
    const screenshare::CapturedAudioPacket& packet,
    screenshare::OpusAudioEncoder& encoder);

class PreviewPlayoutBuffer {
public:
    using Clock = std::chrono::steady_clock;

    PreviewPlayoutBuffer(std::chrono::milliseconds initialDelay, std::chrono::milliseconds maxLateFrameAge)
        : initialDelay_(initialDelay),
          maxLateFrameAge_(maxLateFrameAge)
    {
    }

    void Enqueue(screenshare::DecodedFrameInfo frame)
    {
        int64_t key = frame.timestamp100ns;
        while (frames_.contains(key)) {
            ++key;
        }

        frames_.emplace(key, QueuedFrame{std::move(frame), Clock::now()});
        while (frames_.size() > maxQueuedFrames_) {
            frames_.erase(frames_.begin());
            ++overflowDrops_;
        }
    }

    void PresentReady(
        screenshare::ReceiverPreviewWindow& previewWindow,
        Clock::time_point now,
        bool flush,
        std::optional<int64_t> maxPresentationTimestamp100ns = std::nullopt)
    {
        if (frames_.empty()) {
            return;
        }
        EnsureClockStarted(now);

        while (!frames_.empty()) {
            const int64_t frameTimestamp100ns = frames_.begin()->first;
            if (!flush &&
                maxPresentationTimestamp100ns &&
                frameTimestamp100ns > *maxPresentationTimestamp100ns) {
                ++syncWaits_;
                break;
            }

            const auto frameTime = PresentationTime(frameTimestamp100ns);
            if (!flush && now < frameTime) {
                break;
            }
            if (!flush && frames_.size() > 1 && now - frameTime > maxLateFrameAge_) {
                frames_.erase(frames_.begin());
                ++lateDrops_;
                continue;
            }

            auto queuedFrame = std::move(frames_.begin()->second);
            frames_.erase(frames_.begin());
            const double presentDelayMs =
                std::chrono::duration<double, std::milli>(now - queuedFrame.enqueuedAt).count();
            if (std::isfinite(presentDelayMs) && presentDelayMs >= 0.0) {
                lastPresentDelayMs_ = presentDelayMs;
                maxPresentDelayMs_ = std::max(maxPresentDelayMs_, presentDelayMs);
                totalPresentDelayMs_ += presentDelayMs;
                ++presentDelaySamples_;
            }
            previewWindow.PresentFrame(queuedFrame.frame);
            lastPresentedTimestamp100ns_ = frameTimestamp100ns;
            hasPresentedTimestamp_ = true;
        }
    }

    void ClearPendingAndRestartClock()
    {
        frames_.clear();
        clockStarted_ = false;
        firstTimestamp100ns_ = 0;
        firstPresentationAt_ = {};
        lastPresentedTimestamp100ns_ = 0;
        hasPresentedTimestamp_ = false;
    }

    [[nodiscard]] std::chrono::milliseconds ReceiveTimeout(Clock::time_point now) const
    {
        constexpr auto defaultTimeout = std::chrono::milliseconds(20);
        if (frames_.empty() || !clockStarted_) {
            return defaultTimeout;
        }

        const auto frameTime = PresentationTime(frames_.begin()->first);
        if (frameTime <= now) {
            return std::chrono::milliseconds(1);
        }

        const auto untilFrame = std::chrono::duration_cast<std::chrono::milliseconds>(frameTime - now);
        return std::clamp(untilFrame, std::chrono::milliseconds(1), defaultTimeout);
    }

    [[nodiscard]] size_t queuedFrameCount() const noexcept { return frames_.size(); }
    [[nodiscard]] uint64_t lateDrops() const noexcept { return lateDrops_; }
    [[nodiscard]] uint64_t overflowDrops() const noexcept { return overflowDrops_; }
    [[nodiscard]] uint64_t syncDrops() const noexcept { return syncDrops_; }
    [[nodiscard]] uint64_t syncWaits() const noexcept { return syncWaits_; }
    [[nodiscard]] bool clockStarted() const noexcept { return clockStarted_; }
    [[nodiscard]] int64_t firstTimestamp100ns() const noexcept { return firstTimestamp100ns_; }
    [[nodiscard]] Clock::time_point firstPresentationAt() const noexcept { return firstPresentationAt_; }
    [[nodiscard]] bool hasPresentedTimestamp() const noexcept { return hasPresentedTimestamp_; }
    [[nodiscard]] int64_t lastPresentedTimestamp100ns() const noexcept { return lastPresentedTimestamp100ns_; }
    [[nodiscard]] std::chrono::milliseconds initialDelay() const noexcept { return initialDelay_; }
    [[nodiscard]] std::chrono::milliseconds maxLateFrameAge() const noexcept { return maxLateFrameAge_; }
    [[nodiscard]] double averagePresentDelayMs() const noexcept
    {
        return presentDelaySamples_ == 0 ? 0.0 : totalPresentDelayMs_ / static_cast<double>(presentDelaySamples_);
    }
    [[nodiscard]] double maxPresentDelayMs() const noexcept { return maxPresentDelayMs_; }
    [[nodiscard]] double lastPresentDelayMs() const noexcept { return lastPresentDelayMs_; }
    void AddInitialDelayBias(std::chrono::milliseconds bias)
    {
        initialDelay_ += bias;
    }
    [[nodiscard]] uint64_t DropBeforeTimestamp(int64_t timestamp100ns)
    {
        uint64_t dropped = 0;
        while (!frames_.empty() && frames_.begin()->first < timestamp100ns) {
            frames_.erase(frames_.begin());
            ++dropped;
        }
        syncDrops_ += dropped;
        if (dropped > 0) {
            clockStarted_ = false;
            firstTimestamp100ns_ = 0;
            firstPresentationAt_ = {};
            lastPresentedTimestamp100ns_ = 0;
            hasPresentedTimestamp_ = false;
        }
        return dropped;
    }

private:
    void EnsureClockStarted(Clock::time_point now)
    {
        if (clockStarted_ || frames_.empty()) {
            return;
        }

        firstTimestamp100ns_ = frames_.begin()->first;
        firstPresentationAt_ = now + initialDelay_;
        clockStarted_ = true;
    }

    [[nodiscard]] Clock::time_point PresentationTime(int64_t timestamp100ns) const
    {
        if (timestamp100ns <= firstTimestamp100ns_) {
            return firstPresentationAt_;
        }

        const auto deltaTicks = static_cast<uint64_t>(timestamp100ns - firstTimestamp100ns_);
        constexpr auto maxDeltaTicks =
            static_cast<uint64_t>(std::numeric_limits<std::chrono::nanoseconds::rep>::max() / 100);
        const auto delta = std::chrono::nanoseconds(std::min(deltaTicks, maxDeltaTicks) * 100);
        return firstPresentationAt_ + std::chrono::duration_cast<Clock::duration>(delta);
    }

    struct QueuedFrame {
        screenshare::DecodedFrameInfo frame;
        Clock::time_point enqueuedAt;
    };

    std::map<int64_t, QueuedFrame> frames_;
    Clock::time_point firstPresentationAt_{};
    int64_t firstTimestamp100ns_ = 0;
    int64_t lastPresentedTimestamp100ns_ = 0;
    bool clockStarted_ = false;
    bool hasPresentedTimestamp_ = false;
    uint64_t lateDrops_ = 0;
    uint64_t overflowDrops_ = 0;
    uint64_t syncDrops_ = 0;
    uint64_t syncWaits_ = 0;
    uint64_t presentDelaySamples_ = 0;
    double totalPresentDelayMs_ = 0.0;
    double maxPresentDelayMs_ = 0.0;
    double lastPresentDelayMs_ = 0.0;
    size_t maxQueuedFrames_ = 180;
    std::chrono::milliseconds initialDelay_;
    std::chrono::milliseconds maxLateFrameAge_;
};

class AudioPlayoutBuffer {
public:
    explicit AudioPlayoutBuffer(std::chrono::milliseconds targetLatency)
        : targetLatency_(targetLatency)
    {
    }

    void Clear()
    {
        packets_.clear();
        nextPacketId_.reset();
        started_ = false;
        lastRenderedEndQpc100ns_ = 0;
        hasRenderedQpc_ = false;
    }

    void Enqueue(screenshare::UdpCompletedAudioPacket packet)
    {
        if (nextPacketId_ && packet.packetId < *nextPacketId_) {
            ++lateDrops_;
            return;
        }
        if (!packets_.emplace(packet.packetId, std::move(packet)).second) {
            ++duplicateDrops_;
            return;
        }
        while (packets_.size() > maxQueuedPackets_) {
            packets_.erase(packets_.begin());
            ++overflowDrops_;
        }
    }

    void RenderReady(
        screenshare::WasapiRenderer& renderer,
        std::optional<uint64_t> maxAudioQpcPosition = std::nullopt)
    {
        if (packets_.empty()) {
            return;
        }
        const bool syncGated = maxAudioQpcPosition.has_value();
        if (!started_) {
            if (QueuedDuration() < targetLatency_) {
                return;
            }
            nextPacketId_ = packets_.begin()->first;
            started_ = true;
            if (!syncGated) {
                TrimLatencyBacklog();
            }
        }
        if (!syncGated) {
            TrimLatencyBacklog();
        }

        while (!packets_.empty() && nextPacketId_) {
            auto next = packets_.find(*nextPacketId_);
            if (next == packets_.end()) {
                const uint64_t firstQueuedPacketId = packets_.begin()->first;
                if (firstQueuedPacketId > *nextPacketId_ && QueuedDuration() >= targetLatency_) {
                    missingPacketsSkipped_ += firstQueuedPacketId - *nextPacketId_;
                    nextPacketId_ = firstQueuedPacketId;
                    continue;
                }
                return;
            }

            auto& packet = next->second;
            if (maxAudioQpcPosition &&
                packet.qpcPosition != 0 &&
                packet.qpcPosition > *maxAudioQpcPosition) {
                ++syncWaits_;
                return;
            }
            if (packet.audioFrames > renderer.bufferFrames()) {
                ++oversizedDrops_;
                ++(*nextPacketId_);
                packets_.erase(next);
                continue;
            }

            const bool silent = (packet.flags & screenshare::udp_protocol::AudioPacketFlagSilent) != 0;
            if (!renderer.RenderPacket(std::span<const std::byte>(packet.bytes.data(), packet.bytes.size()), packet.audioFrames, silent)) {
                ++renderBackpressure_;
                return;
            }

            ++packetsRendered_;
            framesRendered_ += packet.audioFrames;
            if (packet.qpcPosition != 0 && packet.sampleRate != 0) {
                const uint64_t packetDuration100ns =
                    static_cast<uint64_t>(packet.audioFrames) * 10'000'000ULL /
                    static_cast<uint64_t>(packet.sampleRate);
                lastRenderedEndQpc100ns_ = packet.qpcPosition + packetDuration100ns;
                hasRenderedQpc_ = true;
            }
            ++(*nextPacketId_);
            packets_.erase(next);
        }
    }

    [[nodiscard]] size_t queuedPacketCount() const noexcept { return packets_.size(); }
    [[nodiscard]] uint64_t packetsRendered() const noexcept { return packetsRendered_; }
    [[nodiscard]] uint64_t framesRendered() const noexcept { return framesRendered_; }
    [[nodiscard]] uint64_t lateDrops() const noexcept { return lateDrops_; }
    [[nodiscard]] uint64_t duplicateDrops() const noexcept { return duplicateDrops_; }
    [[nodiscard]] uint64_t overflowDrops() const noexcept { return overflowDrops_; }
    [[nodiscard]] uint64_t oversizedDrops() const noexcept { return oversizedDrops_; }
    [[nodiscard]] uint64_t latencyDrops() const noexcept { return latencyDrops_; }
    [[nodiscard]] uint64_t syncDrops() const noexcept { return syncDrops_; }
    [[nodiscard]] uint64_t syncWaits() const noexcept { return syncWaits_; }
    [[nodiscard]] uint64_t missingPacketsSkipped() const noexcept { return missingPacketsSkipped_; }
    [[nodiscard]] uint64_t renderBackpressure() const noexcept { return renderBackpressure_; }
    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] bool hasRenderedQpc() const noexcept { return hasRenderedQpc_; }
    [[nodiscard]] uint64_t lastRenderedEndQpc100ns() const noexcept { return lastRenderedEndQpc100ns_; }
    [[nodiscard]] std::chrono::milliseconds targetLatency() const noexcept { return targetLatency_; }
    void AddTargetLatencyBias(std::chrono::milliseconds bias)
    {
        targetLatency_ += bias;
    }
    [[nodiscard]] uint64_t DropBeforeQpc(uint64_t qpcPosition)
    {
        uint64_t dropped = 0;
        while (!packets_.empty() &&
               packets_.begin()->second.qpcPosition != 0 &&
               packets_.begin()->second.qpcPosition < qpcPosition) {
            const uint64_t packetId = packets_.begin()->first;
            if (nextPacketId_ && packetId >= *nextPacketId_) {
                nextPacketId_ = packetId + 1;
            }
            packets_.erase(packets_.begin());
            ++dropped;
        }
        syncDrops_ += dropped;
        return dropped;
    }

    [[nodiscard]] std::chrono::milliseconds QueuedDuration() const
    {
        if (packets_.empty()) {
            return std::chrono::milliseconds(0);
        }

        uint64_t frames = 0;
        uint32_t sampleRate = 0;
        for (const auto& [packetId, packet] : packets_) {
            static_cast<void>(packetId);
            frames += packet.audioFrames;
            sampleRate = packet.sampleRate;
        }
        if (sampleRate == 0) {
            return std::chrono::milliseconds(0);
        }

        return std::chrono::milliseconds(static_cast<int64_t>(frames * 1000 / sampleRate));
    }

private:
    void TrimLatencyBacklog()
    {
        if (!started_ || packets_.empty()) {
            return;
        }

        const auto trimThreshold = targetLatency_ + maxQueueOverTarget_;
        if (QueuedDuration() <= trimThreshold) {
            return;
        }

        while (!packets_.empty() && QueuedDuration() > targetLatency_) {
            auto oldest = packets_.begin();
            if (nextPacketId_) {
                if (oldest->first > *nextPacketId_) {
                    missingPacketsSkipped_ += oldest->first - *nextPacketId_;
                }
                if (oldest->first >= *nextPacketId_) {
                    nextPacketId_ = oldest->first + 1;
                }
            }
            packets_.erase(oldest);
            ++latencyDrops_;
        }
    }

    std::map<uint64_t, screenshare::UdpCompletedAudioPacket> packets_;
    std::optional<uint64_t> nextPacketId_;
    bool started_ = false;
    bool hasRenderedQpc_ = false;
    size_t maxQueuedPackets_ = 512;
    std::chrono::milliseconds targetLatency_;
    std::chrono::milliseconds maxQueueOverTarget_{250};
    uint64_t packetsRendered_ = 0;
    uint64_t framesRendered_ = 0;
    uint64_t lateDrops_ = 0;
    uint64_t duplicateDrops_ = 0;
    uint64_t overflowDrops_ = 0;
    uint64_t oversizedDrops_ = 0;
    uint64_t latencyDrops_ = 0;
    uint64_t syncDrops_ = 0;
    uint64_t syncWaits_ = 0;
    uint64_t missingPacketsSkipped_ = 0;
    uint64_t renderBackpressure_ = 0;
    uint64_t lastRenderedEndQpc100ns_ = 0;
};

struct AvSyncSnapshot {
    bool hasVideo = false;
    bool hasVideoSenderClock = false;
    bool hasAudio = false;
    bool ready = false;
    bool senderClockReady = false;
    uint64_t videoFrames = 0;
    uint64_t audioPackets = 0;
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
    void Clear()
    {
        hasVideo_ = false;
        hasVideoSenderClock_ = false;
        hasAudio_ = false;
        videoFrames_ = 0;
        audioPackets_ = 0;
        ignoredAudioPackets_ = 0;
        firstVideoTimestamp100ns_ = 0;
        latestVideoTimestamp100ns_ = 0;
        firstVideoSenderQpc100ns_ = 0;
        latestVideoSenderQpc100ns_ = 0;
        firstAudioQpc100ns_ = 0;
        latestAudioQpc100ns_ = 0;
        latestVideoFrameId_ = 0;
        latestAudioPacketId_ = 0;
    }

    void ObserveVideoFrame(const screenshare::UdpCompletedFrame& frame)
    {
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
        ++videoFrames_;
    }

    void ObserveAudioPacket(const screenshare::UdpCompletedAudioPacket& packet)
    {
        const bool timestampError =
            (packet.flags & screenshare::udp_protocol::AudioPacketFlagTimestampError) != 0;
        if (timestampError || packet.qpcPosition == 0) {
            ++ignoredAudioPackets_;
            return;
        }

        if (!hasAudio_) {
            firstAudioQpc100ns_ = packet.qpcPosition;
            hasAudio_ = true;
        }

        latestAudioQpc100ns_ = packet.qpcPosition;
        latestAudioPacketId_ = packet.packetId;
        ++audioPackets_;
    }

    [[nodiscard]] AvSyncSnapshot snapshot() const
    {
        AvSyncSnapshot snapshot;
        snapshot.hasVideo = hasVideo_;
        snapshot.hasVideoSenderClock = hasVideoSenderClock_;
        snapshot.hasAudio = hasAudio_;
        snapshot.ready = hasVideo_ && hasAudio_;
        snapshot.senderClockReady = hasVideoSenderClock_ && hasAudio_;
        snapshot.videoFrames = videoFrames_;
        snapshot.audioPackets = audioPackets_;
        snapshot.ignoredAudioPackets = ignoredAudioPackets_;
        snapshot.firstVideoTimestamp100ns = firstVideoTimestamp100ns_;
        snapshot.latestVideoTimestamp100ns = latestVideoTimestamp100ns_;
        snapshot.firstVideoSenderQpc100ns = firstVideoSenderQpc100ns_;
        snapshot.latestVideoSenderQpc100ns = latestVideoSenderQpc100ns_;
        snapshot.firstAudioQpc100ns = firstAudioQpc100ns_;
        snapshot.latestAudioQpc100ns = latestAudioQpc100ns_;
        snapshot.latestVideoFrameId = latestVideoFrameId_;
        snapshot.latestAudioPacketId = latestAudioPacketId_;

        if (snapshot.ready) {
            const uint64_t videoElapsed100ns =
                latestVideoTimestamp100ns_ >= firstVideoTimestamp100ns_
                    ? latestVideoTimestamp100ns_ - firstVideoTimestamp100ns_
                    : 0;
            const uint64_t audioElapsed100ns =
                latestAudioQpc100ns_ >= firstAudioQpc100ns_
                    ? latestAudioQpc100ns_ - firstAudioQpc100ns_
                    : 0;
            snapshot.videoElapsedMs = Ticks100nsToMs(videoElapsed100ns);
            snapshot.audioElapsedMs = Ticks100nsToMs(audioElapsed100ns);
            snapshot.audioAheadMs = snapshot.audioElapsedMs - snapshot.videoElapsedMs;
        }
        if (snapshot.senderClockReady) {
            snapshot.initialAudioSenderOffsetMs =
                SignedTicks100nsToMs(
                    static_cast<int64_t>(firstAudioQpc100ns_) -
                    static_cast<int64_t>(firstVideoSenderQpc100ns_));
            snapshot.senderClockAudioAheadMs =
                SignedTicks100nsToMs(
                    static_cast<int64_t>(latestAudioQpc100ns_) -
                    static_cast<int64_t>(latestVideoSenderQpc100ns_));
            snapshot.senderTimelineAudioAheadMs =
                SignedTicks100nsToMs(
                    static_cast<int64_t>(latestAudioQpc100ns_) -
                    static_cast<int64_t>(firstVideoSenderQpc100ns_) -
                    (static_cast<int64_t>(latestVideoTimestamp100ns_) -
                     static_cast<int64_t>(firstVideoTimestamp100ns_)));
        }

        return snapshot;
    }

private:
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
    uint64_t ignoredAudioPackets_ = 0;
    uint64_t firstVideoTimestamp100ns_ = 0;
    uint64_t latestVideoTimestamp100ns_ = 0;
    uint64_t firstVideoSenderQpc100ns_ = 0;
    uint64_t latestVideoSenderQpc100ns_ = 0;
    uint64_t firstAudioQpc100ns_ = 0;
    uint64_t latestAudioQpc100ns_ = 0;
    uint64_t latestVideoFrameId_ = 0;
    uint64_t latestAudioPacketId_ = 0;
};

struct AudioCaptureWorkerStats {
    uint64_t packets = 0;
    uint64_t frames = 0;
    uint64_t bytes = 0;
    uint64_t silentPackets = 0;
    uint64_t discontinuities = 0;
    uint64_t timestampErrors = 0;
    uint64_t emptyPolls = 0;
    uint64_t payloadBitrate = 0;
    uint64_t latestDevicePosition = 0;
    uint64_t latestQpcPosition = 0;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    uint16_t blockAlign = 0;
    screenshare::udp_protocol::AudioSampleFormat sampleFormat =
        screenshare::udp_protocol::AudioSampleFormat::Unknown;
    screenshare::udp_protocol::AudioCodec codec = screenshare::udp_protocol::AudioCodec::Raw;
    bool started = false;
    std::string deviceName;
    screenshare::AudioCaptureSource source = screenshare::AudioCaptureSource::SystemOutput;
    bool unavailable = false;
    std::string unavailableReason;
};

class AudioUdpCaptureWorker {
public:
    using SendAudioPacketFn = std::function<void(const screenshare::UdpAudioPacket&)>;

    AudioUdpCaptureWorker() = default;

    ~AudioUdpCaptureWorker()
    {
        Stop();
    }

    AudioUdpCaptureWorker(const AudioUdpCaptureWorker&) = delete;
    AudioUdpCaptureWorker& operator=(const AudioUdpCaptureWorker&) = delete;

    void Start(
        SendAudioPacketFn sendAudioPacket,
        screenshare::AudioCaptureSource source,
        const std::string& deviceId,
        uint32_t processId,
        screenshare::udp_protocol::AudioCodec codec)
    {
        Stop();
        stopRequested_ = false;
        {
            std::lock_guard lock(mutex_);
            stats_ = {};
            stats_.codec = codec;
            stats_.source = source;
            error_.clear();
        }

        thread_ = std::thread([this, sendAudioPacket = std::move(sendAudioPacket), source, deviceId, processId, codec] {
            Run(sendAudioPacket, source, deviceId, processId, codec);
        });
    }

    void Stop()
    {
        stopRequested_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] AudioCaptureWorkerStats stats() const
    {
        std::lock_guard lock(mutex_);
        return stats_;
    }

    void ThrowIfFailed() const
    {
        std::lock_guard lock(mutex_);
        if (!error_.empty()) {
            throw std::runtime_error("Audio capture worker failed: " + error_);
        }
    }

private:
    void Run(
        const SendAudioPacketFn& sendAudioPacket,
        screenshare::AudioCaptureSource source,
        const std::string& deviceId,
        uint32_t processId,
        screenshare::udp_protocol::AudioCodec codec)
    {
        try {
            screenshare::WasapiCapture capture;
            screenshare::AudioCaptureConfig config;
            config.source = source;
            config.processId = processId;
            if (!deviceId.empty()) {
                config.deviceId = screenshare::Widen(deviceId);
            }
            try {
                capture.Start(config);
            } catch (const std::exception& error) {
                if (source != screenshare::AudioCaptureSource::ProcessOutput) {
                    throw;
                }
                std::cout
                    << "audio_capture_unavailable=process reason=process_loopback_failed detail=\""
                    << error.what()
                    << "\"\n";
                std::lock_guard lock(mutex_);
                stats_.source = source;
                stats_.unavailable = true;
                stats_.unavailableReason = error.what();
                return;
            }

            const auto format = capture.format();
            const auto udpSampleFormat = ToUdpAudioSampleFormat(format);
            if (udpSampleFormat == screenshare::udp_protocol::AudioSampleFormat::Unknown) {
                throw std::runtime_error("Unsupported WASAPI sample format for combined audio/video stream: " + format.sampleFormat);
            }
            std::unique_ptr<screenshare::OpusAudioEncoder> opusEncoder;
            if (codec == screenshare::udp_protocol::AudioCodec::Opus) {
                opusEncoder = std::make_unique<screenshare::OpusAudioEncoder>();
                opusEncoder->Start(format);
            }

            {
                std::lock_guard lock(mutex_);
                stats_.started = true;
                stats_.deviceName = screenshare::Narrow(capture.deviceName());
                stats_.source = source;
                stats_.sampleRate = format.sampleRate;
                stats_.channels = format.channels;
                stats_.bitsPerSample = format.bitsPerSample;
                stats_.blockAlign = format.blockAlign;
                stats_.sampleFormat = udpSampleFormat;
                stats_.codec = codec;
                stats_.payloadBitrate =
                    codec == screenshare::udp_protocol::AudioCodec::Opus ?
                        screenshare::OpusAudioEncoder::DefaultBitrate :
                        static_cast<uint64_t>(format.sampleRate) *
                        static_cast<uint64_t>(format.blockAlign) *
                        8ULL;
            }

            while (!stopRequested_) {
                auto packet = capture.CapturePacket(std::chrono::milliseconds(50));
                if (!packet) {
                    std::lock_guard lock(mutex_);
                    ++stats_.emptyPolls;
                    continue;
                }

                screenshare::UdpAudioPacket audioPacket =
                    codec == screenshare::udp_protocol::AudioCodec::Opus ?
                        BuildOpusUdpAudioPacket(*packet, *opusEncoder) :
                        BuildRawUdpAudioPacket(*packet, format, udpSampleFormat);
                sendAudioPacket(audioPacket);

                std::lock_guard lock(mutex_);
                ++stats_.packets;
                stats_.frames += packet->frames;
                stats_.bytes += packet->data.size();
                if (packet->silent) {
                    ++stats_.silentPackets;
                }
                if (packet->dataDiscontinuity) {
                    ++stats_.discontinuities;
                }
                if (packet->timestampError) {
                    ++stats_.timestampErrors;
                }
                stats_.latestDevicePosition = packet->devicePosition;
                stats_.latestQpcPosition = packet->qpcPosition;
            }

            capture.Stop();
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex_);
            error_ = error.what();
        }
    }

    mutable std::mutex mutex_;
    AudioCaptureWorkerStats stats_{};
    std::string error_;
    std::thread thread_;
    std::atomic_bool stopRequested_{false};
};

struct ReceiverHealthSnapshot {
    uint64_t completedFrames = 0;
    double completedFps = 0.0;
    size_t pendingFrames = 0;
    size_t pendingDecodePackets = 0;
    size_t previewQueuedFrames = 0;
    uint64_t simulatedDroppedDatagrams = 0;
    uint64_t invalidDatagrams = 0;
    uint64_t incompleteDroppedFrames = 0;
    uint64_t h264DecodeResyncs = 0;
    uint64_t h264DecodeSkippedPackets = 0;
    uint64_t previewFramesPresented = 0;
    uint64_t previewLateDrops = 0;
    uint64_t previewOverflowDrops = 0;
    int decodedWidth = 0;
    int decodedHeight = 0;
    uint64_t previewPlayoutResets = 0;
    int previewLatencyMs = 0;
    int previewMaxLateMs = 0;
    uint64_t recentDroppedDatagrams = 0;
    uint64_t recentInvalidDatagrams = 0;
    uint64_t recentIncompleteFramesDropped = 0;
    uint64_t recentDecodeResyncs = 0;
    uint64_t recentDecodeSkippedPackets = 0;
    uint64_t recentPreviewLateDrops = 0;
    uint64_t recentPreviewOverflowDrops = 0;
};

screenshare::udp_protocol::FeedbackHealthState ReceiverFeedbackHealthState(const ReceiverHealthSnapshot& health);

const char* ReceiverHealthState(const ReceiverHealthSnapshot& health)
{
    return screenshare::udp_protocol::FeedbackHealthStateName(ReceiverFeedbackHealthState(health));
}

screenshare::udp_protocol::FeedbackHealthState ReceiverFeedbackHealthState(const ReceiverHealthSnapshot& health)
{
    if (health.completedFrames == 0) {
        return screenshare::udp_protocol::FeedbackHealthState::Waiting;
    }
    if (health.recentPreviewLateDrops > 0 || health.recentPreviewOverflowDrops > 0) {
        return screenshare::udp_protocol::FeedbackHealthState::PreviewDrop;
    }
    if (health.recentDecodeResyncs > 0 || health.recentDecodeSkippedPackets > 0) {
        return screenshare::udp_protocol::FeedbackHealthState::Recovering;
    }
    if (health.recentDroppedDatagrams > 0 ||
        health.recentInvalidDatagrams > 0 ||
        health.recentIncompleteFramesDropped > 0) {
        return screenshare::udp_protocol::FeedbackHealthState::Loss;
    }
    if (health.pendingFrames >= ReceiverHealthPendingFrameWarning ||
        health.pendingDecodePackets >= OrderedReceiverRecoveryThresholdFrames) {
        return screenshare::udp_protocol::FeedbackHealthState::Buffering;
    }

    return screenshare::udp_protocol::FeedbackHealthState::Ok;
}

uint32_t SaturateUint32(uint64_t value)
{
    return value > std::numeric_limits<uint32_t>::max() ?
        std::numeric_limits<uint32_t>::max() :
        static_cast<uint32_t>(value);
}

screenshare::udp_protocol::FeedbackSnapshot BuildReceiverFeedbackSnapshot(
    const ReceiverHealthSnapshot& health,
    uint64_t sequence,
    uint64_t sessionFingerprint,
    uint64_t accessCodeFingerprint)
{
    screenshare::udp_protocol::FeedbackSnapshot feedback;
    feedback.healthState = ReceiverFeedbackHealthState(health);
    feedback.sequence = sequence;
    feedback.sessionFingerprint = sessionFingerprint;
    feedback.accessCodeFingerprint = accessCodeFingerprint;
    feedback.completedFrames = health.completedFrames;
    feedback.droppedDatagrams = health.simulatedDroppedDatagrams;
    feedback.invalidDatagrams = health.invalidDatagrams;
    feedback.incompleteFramesDropped = health.incompleteDroppedFrames;
    feedback.decodeResyncs = health.h264DecodeResyncs;
    feedback.decodeSkippedPackets = health.h264DecodeSkippedPackets;
    feedback.previewLateDrops = health.previewLateDrops;
    feedback.previewOverflowDrops = health.previewOverflowDrops;
    feedback.pendingFrames = SaturateUint32(health.pendingFrames);
    feedback.pendingDecodePackets = SaturateUint32(health.pendingDecodePackets);
    feedback.previewQueuedFrames = SaturateUint32(health.previewQueuedFrames);
    return feedback;
}

std::string FeedbackSessionText(const screenshare::UdpSenderStats& stats)
{
    if (!stats.hasFeedback || stats.latestFeedback.sessionFingerprint == 0) {
        return "none";
    }
    return FormatSessionFingerprint(stats.latestFeedback.sessionFingerprint);
}

const char* FeedbackAccessText(const screenshare::UdpSenderStats& stats)
{
    if (!stats.hasFeedback) {
        return "unknown";
    }
    return stats.latestFeedback.accessCodeFingerprint == 0 ? "none" : "required";
}

const char* SenderNatStatus(const Options& options, const screenshare::UdpSenderStats& stats)
{
    if (!HasNatShareTarget(options)) {
        return "none";
    }
    if (stats.feedbackPacketsReceived > 0) {
        return "connected";
    }
    if (options.inviteEndpointPreference != InviteEndpointPreference::Auto) {
        return "forced_endpoint_waiting_for_feedback";
    }
    if (stats.natProbeRetargetActive) {
        return "retargeted_waiting_for_feedback";
    }
    if (stats.natProbeRetargetRejected > 0) {
        return "probe_rejected";
    }
    if (stats.natProbePacketsReceived > 0) {
        return "probe_seen";
    }
    if (stats.datagramsSent >= SenderDirectUdpBlockedDatagrams) {
        return "direct_udp_blocked";
    }
    if (stats.datagramsSent > 0) {
        return "waiting_for_probe";
    }
    return "starting";
}

const char* SenderNatHint(const Options& options, const screenshare::UdpSenderStats& stats)
{
    const std::string_view status = SenderNatStatus(options, stats);
    if (status == "none") {
        return "none";
    }
    if (status == "connected") {
        return "receiver_feedback_received";
    }
    if (status == "forced_endpoint_waiting_for_feedback") {
        return "check_receiver_or_try_auto_endpoint";
    }
    if (status == "retargeted_waiting_for_feedback") {
        return "probe_seen_waiting_for_receiver_feedback";
    }
    if (status == "probe_rejected") {
        return "check_access_code_or_session";
    }
    if (status == "probe_seen") {
        return "probe_seen_no_endpoint_change";
    }
    if (status == "direct_udp_blocked") {
        return "signaling_ok_but_no_udp_path_try_tailscale_or_relay";
    }
    if (status == "waiting_for_probe") {
        return "start_watch_with_peer_invite_or_check_firewall";
    }
    return "starting_udp_sender";
}

const char* ReceiverNatStatus(
    bool hasPeerInvite,
    const screenshare::UdpReceiverStats& stats,
    bool hasReceivedStreamTraffic)
{
    if (!hasPeerInvite) {
        return "none";
    }
    if (stats.datagramsAccepted > 0 ||
        stats.framesCompleted > 0 ||
        stats.audioPacketsCompleted > 0) {
        return "receiving";
    }
    if (stats.accessRejectedDatagrams > 0 || stats.cryptoRejectedDatagrams > 0) {
        return "media_rejected";
    }
    if (hasReceivedStreamTraffic || stats.datagramsReceived > 0) {
        return "incoming_unaccepted";
    }
    if (stats.natProbeSendErrors > 0) {
        return "probe_send_errors";
    }
    if (stats.natProbePublicPacketsSent + stats.natProbeLocalPacketsSent >= ReceiverDirectUdpBlockedNatProbes) {
        return "direct_udp_blocked";
    }
    if (stats.natProbePublicPacketsSent > 0 || stats.natProbeLocalPacketsSent > 0) {
        return "probing";
    }
    return "waiting_to_probe";
}

const char* ReceiverNatHint(
    bool hasPeerInvite,
    const screenshare::UdpReceiverStats& stats,
    bool hasReceivedStreamTraffic)
{
    const std::string_view status = ReceiverNatStatus(hasPeerInvite, stats, hasReceivedStreamTraffic);
    if (status == "none") {
        return "none";
    }
    if (status == "receiving") {
        return "media_received";
    }
    if (status == "media_rejected") {
        return "check_access_code_or_plaintext_mode";
    }
    if (status == "incoming_unaccepted") {
        return "check_sender_target_or_packet_format";
    }
    if (status == "probe_send_errors") {
        return "check_peer_invite_endpoint";
    }
    if (status == "direct_udp_blocked") {
        return "signaling_ok_but_no_udp_path_try_tailscale_or_relay";
    }
    if (status == "probing") {
        return "start_share_or_wait_for_signaling_peer";
    }
    return "waiting_to_send_first_probe";
}

bool FeedbackLooksWorse(
    const screenshare::udp_protocol::FeedbackSnapshot& candidate,
    const screenshare::udp_protocol::FeedbackSnapshot& current)
{
    const auto severity = [](screenshare::udp_protocol::FeedbackHealthState state) {
        switch (state) {
        case screenshare::udp_protocol::FeedbackHealthState::PreviewDrop:
            return 6;
        case screenshare::udp_protocol::FeedbackHealthState::Recovering:
            return 5;
        case screenshare::udp_protocol::FeedbackHealthState::Loss:
            return 4;
        case screenshare::udp_protocol::FeedbackHealthState::Buffering:
            return 3;
        case screenshare::udp_protocol::FeedbackHealthState::Waiting:
            return 2;
        case screenshare::udp_protocol::FeedbackHealthState::Unknown:
            return 1;
        case screenshare::udp_protocol::FeedbackHealthState::Ok:
            return 0;
        }
        return 1;
    };

    const int candidateSeverity = severity(candidate.healthState);
    const int currentSeverity = severity(current.healthState);
    if (candidateSeverity != currentSeverity) {
        return candidateSeverity > currentSeverity;
    }

    const uint64_t candidateDrops =
        candidate.droppedDatagrams +
        candidate.invalidDatagrams +
        candidate.incompleteFramesDropped +
        candidate.decodeResyncs +
        candidate.decodeSkippedPackets +
        candidate.previewLateDrops +
        candidate.previewOverflowDrops;
    const uint64_t currentDrops =
        current.droppedDatagrams +
        current.invalidDatagrams +
        current.incompleteFramesDropped +
        current.decodeResyncs +
        current.decodeSkippedPackets +
        current.previewLateDrops +
        current.previewOverflowDrops;
    if (candidateDrops != currentDrops) {
        return candidateDrops > currentDrops;
    }

    return candidate.sequence > current.sequence;
}

void MergeReceiverFeedback(
    screenshare::UdpSenderStats& aggregate,
    const screenshare::udp_protocol::FeedbackSnapshot& feedback)
{
    if (!aggregate.hasFeedback || FeedbackLooksWorse(feedback, aggregate.latestFeedback)) {
        aggregate.latestFeedback = feedback;
    }
    aggregate.hasFeedback = true;
}

std::string UdpEndpointText(const std::string& host, uint16_t port)
{
    return host + ":" + std::to_string(port);
}

std::string LogTokenEncode(std::string_view text)
{
    constexpr char Hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char ch : text) {
        const bool safe =
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' ||
            ch == '_' ||
            ch == '.' ||
            ch == '~';
        if (safe) {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(Hex[(ch >> 4) & 0x0F]);
            encoded.push_back(Hex[ch & 0x0F]);
        }
    }
    return encoded;
}

class UdpSenderFanout {
public:
    struct Target {
        std::unique_ptr<screenshare::UdpSender> sender;
        uint32_t group = 0;
        std::string endpoint;
        std::vector<std::string> additionalEndpoints;
        bool failed = false;
        std::string error;
    };

    struct ViewerSnapshot {
        size_t group = 0;
        std::string endpoint;
        bool failed = false;
        std::string error;
        uint64_t feedbackPacketsReceived = 0;
        uint64_t pendingDatagrams = 0;
        uint64_t pendingQueueDelayMs = 0;
        bool hasFeedback = false;
        std::string displayName;
        screenshare::udp_protocol::FeedbackSnapshot latestFeedback{};
    };

    void Open(const std::vector<screenshare::UdpSenderConfig>& configs)
    {
        Close();
        if (configs.empty()) {
            throw std::invalid_argument("UDP sender fanout needs at least one target");
        }

        for (const auto& config : configs) {
            auto sender = std::make_unique<screenshare::UdpSender>();
            sender->Open(config);
            Target target;
            target.sender = std::move(sender);
            target.group = config.group;
            target.endpoint = UdpEndpointText(config.host, config.port);
            target.additionalEndpoints.reserve(config.additionalTargets.size());
            for (const auto& additional : config.additionalTargets) {
                target.additionalEndpoints.push_back(UdpEndpointText(additional.host, additional.port));
            }
            targets_.push_back(std::move(target));
        }
    }

    void Close()
    {
        targets_.clear();
    }

    bool AddAdditionalTarget(const screenshare::UdpSenderEndpoint& endpoint)
    {
        const std::string endpointText = UdpEndpointText(endpoint.host, endpoint.port);
        suppressedEndpoints_.erase(endpointText);
        for (auto& target : targets_) {
            if (target.failed || target.sender == nullptr || !target.sender->isOpen()) {
                continue;
            }
            const bool added = target.sender->AddAdditionalTarget(endpoint);
            if (added) {
                target.additionalEndpoints.push_back(endpointText);
            }
            return added;
        }
        throw std::runtime_error("No active UDP sender target is available for live signaling");
    }

    void SuppressEndpoint(const std::string& endpoint)
    {
        suppressedEndpoints_.insert(endpoint);
    }

    void UnsuppressEndpoint(const std::string& endpoint)
    {
        suppressedEndpoints_.erase(endpoint);
    }

    void SetViewerName(uint32_t group, std::string name)
    {
        if (group == 0 || name.empty()) {
            return;
        }
        viewerNames_[group] = std::move(name);
    }

    [[nodiscard]] size_t targetCount() const noexcept
    {
        return targets_.size();
    }

    [[nodiscard]] size_t failedTargetCount() const noexcept
    {
        return static_cast<size_t>(std::count_if(targets_.begin(), targets_.end(), [](const auto& target) {
            return target.failed;
        }));
    }

    [[nodiscard]] size_t activeTargetCount() const noexcept
    {
        return targetCount() - failedTargetCount();
    }

    [[nodiscard]] bool isOpen() const noexcept
    {
        return !targets_.empty() &&
               std::any_of(targets_.begin(), targets_.end(), [](const auto& target) {
                   return !target.failed && target.sender != nullptr && target.sender->isOpen();
               });
    }

    void SendFrame(const screenshare::EncodedPacket& packet)
    {
        ForEachActiveTarget("video send", [&](screenshare::UdpSender& sender) {
            sender.SendFrame(packet);
        });
    }

    void SendAudioPacket(const screenshare::UdpAudioPacket& packet)
    {
        ForEachActiveTarget("audio send", [&](screenshare::UdpSender& sender) {
            sender.SendAudioPacket(packet);
        });
    }

    void SetPacingBitrate(uint32_t bitrate)
    {
        ForEachActiveTarget("pacing update", [&](screenshare::UdpSender& sender) {
            sender.SetPacingBitrate(bitrate);
        });
    }

    void Flush()
    {
        for (auto& target : targets_) {
            if (target.failed || target.sender == nullptr) {
                continue;
            }
            try {
                target.sender->Flush();
            } catch (const std::exception& error) {
                MarkFailed(target, "flush", error.what());
            }
        }
    }

    [[nodiscard]] std::optional<screenshare::udp_protocol::FeedbackSnapshot> ReceiveFeedback(
        std::chrono::milliseconds firstTimeout)
    {
        std::optional<screenshare::udp_protocol::FeedbackSnapshot> selectedFeedback;
        bool timeoutUsed = false;
        for (auto& target : targets_) {
            if (target.failed || target.sender == nullptr) {
                continue;
            }
            std::chrono::milliseconds timeout = timeoutUsed ? std::chrono::milliseconds(0) : firstTimeout;
            try {
                for (;;) {
                    auto feedback = target.sender->ReceiveFeedback(timeout);
                    timeoutUsed = true;
                    if (!feedback) {
                        break;
                    }
                    if (!selectedFeedback || FeedbackLooksWorse(*feedback, *selectedFeedback)) {
                        selectedFeedback = *feedback;
                    }
                    timeout = std::chrono::milliseconds(0);
                }
            } catch (const std::exception& error) {
                MarkFailed(target, "feedback receive", error.what());
            }
        }
        return selectedFeedback;
    }

    [[nodiscard]] screenshare::UdpSenderStats stats() const
    {
        screenshare::UdpSenderStats aggregate;
        for (const auto& target : targets_) {
            if (target.sender == nullptr) {
                continue;
            }
            const auto current = target.sender->stats();
            aggregate.framesSent += current.framesSent;
            aggregate.framesDropped += current.framesDropped;
            aggregate.audioPacketsSent += current.audioPacketsSent;
            aggregate.audioPacketsDropped += current.audioPacketsDropped;
            aggregate.audioFramesSent += current.audioFramesSent;
            aggregate.audioDatagramsQueued += current.audioDatagramsQueued;
            aggregate.audioPayloadBytesSent += current.audioPayloadBytesSent;
            aggregate.datagramsQueued += current.datagramsQueued;
            aggregate.datagramsSent += current.datagramsSent;
            aggregate.datagramsDropped += current.datagramsDropped;
            aggregate.payloadBytesSent += current.payloadBytesSent;
            aggregate.wireBytesSent += current.wireBytesSent;
            aggregate.pendingDatagrams += current.pendingDatagrams;
            aggregate.peakPendingDatagrams = std::max(aggregate.peakPendingDatagrams, current.peakPendingDatagrams);
            aggregate.pendingQueueDelayMs = std::max(aggregate.pendingQueueDelayMs, current.pendingQueueDelayMs);
            aggregate.peakQueueDelayMs = std::max(aggregate.peakQueueDelayMs, current.peakQueueDelayMs);
            aggregate.feedbackPacketsReceived += current.feedbackPacketsReceived;
            aggregate.invalidFeedbackPackets += current.invalidFeedbackPackets;
            aggregate.feedbackAccessRejected += current.feedbackAccessRejected;
            aggregate.feedbackCryptoRejected += current.feedbackCryptoRejected;
            aggregate.natProbePacketsReceived += current.natProbePacketsReceived;
            aggregate.natProbeRetargets += current.natProbeRetargets;
            aggregate.natProbeRetargetRejected += current.natProbeRetargetRejected;
            aggregate.natProbeTargetCount += current.natProbeTargetCount;
            aggregate.natProbeRetargetActive = aggregate.natProbeRetargetActive || current.natProbeRetargetActive;
            if (aggregate.natProbeRetargetEndpoint.empty() && !current.natProbeRetargetEndpoint.empty()) {
                aggregate.natProbeRetargetEndpoint = current.natProbeRetargetEndpoint;
            }
            aggregate.encryptionEnabled = aggregate.encryptionEnabled || current.encryptionEnabled;
            if (current.hasFeedback) {
                MergeReceiverFeedback(aggregate, current.latestFeedback);
            }
            for (const auto& peer : current.feedbackPeers) {
                auto existing = std::find_if(
                    aggregate.feedbackPeers.begin(),
                    aggregate.feedbackPeers.end(),
                    [&](const screenshare::UdpSenderStats::FeedbackPeer& candidate) {
                        return candidate.endpoint == peer.endpoint;
                    });
                if (existing == aggregate.feedbackPeers.end()) {
                    aggregate.feedbackPeers.push_back(peer);
                } else {
                    existing->packetsReceived += peer.packetsReceived;
                    existing->latestFeedback = peer.latestFeedback;
                }
            }
        }
        return aggregate;
    }

    [[nodiscard]] std::vector<ViewerSnapshot> viewerSnapshots() const
    {
        std::vector<ViewerSnapshot> snapshots;
        for (const auto& target : targets_) {
            const screenshare::UdpSenderStats stats =
                target.sender != nullptr ? target.sender->stats() : screenshare::UdpSenderStats{};

            auto buildSnapshot = [&](const std::string& endpoint) {
                ViewerSnapshot snapshot;
                snapshot.group = target.group;
                snapshot.endpoint = endpoint;
                snapshot.failed = target.failed;
                snapshot.error = target.error;
                snapshot.pendingDatagrams = stats.pendingDatagrams;
                snapshot.pendingQueueDelayMs = stats.pendingQueueDelayMs;
                if (auto name = viewerNames_.find(snapshot.group); name != viewerNames_.end()) {
                    snapshot.displayName = name->second;
                }
                return snapshot;
            };

            if (stats.feedbackPeers.empty()) {
                if (suppressedEndpoints_.find(target.endpoint) != suppressedEndpoints_.end()) {
                    continue;
                }
                snapshots.push_back(buildSnapshot(target.endpoint));
                continue;
            }

            std::set<std::string> emittedEndpoints;
            for (const auto& peer : stats.feedbackPeers) {
                if (suppressedEndpoints_.find(peer.endpoint) != suppressedEndpoints_.end()) {
                    continue;
                }
                if (!emittedEndpoints.insert(peer.endpoint).second) {
                    continue;
                }
                ViewerSnapshot snapshot = buildSnapshot(peer.endpoint);
                snapshot.group = peer.group;
                if (auto name = viewerNames_.find(snapshot.group); name != viewerNames_.end()) {
                    snapshot.displayName = name->second;
                }
                snapshot.feedbackPacketsReceived = peer.packetsReceived;
                snapshot.hasFeedback = true;
                snapshot.latestFeedback = peer.latestFeedback;
                snapshots.push_back(std::move(snapshot));
            }
        }
        return snapshots;
    }

private:
    template <typename Fn>
    void ForEachActiveTarget(const char* operation, Fn&& fn)
    {
        bool sentToAnyTarget = false;
        for (auto& target : targets_) {
            if (target.failed || target.sender == nullptr) {
                continue;
            }
            try {
                fn(*target.sender);
                sentToAnyTarget = true;
            } catch (const std::exception& error) {
                MarkFailed(target, operation, error.what());
            }
        }
        if (!sentToAnyTarget && failedTargetCount() == targetCount()) {
            throw std::runtime_error("All UDP fanout targets failed");
        }
    }

    void MarkFailed(Target& target, const char* operation, const std::string& message)
    {
        if (target.failed) {
            return;
        }
        target.failed = true;
        target.error = std::string(operation) + " failed: " + message;
        std::cerr << "UDP fanout target failed: " << target.error << "\n";
    }

    std::vector<Target> targets_;
    std::set<std::string> suppressedEndpoints_;
    std::map<uint32_t, std::string> viewerNames_;
};

template <typename UdpSenderLike>
std::optional<screenshare::udp_protocol::FeedbackSnapshot> DrainUdpFeedback(
    UdpSenderLike& udpSender,
    std::chrono::milliseconds firstTimeout)
{
    auto latestFeedback = udpSender.ReceiveFeedback(firstTimeout);
    if (!latestFeedback) {
        return std::nullopt;
    }

    while (auto feedback = udpSender.ReceiveFeedback(std::chrono::milliseconds(0))) {
        latestFeedback = *feedback;
    }

    return latestFeedback;
}

void RecordLatestReceiverFeedback(
    SavedReportContext& reportContext,
    const std::optional<screenshare::udp_protocol::FeedbackSnapshot>& feedback)
{
    if (feedback) {
        reportContext.latestReceiverFeedback = *feedback;
    }
}

void PrintViewerSnapshots(const UdpSenderFanout& udpSender)
{
    const auto viewers = udpSender.viewerSnapshots();
    for (size_t index = 0; index < viewers.size(); ++index) {
        const auto& viewer = viewers[index];
        const char* state =
            viewer.failed ? "failed" :
            (viewer.hasFeedback ? "feedback" : "waiting");
        std::cout
            << "viewer_target=" << index
            << " viewer_group=" << viewer.group
            << " viewer_endpoint=" << (viewer.endpoint.empty() ? "unknown" : viewer.endpoint)
            << " viewer_name=" << (viewer.displayName.empty() ? "none" : LogTokenEncode(viewer.displayName))
            << " viewer_state=" << state
            << " viewer_feedback_packets=" << viewer.feedbackPacketsReceived
            << " viewer_pending=" << viewer.pendingDatagrams
            << " viewer_queue_ms=" << viewer.pendingQueueDelayMs
            << " viewer_feedback_health="
            << (viewer.hasFeedback ?
                screenshare::udp_protocol::FeedbackHealthStateName(viewer.latestFeedback.healthState) :
                "none")
            << " viewer_feedback_completed_frames="
            << (viewer.hasFeedback ? viewer.latestFeedback.completedFrames : 0)
            << " viewer_feedback_resyncs="
            << (viewer.hasFeedback ? viewer.latestFeedback.decodeResyncs : 0)
            << " viewer_feedback_session="
            << (viewer.hasFeedback && viewer.latestFeedback.sessionFingerprint != 0 ?
                FormatSessionFingerprint(viewer.latestFeedback.sessionFingerprint) :
                "none")
            << " viewer_feedback_access="
            << (viewer.hasFeedback ?
                (viewer.latestFeedback.accessCodeFingerprint == 0 ? "none" : "required") :
                "unknown")
            << "\n";
    }
}

std::string SignalingCandidateEndpoint(const screenshare::SignalingCandidate& candidate)
{
    return candidate.ip + ":" + std::to_string(candidate.port);
}

uint32_t SignalingPeerGroup(const std::string& peerId)
{
    uint32_t hash = 2166136261u;
    for (const unsigned char ch : peerId) {
        hash ^= ch;
        hash *= 16777619u;
    }
    hash &= 0x7fffffffu;
    return hash == 0 ? 1u : hash;
}

std::string DefaultSignalingPeerName()
{
    if (const char* computerName = std::getenv("COMPUTERNAME");
        computerName != nullptr && *computerName != '\0') {
        return computerName;
    }
    return "ScreenShare";
}

std::string SignalingPeerDisplayName(const screenshare::SignalingPeer& peer)
{
    if (!peer.metadata.name.empty()) {
        return peer.metadata.name;
    }
    return peer.peerId;
}

bool IsUsableHostSignalingAddress(const std::string& address)
{
    return !address.empty() &&
           address != "0.0.0.0" &&
           address != "127.0.0.1";
}

void MergeSignalingPeers(
    std::map<std::string, screenshare::SignalingPeer>& peersById,
    const screenshare::SignalingRoomResponse& response)
{
    if (!response.ok) {
        throw std::runtime_error("Signaling room response was not ok");
    }
    for (const auto& peer : response.peers) {
        if (peer.candidates.empty()) {
            continue;
        }
        peersById[peer.peerId] = peer;
    }
}

void ApplySignalingRoomAccessKey(Options& options, const screenshare::SignalingRoomResponse& response)
{
    if (options.accessCodeProvided || options.allowPlaintext) {
        return;
    }
    if (response.roomAccessKey.empty()) {
        throw std::runtime_error("Signaling server did not provide a room encryption key");
    }
    if (response.passwordProtected && options.signalingRoomPassword.empty()) {
        throw std::runtime_error("Signaling room requires a password");
    }

    const bool mixRoomPassword =
        response.passwordProtected || !options.signalingRoomPassword.empty();
    std::string keyMaterial = response.roomAccessKey;
    if (mixRoomPassword) {
        keyMaterial += "\npassword:";
        keyMaterial += options.signalingRoomPassword;
    }
    options.accessCodeFingerprint = screenshare::UdpAccessCodeFingerprint(keyMaterial);
    options.accessCodeKey = screenshare::DeriveUdpCryptoKey(keyMaterial);
    options.accessCodeProvided = true;
    std::cout
        << "signaling_room_encryption=ready"
        << " room=" << options.signalingRoomId
        << " password=" << (mixRoomPassword ? "yes" : "no")
        << " source=server\n";
}

screenshare::SignalingPeerState BuildLiveSignalingPeerState(const Options& options)
{
    if (!options.signalingLocalCandidateAvailable) {
        throw std::logic_error("Live signaling local candidate is not available");
    }

    screenshare::SignalingPeerState peer;
    peer.peerId = options.signalingPeerId;
    peer.metadata.name = options.signalingName.empty() ?
        (options.lanName.empty() ? DefaultSignalingPeerName() : options.lanName) :
        options.signalingName;
    peer.metadata.platform = options.signalingPlatform.empty() ? "windows" : options.signalingPlatform;
    peer.roomName = options.signalingRoomName;
    peer.roomPassword = options.signalingRoomPassword;
    peer.passwordProtected = !options.signalingRoomPassword.empty();
    peer.candidates.push_back(options.signalingLocalCandidate);
    if (options.signalingHostCandidateAvailable) {
        const bool duplicate =
            options.signalingHostCandidate.ip == options.signalingLocalCandidate.ip &&
            options.signalingHostCandidate.port == options.signalingLocalCandidate.port;
        if (!duplicate) {
            peer.candidates.push_back(options.signalingHostCandidate);
        }
    }
    return peer;
}

struct LiveSignalingPeerCandidate {
    std::string peerId;
    screenshare::SignalingCandidate candidate;
    std::string displayName;
};

class LiveSignalingRuntime {
public:
    ~LiveSignalingRuntime()
    {
        Stop();
    }

    void Start(const Options& options)
    {
        Stop();

        screenshare::SignalingClientConfig clientConfig;
        clientConfig.serverUrl = options.signalingServerUrl;
        clientConfig.timeout = std::min(
            std::chrono::milliseconds(options.signalingTimeoutMs),
            BackgroundRequestTimeout);

        auto state = std::make_shared<State>();
        state->roomId = options.signalingRoomId;
        state->peer = BuildLiveSignalingPeerState(options);
        state_ = state;
        worker_ = std::thread(&LiveSignalingRuntime::Run, std::move(state), std::move(clientConfig));

        std::cout
            << "signaling_live_refresh=started"
            << " room=" << options.signalingRoomId
            << " peer_id=" << options.signalingPeerId
            << " events=websocket"
            << " fallback_interval_ms=" << FallbackRefreshInterval.count()
            << "\n";
    }

    void Stop()
    {
        std::shared_ptr<State> state;
        if (state_) {
            state = state_;
            {
                std::lock_guard lock(state->mutex);
                state->stopRequested = true;
            }
            state->wake.notify_all();
            state_.reset();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::vector<LiveSignalingPeerCandidate> DrainDiscoveredPeers()
    {
        if (!state_) {
            return {};
        }

        std::lock_guard lock(state_->mutex);
        std::vector<LiveSignalingPeerCandidate> peers;
        peers.swap(state_->discoveredPeers);
        return peers;
    }

    std::vector<LiveSignalingPeerCandidate> DrainRemovedPeers()
    {
        if (!state_) {
            return {};
        }

        std::lock_guard lock(state_->mutex);
        std::vector<LiveSignalingPeerCandidate> peers;
        peers.swap(state_->removedPeers);
        return peers;
    }

    void UpdateRoomName(std::string roomName)
    {
        if (!state_) {
            return;
        }

        {
            std::lock_guard lock(state_->mutex);
            if (state_->peer.roomName == roomName) {
                return;
            }
            state_->peer.roomName = std::move(roomName);
            state_->refreshRequested = true;
        }
        state_->wake.notify_all();
    }

private:
    static constexpr std::chrono::milliseconds FallbackRefreshInterval{2000};
    static constexpr std::chrono::milliseconds EventReconnectDelay{5000};
    static constexpr std::chrono::milliseconds BackgroundRequestTimeout{250};

    struct State {
        mutable std::mutex mutex;
        std::condition_variable wake;
        std::string roomId;
        screenshare::SignalingPeerState peer;
        std::map<std::string, LiveSignalingPeerCandidate> seenCandidates;
        std::vector<LiveSignalingPeerCandidate> discoveredPeers;
        std::vector<LiveSignalingPeerCandidate> removedPeers;
        bool refreshRequested = false;
        bool stopRequested = false;
    };

    [[nodiscard]] static std::string CandidateKey(
        const std::string& peerId,
        const screenshare::SignalingCandidate& candidate)
    {
        return peerId + "|" + candidate.ip + ":" + std::to_string(candidate.port) + "|" + candidate.protocol;
    }

    static void RecordPeers(const std::shared_ptr<State>& state, const screenshare::SignalingRoomResponse& response)
    {
        if (!response.ok) {
            throw std::runtime_error("Signaling room response was not ok");
        }

        std::lock_guard lock(state->mutex);
        std::map<std::string, LiveSignalingPeerCandidate> latestCandidates;
        for (const auto& peer : response.peers) {
            if (peer.peerId == state->peer.peerId) {
                continue;
            }
            for (const auto& candidate : peer.candidates) {
                const std::string key = CandidateKey(peer.peerId, candidate);
                LiveSignalingPeerCandidate record{peer.peerId, candidate, SignalingPeerDisplayName(peer)};
                latestCandidates.emplace(key, record);
                if (state->seenCandidates.find(key) == state->seenCandidates.end()) {
                    state->discoveredPeers.push_back(std::move(record));
                }
            }
        }

        for (const auto& [key, candidate] : state->seenCandidates) {
            if (latestCandidates.find(key) == latestCandidates.end()) {
                state->removedPeers.push_back(candidate);
            }
        }
        state->seenCandidates = std::move(latestCandidates);
    }

    static void RequestRefresh(const std::shared_ptr<State>& state)
    {
        {
            std::lock_guard lock(state->mutex);
            if (state->stopRequested) {
                return;
            }
            state->refreshRequested = true;
        }
        state->wake.notify_all();
    }

    [[nodiscard]] static bool WaitForNextRefresh(const std::shared_ptr<State>& state)
    {
        std::unique_lock lock(state->mutex);
        state->wake.wait_for(lock, FallbackRefreshInterval, [&]() {
            return state->stopRequested || state->refreshRequested;
        });
        if (state->stopRequested) {
            return true;
        }
        state->refreshRequested = false;
        return false;
    }

    [[nodiscard]] static bool WaitForEventReconnect(const std::shared_ptr<State>& state)
    {
        std::unique_lock lock(state->mutex);
        return state->wake.wait_for(lock, EventReconnectDelay, [&]() {
            return state->stopRequested;
        });
    }

    [[nodiscard]] static bool stopRequested(const std::shared_ptr<State>& state)
    {
        std::lock_guard lock(state->mutex);
        return state->stopRequested;
    }

    [[nodiscard]] static screenshare::SignalingPeerState SnapshotPeer(const std::shared_ptr<State>& state)
    {
        std::lock_guard lock(state->mutex);
        return state->peer;
    }

    static void RunEvents(std::shared_ptr<State> state, screenshare::SignalingClientConfig clientConfig)
    {
        while (!stopRequested(state)) {
            try {
                screenshare::SignalingClient client(clientConfig);
                client.ListenRoomEvents(
                    state->roomId,
                    state->peer.peerId,
                    state->peer.roomPassword,
                    [state](const std::string& message) {
                        const bool shouldRefresh =
                            message.find("\"type\":\"hello\"") != std::string::npos ||
                            message.find("\"type\":\"peer_joined\"") != std::string::npos ||
                            message.find("\"type\":\"peer_updated\"") != std::string::npos ||
                            message.find("\"type\":\"peer_left\"") != std::string::npos;
                        std::cout
                            << "signaling_live_event=received"
                            << " room=" << state->roomId
                            << " peer_id=" << state->peer.peerId
                            << " bytes=" << message.size()
                            << " refresh=" << (shouldRefresh ? "yes" : "no")
                            << "\n";
                        if (shouldRefresh) {
                            RequestRefresh(state);
                        }
                    },
                    [state]() {
                        return stopRequested(state);
                    });
            } catch (const std::exception& error) {
                if (!stopRequested(state)) {
                    std::cerr
                        << "signaling_live_events=error"
                        << " room=" << state->roomId
                        << " peer_id=" << state->peer.peerId
                        << " error=\"" << error.what() << "\""
                        << "\n";
                }
            }

            if (WaitForEventReconnect(state)) {
                break;
            }
        }
    }

    static void Run(std::shared_ptr<State> state, screenshare::SignalingClientConfig clientConfig)
    {
        std::thread eventsWorker(&LiveSignalingRuntime::RunEvents, state, clientConfig);
        screenshare::SignalingClient client(std::move(clientConfig));

        while (!stopRequested(state)) {
            try {
                RecordPeers(state, client.Join(state->roomId, SnapshotPeer(state)));
            } catch (const std::exception& error) {
                std::cerr
                    << "signaling_live_refresh=error"
                    << " room=" << state->roomId
                    << " peer_id=" << state->peer.peerId
                    << " error=\"" << error.what() << "\""
                    << "\n";
            }

            if (WaitForNextRefresh(state)) {
                break;
            }
        }

        const std::string peerId = SnapshotPeer(state).peerId;
        {
            std::lock_guard lock(state->mutex);
            state->stopRequested = true;
        }
        state->wake.notify_all();

        try {
            client.Leave(state->roomId, peerId);
            std::cout
                << "signaling_live_leave=ok"
                << " room=" << state->roomId
                << " peer_id=" << peerId
                << "\n" << std::flush;
        } catch (const std::exception& error) {
            std::cerr
                << "signaling_live_leave=error"
                << " room=" << state->roomId
                << " peer_id=" << peerId
                << " error=\"" << error.what() << "\""
                << "\n";
        }

        if (eventsWorker.joinable()) {
            // WinHTTP WebSocket receives can remain blocked during shutdown. The
            // event worker owns only shared shutdown state, so do not let it
            // delay leaving the room or completing Stop().
            eventsWorker.detach();
        }
    }

    std::thread worker_;
    std::shared_ptr<State> state_;
};

UdpSendTargetSpec SignalingSendTargetSpec(
    const screenshare::SignalingCandidate& candidate,
    uint16_t localPort,
    uint64_t probeSession,
    uint32_t group,
    std::string displayName)
{
    return UdpSendTargetSpec{
        SignalingCandidateEndpoint(candidate),
        true,
        "signaling",
        localPort,
        false,
        true,
        true,
        probeSession,
        group,
        std::move(displayName)};
}

class AdaptiveBitrateAdvisor {
public:
    void Configure(uint32_t targetBitrate, uint32_t minBitrate, uint32_t reduceCooldownReports)
    {
        targetBitrate_ = targetBitrate;
        recommendedBitrate_ = targetBitrate;
        const uint32_t defaultMinBitrate = std::max<uint32_t>(1'000'000, targetBitrate / 4);
        minBitrate_ = std::min(minBitrate == 0 ? defaultMinBitrate : minBitrate, targetBitrate);
        reduceCooldownReports_ = reduceCooldownReports;
        reduceCooldownRemaining_ = 0;
        suppressedReductions_ = 0;
        stableFeedbackCount_ = 0;
        previewPressureReports_ = 0;
        queuePressureReports_ = 0;
        hasFeedback_ = false;
        lastFeedbackSequence_ = 0;
        lastTransportDropSignals_ = 0;
        lastPreviewDropSignals_ = 0;
        lastResyncs_ = 0;
        lastSkippedPackets_ = 0;
        action_ = "hold";
        reason_ = "waiting_for_feedback";
    }

    void Update(const screenshare::UdpSenderStats& stats)
    {
        if (targetBitrate_ == 0) {
            return;
        }
        if (!stats.hasFeedback) {
            action_ = "hold";
            reason_ = "waiting_for_feedback";
            return;
        }

        const auto& feedback = stats.latestFeedback;
        if (hasFeedback_ && feedback.sequence == lastFeedbackSequence_) {
            return;
        }

        const uint64_t transportDropSignals =
            feedback.droppedDatagrams +
            feedback.invalidDatagrams +
            feedback.incompleteFramesDropped;
        const uint64_t previewDropSignals =
            feedback.previewLateDrops +
            feedback.previewOverflowDrops;
        const bool newTransportDropSignal =
            !hasFeedback_ ? transportDropSignals > 0 : transportDropSignals > lastTransportDropSignals_;
        const bool newPreviewDropSignal =
            !hasFeedback_ ? previewDropSignals > 0 : previewDropSignals > lastPreviewDropSignals_;
        const bool newDecodeRecovery =
            (!hasFeedback_ ? feedback.decodeResyncs > 0 : feedback.decodeResyncs > lastResyncs_) ||
            (!hasFeedback_ ? feedback.decodeSkippedPackets > 0 : feedback.decodeSkippedPackets > lastSkippedPackets_);
        const bool senderQueuePressure =
            stats.pendingQueueDelayMs >= SenderQueuePressureMs ||
            (stats.pendingQueueDelayMs == 0 && stats.pendingDatagrams >= SenderQueuePressureDatagrams);
        const bool queuePressure =
            feedback.healthState == screenshare::udp_protocol::FeedbackHealthState::Buffering ||
            feedback.pendingFrames >= ReceiverHealthPendingFrameWarning ||
            feedback.pendingDecodePackets >= OrderedReceiverRecoveryThresholdFrames ||
            senderQueuePressure;
        if (queuePressure) {
            ++queuePressureReports_;
        } else {
            queuePressureReports_ = 0;
        }
        if (newPreviewDropSignal) {
            ++previewPressureReports_;
        } else {
            previewPressureReports_ = 0;
        }
        const bool sustainedQueuePressure =
            queuePressureReports_ >= QueuePressureReportsBeforeReduce;
        const bool sustainedPreviewPressure =
            previewPressureReports_ >= PreviewPressureReportsBeforeReduce;

        if (newTransportDropSignal || newDecodeRecovery || sustainedQueuePressure || sustainedPreviewPressure) {
            stableFeedbackCount_ = 0;
            if (reduceCooldownRemaining_ > 0) {
                --reduceCooldownRemaining_;
                ++suppressedReductions_;
                action_ = "hold";
                reason_ = "reduce_cooldown";
            } else if (recommendedBitrate_ <= minBitrate_) {
                action_ = "hold";
                reason_ = "min_bitrate";
            } else {
                const uint32_t reduced = static_cast<uint32_t>(
                    static_cast<uint64_t>(recommendedBitrate_) * 80 / 100);
                recommendedBitrate_ = std::max(minBitrate_, reduced);
                reduceCooldownRemaining_ = reduceCooldownReports_;
                action_ = "reduce";
                reason_ = newDecodeRecovery ? "receiver_recovery" :
                    (newTransportDropSignal ? "receiver_loss" :
                        (sustainedQueuePressure ? "queue_pressure" : "preview_pressure"));
            }
        } else if (queuePressure) {
            stableFeedbackCount_ = 0;
            if (reduceCooldownRemaining_ > 0) {
                --reduceCooldownRemaining_;
            }
            action_ = "hold";
            reason_ = "queue_stabilizing";
        } else if (newPreviewDropSignal) {
            stableFeedbackCount_ = 0;
            if (reduceCooldownRemaining_ > 0) {
                --reduceCooldownRemaining_;
            }
            action_ = "hold";
            reason_ = "preview_stabilizing";
        } else if (recommendedBitrate_ < targetBitrate_) {
            if (reduceCooldownRemaining_ > 0) {
                --reduceCooldownRemaining_;
            }
            ++stableFeedbackCount_;
            if (stableFeedbackCount_ >= StableFeedbackReportsBeforeIncrease) {
                const uint32_t increased = static_cast<uint32_t>(
                    static_cast<uint64_t>(recommendedBitrate_) * 110 / 100);
                recommendedBitrate_ = std::min(targetBitrate_, increased);
                stableFeedbackCount_ = 0;
                action_ = "increase";
                reason_ = "stable_feedback";
            } else {
                action_ = "hold";
                reason_ = "stabilizing";
            }
        } else {
            if (reduceCooldownRemaining_ > 0) {
                --reduceCooldownRemaining_;
            }
            stableFeedbackCount_ = 0;
            action_ = "hold";
            reason_ = feedback.healthState == screenshare::udp_protocol::FeedbackHealthState::Ok ?
                "healthy" :
                "waiting_for_recovery";
        }

        hasFeedback_ = true;
        lastFeedbackSequence_ = feedback.sequence;
        lastTransportDropSignals_ = transportDropSignals;
        lastPreviewDropSignals_ = previewDropSignals;
        lastResyncs_ = feedback.decodeResyncs;
        lastSkippedPackets_ = feedback.decodeSkippedPackets;
    }

    [[nodiscard]] uint32_t recommendedBitrate() const noexcept { return recommendedBitrate_; }
    [[nodiscard]] uint32_t targetBitrate() const noexcept { return targetBitrate_; }
    [[nodiscard]] uint32_t minBitrate() const noexcept { return minBitrate_; }
    [[nodiscard]] uint32_t reduceCooldownRemaining() const noexcept { return reduceCooldownRemaining_; }
    [[nodiscard]] uint64_t suppressedReductions() const noexcept { return suppressedReductions_; }
    [[nodiscard]] const char* action() const noexcept { return action_; }
    [[nodiscard]] const char* reason() const noexcept { return reason_; }
    [[nodiscard]] bool configured() const noexcept { return targetBitrate_ != 0; }

private:
    static constexpr uint32_t StableFeedbackReportsBeforeIncrease = 3;
    static constexpr uint32_t QueuePressureReportsBeforeReduce = 2;
    static constexpr uint32_t PreviewPressureReportsBeforeReduce = 3;

    uint32_t targetBitrate_ = 0;
    uint32_t recommendedBitrate_ = 0;
    uint32_t minBitrate_ = 0;
    uint32_t reduceCooldownReports_ = 3;
    uint32_t reduceCooldownRemaining_ = 0;
    uint32_t stableFeedbackCount_ = 0;
    uint32_t previewPressureReports_ = 0;
    uint32_t queuePressureReports_ = 0;
    uint64_t suppressedReductions_ = 0;
    bool hasFeedback_ = false;
    uint64_t lastFeedbackSequence_ = 0;
    uint64_t lastTransportDropSignals_ = 0;
    uint64_t lastPreviewDropSignals_ = 0;
    uint64_t lastResyncs_ = 0;
    uint64_t lastSkippedPackets_ = 0;
    const char* action_ = "hold";
    const char* reason_ = "waiting_for_feedback";
};

std::string FormatAvSyncTitle(const AvSyncSnapshot& avSync, double playoutAudioAheadMs, bool playoutReady)
{
    if (!playoutReady) {
        return "av wait";
    }
    if (!avSync.ready) {
        return "av video";
    }

    std::ostringstream stream;
    stream << "av "
           << (playoutAudioAheadMs >= 0.0 ? "+" : "")
           << std::fixed << std::setprecision(0)
           << playoutAudioAheadMs
           << "ms";
    return stream.str();
}

std::string FormatAudioPlaybackTitle(bool enabled, std::string_view status, bool muted, float volume)
{
    if (!enabled) {
        return "aud off";
    }

    const int percent = static_cast<int>(std::lround(std::clamp(volume, 0.0f, 2.0f) * 100.0f));
    std::ostringstream stream;
    stream << "aud ";
    if (muted) {
        stream << "muted";
    } else if (!status.empty()) {
        stream << status;
    } else {
        stream << "on";
    }
    stream << " " << percent << "%";
    return stream.str();
}

std::string FormatReceiverHealthTitle(
    const ReceiverHealthSnapshot& health,
    const AvSyncSnapshot& avSync,
    double playoutAudioAheadMs,
    bool avSyncPlayoutReady)
{
    const uint64_t transportDrops =
        health.simulatedDroppedDatagrams + health.invalidDatagrams + health.incompleteDroppedFrames;
    const uint64_t previewDrops = health.previewLateDrops + health.previewOverflowDrops;

    std::ostringstream stream;
    stream << ReceiverHealthState(health)
           << " | res " << health.decodedWidth << "x" << health.decodedHeight
           << " | fps " << std::fixed << std::setprecision(1) << health.completedFps
           << " | lat " << health.previewLatencyMs << "/" << health.previewMaxLateMs << "ms"
           << " | q " << health.pendingFrames << "/" << health.pendingDecodePackets << "/" << health.previewQueuedFrames
           << " | resync " << health.h264DecodeResyncs
           << " | skip " << health.h264DecodeSkippedPackets
           << " | drops " << transportDrops << "/" << previewDrops
           << " | reset " << health.previewPlayoutResets
           << " | shown " << health.previewFramesPresented
           << " | " << FormatAvSyncTitle(avSync, playoutAudioAheadMs, avSyncPlayoutReady);
    return stream.str();
}

const char* CaptureBackendName(screenshare::CaptureBackend backend)
{
    switch (backend) {
    case screenshare::CaptureBackend::DesktopDuplication:
        return "dxgi";
    case screenshare::CaptureBackend::WindowsGraphicsCapture:
        return "wgc";
    default:
        return "unknown";
    }
}

bool PrefersHardwareStream(StreamEncoderPreference preference)
{
    return preference == StreamEncoderPreference::Auto || preference == StreamEncoderPreference::Hardware;
}

void ConfigureCapturePayloads(
    screenshare::CaptureConfig& config,
    const Options& options,
    StreamEncoderPreference streamEncoderPreference)
{
    const bool optimizeForHardwareStream = options.streamEncode && PrefersHardwareStream(streamEncoderPreference);
    config.includeNv12 = options.streamEncode;
    config.includeNv12Readback = !optimizeForHardwareStream;
    config.includeBgraReadback =
        !optimizeForHardwareStream ||
        !options.recordPath.empty() ||
        !options.capturedBmpPath.empty();
}

bool HasSoftwareStreamInput(const screenshare::CapturedFrame& frame)
{
    return !frame.nv12Pixels.empty() || !frame.pixels.empty();
}

uint32_t SelectBitrate(const Options& options, int width, int height)
{
    if (options.bitrate > 0) {
        return options.bitrate;
    }

    const uint64_t pixelsPerSecond =
        static_cast<uint64_t>(width) *
        static_cast<uint64_t>(height) *
        static_cast<uint64_t>(options.fps);

    const uint64_t estimated = pixelsPerSecond * 16 / 100;
    constexpr uint64_t minBitrate = 8'000'000;
    constexpr uint64_t maxBitrate = 80'000'000;
    return static_cast<uint32_t>(std::clamp(estimated, minBitrate, maxBitrate));
}

uint32_t SelectAdaptiveMinBitrate(
    const Options& options,
    uint32_t targetBitrate,
    const std::vector<ResolutionTier>& resolutionTiers)
{
    if (options.adaptMinBitrateProvided) {
        return options.adaptMinBitrate;
    }
    if (!options.adaptResolution || targetBitrate == 0 || resolutionTiers.size() < 2) {
        return 0;
    }

    const auto& nativeTier = resolutionTiers.front();
    const auto& lowestTier = resolutionTiers.back();
    const uint64_t nativePixels =
        static_cast<uint64_t>(nativeTier.width) *
        static_cast<uint64_t>(nativeTier.height);
    const uint64_t lowestPixels =
        static_cast<uint64_t>(lowestTier.width) *
        static_cast<uint64_t>(lowestTier.height);
    if (nativePixels == 0 || lowestPixels == 0) {
        return 0;
    }

    const uint64_t areaScaledFloor =
        static_cast<uint64_t>(targetBitrate) *
        lowestPixels /
        nativePixels /
        2;
    const uint64_t legacyFloor = std::max<uint64_t>(1'000'000, targetBitrate / 4);
    const uint64_t adaptiveFloor = std::min(areaScaledFloor, legacyFloor);
    return static_cast<uint32_t>(std::clamp<uint64_t>(adaptiveFloor, 2'000'000, targetBitrate));
}

double Mbps(uint32_t bitrate)
{
    return static_cast<double>(bitrate) / 1'000'000.0;
}

double Dbfs(double value)
{
    if (value <= 0.000001) {
        return -120.0;
    }
    return 20.0 * std::log10(std::min(value, 1.0));
}

screenshare::udp_protocol::AudioSampleFormat ToUdpAudioSampleFormat(const screenshare::AudioCaptureFormat& format)
{
    if (format.sampleFormat == "float32") {
        return screenshare::udp_protocol::AudioSampleFormat::Float32;
    }
    if (format.sampleFormat == "pcm16") {
        return screenshare::udp_protocol::AudioSampleFormat::Pcm16;
    }
    if (format.sampleFormat == "pcm24") {
        return screenshare::udp_protocol::AudioSampleFormat::Pcm24;
    }
    if (format.sampleFormat == "pcm32") {
        return screenshare::udp_protocol::AudioSampleFormat::Pcm32;
    }
    return screenshare::udp_protocol::AudioSampleFormat::Unknown;
}

screenshare::AudioPlaybackFormat AudioPlaybackFormatFromPacket(const screenshare::UdpCompletedAudioPacket& packet)
{
    screenshare::AudioPlaybackFormat format;
    format.sampleRate = packet.sampleRate;
    format.channels = packet.channels;
    format.bitsPerSample = packet.bitsPerSample;
    format.blockAlign = packet.blockAlign;
    format.sampleFormat = packet.sampleFormat;
    return format;
}

uint32_t AudioFlagsFromPacket(const screenshare::CapturedAudioPacket& packet)
{
    return
        (packet.silent ? screenshare::udp_protocol::AudioPacketFlagSilent : 0U) |
        (packet.dataDiscontinuity ? screenshare::udp_protocol::AudioPacketFlagDataDiscontinuity : 0U) |
        (packet.timestampError ? screenshare::udp_protocol::AudioPacketFlagTimestampError : 0U);
}

screenshare::UdpAudioPacket BuildRawUdpAudioPacket(
    const screenshare::CapturedAudioPacket& packet,
    const screenshare::AudioCaptureFormat& format,
    screenshare::udp_protocol::AudioSampleFormat sampleFormat)
{
    screenshare::UdpAudioPacket audioPacket;
    audioPacket.devicePosition = packet.devicePosition;
    audioPacket.qpcPosition = packet.qpcPosition;
    audioPacket.sampleRate = format.sampleRate;
    audioPacket.channels = format.channels;
    audioPacket.bitsPerSample = format.bitsPerSample;
    audioPacket.blockAlign = format.blockAlign;
    audioPacket.sampleFormat = sampleFormat;
    audioPacket.codec = screenshare::udp_protocol::AudioCodec::Raw;
    audioPacket.audioFrames = packet.frames;
    audioPacket.flags = AudioFlagsFromPacket(packet);
    audioPacket.bytes = packet.data;
    return audioPacket;
}

screenshare::UdpAudioPacket BuildOpusUdpAudioPacket(
    const screenshare::CapturedAudioPacket& packet,
    screenshare::OpusAudioEncoder& encoder)
{
    const auto encoded = encoder.Encode(packet);

    screenshare::UdpAudioPacket audioPacket;
    audioPacket.devicePosition = packet.devicePosition;
    audioPacket.qpcPosition = packet.qpcPosition;
    audioPacket.sampleRate = encoded.sampleRate;
    audioPacket.channels = encoded.channels;
    audioPacket.bitsPerSample = encoded.bitsPerSample;
    audioPacket.blockAlign = encoded.blockAlign;
    audioPacket.sampleFormat = screenshare::udp_protocol::AudioSampleFormat::Float32;
    audioPacket.codec = screenshare::udp_protocol::AudioCodec::Opus;
    audioPacket.audioFrames = encoded.audioFrames;
    audioPacket.flags = AudioFlagsFromPacket(packet);
    audioPacket.bytes = encoded.bytes;
    return audioPacket;
}

screenshare::UdpCompletedAudioPacket DecodeOpusAudioPacketForPlayback(
    const screenshare::UdpCompletedAudioPacket& packet,
    screenshare::OpusAudioDecoder& decoder)
{
    screenshare::UdpCompletedAudioPacket decoded = packet;
    decoded.bytes = decoder.Decode(packet.bytes, packet.audioFrames);
    decoded.codec = screenshare::udp_protocol::AudioCodec::Raw;
    decoded.sampleRate = screenshare::OpusAudioDecoder::OutputSampleRate;
    decoded.channels = screenshare::OpusAudioDecoder::OutputChannels;
    decoded.bitsPerSample = screenshare::OpusAudioDecoder::OutputBitsPerSample;
    decoded.blockAlign = screenshare::OpusAudioDecoder::OutputBlockAlign;
    decoded.sampleFormat = screenshare::udp_protocol::AudioSampleFormat::Float32;
    decoded.audioFrames = static_cast<uint32_t>(decoded.bytes.size() / decoded.blockAlign);
    return decoded;
}

void WarnIfPlaintextUdpSession(const Options& options)
{
    if (HasUdpSession(options) && !options.accessCodeProvided && !options.allowPlaintext) {
        std::cout
            << "Warning: UDP session is plaintext. Add --access-code CODE for encryption, "
            << "or --allow-plaintext to acknowledge plaintext mode.\n";
    }
}

void RunAudioCaptureStats(
    const Options& options,
    SavedReportContext& reportContext,
    screenshare::ISessionRuntimeControl& runtimeControl)
{
    screenshare::WasapiCapture capture;
    screenshare::AudioCaptureConfig config;
    config.source = options.audioCaptureSource;
    config.processId = options.audioProcessId;
    if (!options.audioDeviceId.empty()) {
        config.deviceId = screenshare::Widen(options.audioDeviceId);
    }
    capture.Start(config);

    const auto& format = capture.format();
    const auto udpSampleFormat = ToUdpAudioSampleFormat(format);
    if (udpSampleFormat == screenshare::udp_protocol::AudioSampleFormat::Unknown) {
        throw std::runtime_error("Unsupported WASAPI sample format for audio UDP diagnostics: " + format.sampleFormat);
    }

    std::unique_ptr<screenshare::UdpSender> audioSender;
    std::unique_ptr<screenshare::OpusAudioEncoder> opusEncoder;
    if (!options.audioSendTarget.empty()) {
        auto udpConfig = screenshare::ParseUdpSenderTarget(options.audioSendTarget);
        udpConfig.localPort = options.udpLocalPort;
        udpConfig.pacingEnabled = false;
        udpConfig.maxQueuedDatagrams = 16'384;
        udpConfig.accessCodeFingerprint = options.accessCodeFingerprint;
        udpConfig.encryptionKey = options.accessCodeKey;
        audioSender = std::make_unique<screenshare::UdpSender>();
        audioSender->Open(udpConfig);
        if (options.audioCodec == screenshare::udp_protocol::AudioCodec::Opus) {
            opusEncoder = std::make_unique<screenshare::OpusAudioEncoder>();
            opusEncoder->Start(format);
        }
    }

    WarnIfPlaintextUdpSession(options);

    std::cout
        << "Capturing "
        << screenshare::AudioCaptureSourceName(options.audioCaptureSource)
        << " audio with WASAPI"
        << ", device=\"" << screenshare::Narrow(capture.deviceName()) << "\""
        << ", format=" << screenshare::AudioCaptureFormatName(format)
        << ", buffer_frames=" << capture.bufferFrames()
        << ", session " << options.sessionId
        << " (" << FormatSessionFingerprint(options.sessionFingerprint) << ")"
        << ", seconds=" << options.seconds;
    if (options.accessCodeProvided) {
        std::cout << ", access code required, UDP encryption enabled";
    }
    if (audioSender) {
        std::cout
            << ", audio UDP sending to " << options.audioSendTarget
            << ", local port " << (options.udpLocalPort == 0 ? std::string("auto") : std::to_string(options.udpLocalPort))
            << ", codec " << screenshare::udp_protocol::AudioCodecName(options.audioCodec);
    }
    if (!options.audioDeviceId.empty()) {
        std::cout << ", selected by id";
    }
    std::cout << ".\n";

    using Clock = std::chrono::steady_clock;
    const auto startedAt = Clock::now();
    auto lastReportAt = startedAt;
    uint64_t totalPackets = 0;
    uint64_t totalFrames = 0;
    uint64_t totalBytes = 0;
    uint64_t totalSilentPackets = 0;
    uint64_t totalDiscontinuities = 0;
    uint64_t totalTimestampErrors = 0;
    uint64_t intervalPackets = 0;
    uint64_t intervalFrames = 0;
    uint64_t intervalBytes = 0;
    uint64_t intervalSilentPackets = 0;
    uint64_t intervalDiscontinuities = 0;
    uint64_t intervalTimestampErrors = 0;
    uint64_t intervalSamples = 0;
    long double intervalSquaredSamples = 0.0;
    double intervalPeak = 0.0;
    uint64_t emptyPolls = 0;

    auto recordPacket = [&](const screenshare::CapturedAudioPacket& packet) {
        ++totalPackets;
        ++intervalPackets;
        totalFrames += packet.frames;
        intervalFrames += packet.frames;
        totalBytes += packet.data.size();
        intervalBytes += packet.data.size();

        if (packet.silent) {
            ++totalSilentPackets;
            ++intervalSilentPackets;
        }
        if (packet.dataDiscontinuity) {
            ++totalDiscontinuities;
            ++intervalDiscontinuities;
        }
        if (packet.timestampError) {
            ++totalTimestampErrors;
            ++intervalTimestampErrors;
        }
        if (packet.samplesAnalyzed > 0) {
            intervalSamples += packet.samplesAnalyzed;
            intervalSquaredSamples +=
                static_cast<long double>(packet.rms) *
                static_cast<long double>(packet.rms) *
                static_cast<long double>(packet.samplesAnalyzed);
            intervalPeak = std::max(intervalPeak, packet.peak);
        }

        if (audioSender) {
            screenshare::UdpAudioPacket audioPacket =
                options.audioCodec == screenshare::udp_protocol::AudioCodec::Opus ?
                    BuildOpusUdpAudioPacket(packet, *opusEncoder) :
                    BuildRawUdpAudioPacket(packet, format, udpSampleFormat);
            audioSender->SendAudioPacket(audioPacket);
        }
    };

    while (!runtimeControl.StopRequested() && Clock::now() - startedAt < std::chrono::seconds(options.seconds)) {
        if (auto packet = capture.CapturePacket(std::chrono::milliseconds(100))) {
            recordPacket(*packet);
        } else {
            ++emptyPolls;
        }

        const auto now = Clock::now();
        if (now - lastReportAt >= std::chrono::seconds(1)) {
            const double elapsed = std::chrono::duration<double>(now - lastReportAt).count();
            const double packetsPerSecond = static_cast<double>(intervalPackets) / elapsed;
            const double audioFramesPerSecond = static_cast<double>(intervalFrames) / elapsed;
            const double intervalRms =
                intervalSamples == 0 ? 0.0 : std::sqrt(static_cast<double>(intervalSquaredSamples / intervalSamples));
            if (audioSender) {
                RecordLatestReceiverFeedback(
                    reportContext,
                    DrainUdpFeedback(*audioSender, std::chrono::milliseconds(0)));
            }
            const screenshare::UdpSenderStats udpStatsNow =
                audioSender ? audioSender->stats() : screenshare::UdpSenderStats{};

            std::cout
                << "audio_packets=" << totalPackets
                << " session=" << options.sessionId
                << " session_fingerprint=" << FormatSessionFingerprint(options.sessionFingerprint)
                << " audio_packets_per_second=" << packetsPerSecond
                << " audio_frames=" << totalFrames
                << " audio_frames_per_second=" << audioFramesPerSecond
                << " audio_bytes=" << totalBytes
                << " silent_packets=" << totalSilentPackets
                << " discontinuities=" << totalDiscontinuities
                << " timestamp_errors=" << totalTimestampErrors
                << " empty_polls=" << emptyPolls
                << " interval_packets=" << intervalPackets
                << " interval_frames=" << intervalFrames
                << " interval_bytes=" << intervalBytes
                << " interval_silent_packets=" << intervalSilentPackets
                << " interval_discontinuities=" << intervalDiscontinuities
                << " interval_timestamp_errors=" << intervalTimestampErrors
                << " peak=" << intervalPeak
                << " peak_dbfs=" << Dbfs(intervalPeak)
                << " rms=" << intervalRms
                << " rms_dbfs=" << Dbfs(intervalRms);
            if (audioSender) {
                std::cout
                    << " audio_udp_packets=" << udpStatsNow.audioPacketsSent
                    << " audio_udp_frames=" << udpStatsNow.audioFramesSent
                    << " audio_udp_datagrams=" << udpStatsNow.datagramsSent
                    << " audio_udp_queued_datagrams=" << udpStatsNow.audioDatagramsQueued
                    << " audio_udp_pending=" << udpStatsNow.pendingDatagrams
                    << " audio_udp_dropped_packets=" << udpStatsNow.audioPacketsDropped
                    << " audio_udp_wire_bytes=" << udpStatsNow.wireBytesSent
                    << " audio_codec=" << screenshare::udp_protocol::AudioCodecName(options.audioCodec)
                    << " udp_feedback_packets=" << udpStatsNow.feedbackPacketsReceived
                    << " udp_feedback_invalid=" << udpStatsNow.invalidFeedbackPackets
                    << " udp_feedback_access_rejected=" << udpStatsNow.feedbackAccessRejected
                    << " udp_feedback_crypto_rejected=" << udpStatsNow.feedbackCryptoRejected
                    << " udp_encryption=" << (udpStatsNow.encryptionEnabled ? "enabled" : "disabled")
                    << " udp_feedback_session=" << FeedbackSessionText(udpStatsNow)
                    << " udp_feedback_access=" << FeedbackAccessText(udpStatsNow);
            }
            std::cout
                << "\n" << std::flush;

            intervalPackets = 0;
            intervalFrames = 0;
            intervalBytes = 0;
            intervalSilentPackets = 0;
            intervalDiscontinuities = 0;
            intervalTimestampErrors = 0;
            intervalSamples = 0;
            intervalSquaredSamples = 0.0;
            intervalPeak = 0.0;
            lastReportAt = now;
        }
    }

    const double totalElapsed = std::chrono::duration<double>(Clock::now() - startedAt).count();
    screenshare::UdpSenderStats finalUdpStats;
    if (audioSender) {
        audioSender->Flush();
        RecordLatestReceiverFeedback(
            reportContext,
            DrainUdpFeedback(*audioSender, std::chrono::milliseconds(100)));
        finalUdpStats = audioSender->stats();
    }
    audioSender.reset();
    capture.Stop();

    std::cout
        << "Done. Audio packets: " << totalPackets
        << ", session: " << options.sessionId
        << ", session fingerprint: " << FormatSessionFingerprint(options.sessionFingerprint)
        << ", audio frames: " << totalFrames
        << ", average audio frames/sec: " << (totalElapsed == 0.0 ? 0.0 : static_cast<double>(totalFrames) / totalElapsed)
        << ", bytes: " << totalBytes
        << ", silent packets: " << totalSilentPackets
        << ", discontinuities: " << totalDiscontinuities
        << ", timestamp errors: " << totalTimestampErrors
        << ", empty polls: " << emptyPolls;
    if (!options.audioSendTarget.empty()) {
        std::cout
            << ", audio UDP packets: " << finalUdpStats.audioPacketsSent
            << ", audio UDP frames: " << finalUdpStats.audioFramesSent
            << ", audio UDP datagrams: " << finalUdpStats.datagramsSent
            << ", audio UDP dropped packets: " << finalUdpStats.audioPacketsDropped
            << ", audio UDP pending datagrams: " << finalUdpStats.pendingDatagrams
            << ", audio UDP wire bytes: " << finalUdpStats.wireBytesSent
            << ", audio codec: " << screenshare::udp_protocol::AudioCodecName(options.audioCodec)
            << ", UDP feedback packets: " << finalUdpStats.feedbackPacketsReceived
            << ", UDP invalid feedback packets: " << finalUdpStats.invalidFeedbackPackets
            << ", UDP feedback access rejected: " << finalUdpStats.feedbackAccessRejected
            << ", UDP feedback crypto rejected: " << finalUdpStats.feedbackCryptoRejected
            << ", UDP encryption: " << (finalUdpStats.encryptionEnabled ? "enabled" : "disabled")
            << ", UDP feedback session: " << FeedbackSessionText(finalUdpStats)
            << ", UDP feedback access: " << FeedbackAccessText(finalUdpStats);
    }
    std::cout
        << "\n";
}

void RunCaptureStats(
    const Options& options,
    SavedReportContext& reportContext,
    screenshare::ISessionRuntimeControl& runtimeControl)
{
    std::optional<screenshare::NatInvite> peerInvite;
    if (!options.peerInvite.empty()) {
        peerInvite = ParseValidatedPeerInvite(options);
    }

    StreamEncoderPreference streamEncoderPreference = options.streamEncoderPreference;

    screenshare::CaptureConfig config;
    config.sourceType = options.captureSourceType;
    config.displayIndex = options.displayIndex;
    config.windowHandle = options.windowHandle;
    config.targetWidth = options.width;
    config.targetHeight = options.height;
    config.targetFps = options.fps;
    config.backend = options.captureBackend;
    config.wgcBorderRequired = options.wgcBorderRequired;
    ConfigureCapturePayloads(config, options, streamEncoderPreference);
    config.hdrToSdr = options.hdrToSdr;
    config.hdrSdrWhiteNits = options.hdrSdrWhiteNits;
    config.hdrSdrBgraExposure = options.hdrSdrBgraExposure;

    screenshare::DesktopCapturer capturer;
    capturer.Start(config);

    WarnIfPlaintextUdpSession(options);

    if (options.captureSourceType == screenshare::CaptureSourceType::Window) {
        std::cout << "Capturing window 0x" << std::hex << options.windowHandle << std::dec;
    } else {
        std::cout << "Capturing display " << options.displayIndex;
    }
    std::cout << " at target " << options.fps << " FPS";

    if (options.width > 0 && options.height > 0) {
        std::cout << ", requested output " << options.width << "x" << options.height;
    } else {
        std::cout << ", requested output native resolution";
    }
    std::cout
        << ", capture backend " << CaptureBackendName(options.captureBackend)
        << ", session " << options.sessionId
        << " (" << FormatSessionFingerprint(options.sessionFingerprint) << ")";
    if (options.accessCodeProvided) {
        std::cout << ", access code required, UDP encryption enabled";
    }
    if (!options.recordPath.empty()) {
        std::cout << ", recording H.264 to " << options.recordPath;
    }
    if (!options.capturedBmpPath.empty()) {
        std::cout << ", dumping latest captured BMP to " << options.capturedBmpPath;
    }
    if (options.streamEncode) {
        std::cout << ", stream-encoding H.264 packets with " << StreamEncoderPreferenceName(streamEncoderPreference) << " encoder preference";
        if (options.keyframeIntervalSeconds > 0) {
            std::cout << ", keyframe interval " << options.keyframeIntervalSeconds << "s";
        } else {
            std::cout << ", encoder-default keyframe interval";
        }
    }
    if (!options.udpSendTarget.empty()) {
        std::cout << ", UDP sending to " << options.udpSendTarget;
        if (options.udpSendTargets.size() > 1) {
            std::cout << " plus " << (options.udpSendTargets.size() - 1) << " extra target(s)";
        }
        const size_t inviteTargetCount = static_cast<size_t>(std::count_if(
            options.udpSendTargetSpecs.begin(),
            options.udpSendTargetSpecs.end(),
            [](const UdpSendTargetSpec& target) {
                return target.fromPeerInvite;
            }));
        if (inviteTargetCount > 0) {
            std::cout << ", NAT invite targets " << inviteTargetCount;
        }
        if (peerInvite && options.udpSendTargetFromPeerInvite) {
            std::cout << " from peer invite endpoint=" << options.udpSendPeerInviteEndpoint;
        }
        if (options.udpLocalPort != 0) {
            std::cout << ", UDP local port " << options.udpLocalPort;
            if (options.udpLocalPortFromLocalInvite) {
                std::cout << " from local invite";
            }
        } else if (options.udpSendTargetFromPeerInvite) {
            std::cout << ", UDP local port auto (use --udp-local-port to match this side's invite)";
        }
        std::cout << ", UDP pacing " << (options.udpPacing ? "enabled" : "disabled");
        if (options.udpMaxQueueMs == 0) {
            std::cout << ", UDP live queue cap disabled";
        } else {
            std::cout << ", UDP live queue cap " << options.udpMaxQueueMs << "ms";
        }
        std::cout << ", adaptive bitrate " << (options.adaptBitrate ? "enabled" : "advice-only");
        if (options.adaptMinBitrateProvided) {
            std::cout << ", adaptive minimum " << Mbps(options.adaptMinBitrate) << " Mbps";
        }
        std::cout << ", adaptive reduce cooldown " << options.adaptReduceCooldownSeconds << "s";
        if (options.adaptResolution) {
            std::cout
                << ", adaptive resolution enabled"
                << ", min scale " << options.adaptResolutionMinScale
                << ", resolution cooldown " << options.adaptResolutionCooldownSeconds << "s";
        }
    } else if (options.shareRoom) {
        std::cout
            << ", live signaling room sharing on UDP local port " << options.udpLocalPort
            << ", waiting for room peers";
    }
    if (options.audioCapture) {
        std::cout
            << ", capturing "
            << screenshare::AudioCaptureSourceName(options.audioCaptureSource)
            << " audio for UDP";
        if (!options.audioDeviceId.empty()) {
            std::cout << ", selected audio device id";
        }
        if (options.audioCaptureSource == screenshare::AudioCaptureSource::ProcessOutput && options.audioProcessId != 0) {
            std::cout << ", process " << options.audioProcessId;
        }
        std::cout << ", audio codec " << screenshare::udp_protocol::AudioCodecName(options.audioCodec);
    }
    if (options.captureBackend == screenshare::CaptureBackend::WindowsGraphicsCapture) {
        std::cout << ", WGC border " << (options.wgcBorderRequired ? "enabled" : "disabled when permitted");
    }
    if (options.hdrToSdr) {
        std::cout
            << ", HDR-to-SDR enabled at " << options.hdrSdrWhiteNits << " nits"
            << ", HDR desktop exposure " << options.hdrSdrBgraExposure;
    } else {
        std::cout << ", HDR-to-SDR disabled";
    }
    std::cout << ".\n";

    using Clock = std::chrono::steady_clock;
    const auto startedAt = Clock::now();
    auto lastReportAt = startedAt;
    uint64_t totalOutputFrames = 0;
    uint64_t intervalOutputFrames = 0;
    uint64_t totalDesktopUpdates = 0;
    uint64_t intervalDesktopUpdates = 0;
    uint64_t totalRepeatedFrames = 0;
    uint64_t intervalRepeatedFrames = 0;
    int lastSourceWidth = 0;
    int lastSourceHeight = 0;
    int lastOutputWidth = 0;
    int lastOutputHeight = 0;
    DXGI_FORMAT lastSourceFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT lastOutputFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_COLOR_SPACE_TYPE lastDisplayColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool lastDisplayHdrActive = false;
    uint32_t lastColorConversionMode = 0;
    bool lastNv12GeneratedOnGpu = false;
    bool lastNv12TextureAvailable = false;
    bool hasFrame = false;
    screenshare::CapturedFrame lastFrame;
    std::unique_ptr<screenshare::H264FileEncoder> fileEncoder;
    std::unique_ptr<screenshare::H264StreamEncoder> streamEncoder;
    uint64_t fileEncodedFrames = 0;
    uint64_t streamEncodedFrames = 0;
    uint64_t streamPackets = 0;
    uint64_t streamBytes = 0;
    uint32_t streamBitrate = 0;
    uint32_t streamTargetBitrate = 0;
    int streamEncoderWidth = 0;
    int streamEncoderHeight = 0;
    bool autoBitrateEnabled = options.bitrate == 0;
    uint64_t bitrateAdaptations = 0;
    uint64_t bitrateAdaptationFailures = 0;
    uint32_t lastBitrateAdaptationAttempt = 0;
    bool adaptiveBitrateEnabled = options.adaptBitrate;
    const char* bitrateAdaptationStatus = adaptiveBitrateEnabled ? "waiting" : "disabled";
    AdaptiveBitrateAdvisor bitrateAdvisor;
    std::vector<ResolutionTier> adaptiveResolutionTiers;
    size_t adaptiveResolutionTierIndex = 0;
    bool adaptiveResolutionEnabled = options.adaptResolution;
    uint64_t resolutionAdaptations = 0;
    uint64_t resolutionAdaptationFailures = 0;
    int resolutionCooldownRemaining = 0;
    uint32_t resolutionStableFeedbackReports = 0;
    uint32_t resolutionReductionPressureReports = 0;
    const char* resolutionAdaptationStatus = adaptiveResolutionEnabled ? "waiting" : "disabled";
    int runtimeFps = options.fps;
    uint32_t streamKeyframeIntervalFrames =
        options.keyframeIntervalSeconds <= 0 ?
        0 :
        static_cast<uint32_t>(options.keyframeIntervalSeconds * runtimeFps);
    double intervalCaptureMs = 0.0;
    uint64_t intervalCaptureCalls = 0;
    double intervalStreamEncodeMs = 0.0;
    uint64_t intervalStreamEncodeCalls = 0;
    double totalCaptureMs = 0.0;
    uint64_t totalCaptureCalls = 0;
    double totalStreamEncodeMs = 0.0;
    uint64_t totalStreamEncodeCalls = 0;
    bool capturedBmpWritten = false;
    std::unique_ptr<UdpSenderFanout> udpSender;
    std::unique_ptr<AudioUdpCaptureWorker> audioCaptureWorker;
    std::unique_ptr<LiveSignalingRuntime> liveSignalingRuntime;
    std::vector<UdpSendTargetSpec> udpSendTargetSpecs = options.udpSendTargetSpecs;
    std::set<std::string> liveSignalingSendTargets;
    uint64_t audioPacingBitrate = 0;
    bool audioPacingApplied = false;
    bool runtimeVideoPaused = options.videoPaused;
    bool runtimeAudioCaptureEnabled = options.audioCapture;
    std::string runtimeAudioDeviceId = options.audioDeviceId;
    uint32_t runtimeAudioProcessId =
        options.audioProcessId != 0 ? options.audioProcessId : options.windowProcessId;
    auto selectRuntimeAudioSource = [&]() {
        if (options.audioCaptureSource == screenshare::AudioCaptureSource::Microphone) {
            return screenshare::AudioCaptureSource::Microphone;
        }
        if (!runtimeAudioDeviceId.empty()) {
            return screenshare::AudioCaptureSource::SystemOutput;
        }
        if (config.sourceType == screenshare::CaptureSourceType::Window && runtimeAudioProcessId != 0) {
            return screenshare::AudioCaptureSource::ProcessOutput;
        }
        return options.audioCaptureSource == screenshare::AudioCaptureSource::ProcessOutput ?
            screenshare::AudioCaptureSource::ProcessOutput :
            screenshare::AudioCaptureSource::SystemOutput;
    };
    screenshare::AudioCaptureSource runtimeAudioSource = selectRuntimeAudioSource();

    for (const auto& target : udpSendTargetSpecs) {
        liveSignalingSendTargets.insert(target.target);
    }
    if (options.signalingLive && options.shareRoom) {
        liveSignalingRuntime = std::make_unique<LiveSignalingRuntime>();
        liveSignalingRuntime->Start(options);
    }

    auto streamUdpPacingBitrate = [&]() -> uint64_t {
        if (streamBitrate == 0) {
            return 0;
        }
        return (static_cast<uint64_t>(streamBitrate) * DefaultUdpPacingHeadroomPercent + 99) / 100;
    };

    auto combinedUdpPacingBitrate = [&]() -> uint32_t {
        const uint64_t combined =
            streamUdpPacingBitrate() +
            (runtimeAudioCaptureEnabled ? audioPacingBitrate : 0ULL);
        return static_cast<uint32_t>(std::min<uint64_t>(
            combined,
            std::numeric_limits<uint32_t>::max()));
    };

    auto updateUdpPacingBitrate = [&]() {
        if (udpSender) {
            udpSender->SetPacingBitrate(combinedUdpPacingBitrate());
        }
    };

    auto stopAudioCaptureWorker = [&]() {
        if (audioCaptureWorker) {
            audioCaptureWorker->Stop();
            audioCaptureWorker.reset();
        }
    };

    auto startAudioCaptureWorker = [&]() {
        if (!udpSender || !runtimeAudioCaptureEnabled) {
            return;
        }
        audioCaptureWorker = std::make_unique<AudioUdpCaptureWorker>();
        audioCaptureWorker->Start(
            [udpSender = udpSender.get()](const screenshare::UdpAudioPacket& packet) {
                udpSender->SendAudioPacket(packet);
            },
            runtimeAudioSource,
            runtimeAudioDeviceId,
            runtimeAudioProcessId,
            options.audioCodec);
        std::cout
            << "Audio capture worker started source="
            << screenshare::AudioCaptureSourceName(runtimeAudioSource)
            << (runtimeAudioDeviceId.empty() ? "" : " device=selected")
            << (runtimeAudioSource == screenshare::AudioCaptureSource::ProcessOutput ?
                std::string(" process=") + std::to_string(runtimeAudioProcessId) :
                std::string())
            << "\n";
    };

    auto restartAudioCaptureWorker = [&]() {
        stopAudioCaptureWorker();
        startAudioCaptureWorker();
        updateUdpPacingBitrate();
    };

    auto ensureUdpSenderForTargets = [&]() {
        const bool hasUdpSendTargets =
            !udpSendTargetSpecs.empty() || !options.udpSendTarget.empty();
        if (!hasUdpSendTargets) {
            return;
        }
        if (udpSender) {
            updateUdpPacingBitrate();
            return;
        }

        std::vector<UdpSendTargetSpec> udpTargets = udpSendTargetSpecs;
        if (udpTargets.empty()) {
            udpTargets.push_back(UdpSendTargetSpec{
                options.udpSendTarget,
                false,
                "direct",
                options.udpLocalPort,
                false,
                false,
                false,
                0});
        }

        std::vector<screenshare::UdpSenderConfig> udpConfigs;
        std::vector<std::pair<uint32_t, std::string>> viewerNames;
        udpConfigs.reserve(udpTargets.size());
        for (size_t targetIndex = 0; targetIndex < udpTargets.size(); ++targetIndex) {
            const auto& target = udpTargets[targetIndex];
            auto udpConfig = screenshare::ParseUdpSenderTarget(target.target);
            const uint32_t targetGroup =
                target.group != 0 ? target.group : static_cast<uint32_t>(targetIndex + 1);
            if (!target.displayName.empty()) {
                viewerNames.push_back({targetGroup, target.displayName});
            }
            if (targetIndex > 0 &&
                target.fromPeerInvite &&
                target.localPort == 0 &&
                !udpConfigs.empty() &&
                udpConfigs.front().collectNatProbeTargets) {
                udpConfigs.front().additionalTargets.push_back(
                    screenshare::UdpSenderEndpoint{udpConfig.host, udpConfig.port, targetGroup});
                continue;
            }
            udpConfig.group = targetGroup;
            udpConfig.localPort = target.localPort;
            udpConfig.pacingEnabled = options.udpPacing;
            udpConfig.pacingBitrate = combinedUdpPacingBitrate();
            udpConfig.maxQueueDelay = std::chrono::milliseconds(options.udpMaxQueueMs);
            udpConfig.accessCodeFingerprint = options.accessCodeFingerprint;
            udpConfig.encryptionKey = options.accessCodeKey;
            udpConfig.retargetOnNatProbe =
                target.fromPeerInvite &&
                options.inviteEndpointPreference == InviteEndpointPreference::Auto;
            udpConfig.collectNatProbeTargets =
                udpConfig.retargetOnNatProbe && target.collectNatProbeTargets;
            udpConfig.preferNatProbeTargets =
                udpConfig.collectNatProbeTargets && target.preferNatProbeTargets;
            udpConfig.natProbeSessionFingerprint =
                target.natProbeSessionFingerprint != 0 ?
                    target.natProbeSessionFingerprint :
                    NatProbeSessionFingerprint(options);
            if (runtimeAudioCaptureEnabled) {
                udpConfig.maxQueuedDatagrams = 16'384;
            }
            udpConfigs.push_back(std::move(udpConfig));
        }

        udpSender = std::make_unique<UdpSenderFanout>();
        udpSender->Open(udpConfigs);
        for (const auto& [group, name] : viewerNames) {
            udpSender->SetViewerName(group, name);
        }
        const size_t inviteTargetCount = static_cast<size_t>(std::count_if(
            udpTargets.begin(),
            udpTargets.end(),
            [](const UdpSendTargetSpec& target) {
                return target.fromPeerInvite;
            }));
        const bool anyRetarget = std::any_of(
            udpConfigs.begin(),
            udpConfigs.end(),
            [](const screenshare::UdpSenderConfig& config) {
                return config.retargetOnNatProbe;
            });
        const bool anyNatProbeFanout = std::any_of(
            udpConfigs.begin(),
            udpConfigs.end(),
            [](const screenshare::UdpSenderConfig& config) {
                return config.collectNatProbeTargets;
            });
        const bool anyLocalInvitePort = std::any_of(
            udpTargets.begin(),
            udpTargets.end(),
            [](const UdpSendTargetSpec& target) {
                return target.localPortFromLocalInvite;
            });
        std::string inviteEndpoint = "none";
        if (inviteTargetCount == 1) {
            const auto iterator = std::find_if(
                udpTargets.begin(),
                udpTargets.end(),
                [](const UdpSendTargetSpec& target) {
                    return target.fromPeerInvite;
                });
            if (iterator != udpTargets.end()) {
                inviteEndpoint = iterator->inviteEndpoint;
            }
        } else if (inviteTargetCount > 1) {
            inviteEndpoint = "mixed";
        }
        std::cout
            << "UDP sender pacing=" << (udpConfigs.front().pacingEnabled ? "enabled" : "disabled")
            << " targets=" << udpTargets.size()
            << " target_source="
            << (inviteTargetCount == 0 ? "direct" :
                (inviteTargetCount == udpTargets.size() ? "peer_invite" : "mixed"))
            << " invite_targets=" << inviteTargetCount
            << " invite_endpoint=" << inviteEndpoint
            << " nat_probe_retarget=" << (anyRetarget ? "enabled" : "disabled")
            << " nat_probe_fanout=" << (anyNatProbeFanout ? "enabled" : "disabled")
            << " bitrate_mbps=" << Mbps(udpConfigs.front().pacingBitrate)
            << " local_port="
            << (udpConfigs.front().localPort == 0 ? std::string("auto") : std::to_string(udpConfigs.front().localPort))
            << " local_port_source=" << (anyLocalInvitePort ? "local_invite" : "option_or_auto")
            << " max_queue_ms=" << udpConfigs.front().maxQueueDelay.count()
            << " adaptive_bitrate=" << (adaptiveBitrateEnabled ? "enabled" : "advice-only")
            << " adapt_min_bitrate_mbps=" << Mbps(bitrateAdvisor.minBitrate())
            << " adapt_reduce_cooldown_s=" << options.adaptReduceCooldownSeconds
            << " max_queued_datagrams=" << udpConfigs.front().maxQueuedDatagrams
            << " access_code=" << (options.accessCodeProvided ? "required" : "none")
            << "\n";
        startAudioCaptureWorker();
    };

    auto sendStreamPackets = [&](const std::vector<screenshare::EncodedPacket>& packets) {
        streamPackets += packets.size();
        for (const auto& packet : packets) {
            streamBytes += packet.bytes.size();
            if (udpSender) {
                udpSender->SendFrame(packet);
            }
        }
    };

    auto resetStreamEncoder = [&]() {
        streamEncoder.reset();
        streamEncoderWidth = 0;
        streamEncoderHeight = 0;
    };

    auto drainLiveSignalingSendTargets = [&]() {
        if (!liveSignalingRuntime) {
            return;
        }

        for (const auto& peer : liveSignalingRuntime->DrainRemovedPeers()) {
            const std::string endpoint = SignalingCandidateEndpoint(peer.candidate);
            const bool known = liveSignalingSendTargets.erase(endpoint) > 0;
            udpSendTargetSpecs.erase(
                std::remove_if(
                    udpSendTargetSpecs.begin(),
                    udpSendTargetSpecs.end(),
                    [&](const UdpSendTargetSpec& target) {
                        return target.target == endpoint;
                    }),
                udpSendTargetSpecs.end());
            if (udpSender) {
                udpSender->SuppressEndpoint(endpoint);
            }

            std::cout
                << "signaling_live_sender_peer=removed"
                << " room=" << options.signalingRoomId
                << " peer_id=" << peer.peerId
                << " endpoint=" << endpoint
                << " active=" << (known ? "suppressed" : "unknown")
                << "\n";
        }

        const uint64_t probeSession = NatProbeSessionFingerprint(options);
        for (const auto& peer : liveSignalingRuntime->DrainDiscoveredPeers()) {
            const std::string endpoint = SignalingCandidateEndpoint(peer.candidate);
            if (!liveSignalingSendTargets.insert(endpoint).second) {
                continue;
            }
            if (udpSender) {
                udpSender->UnsuppressEndpoint(endpoint);
            }

            UdpSendTargetSpec target = SignalingSendTargetSpec(
                peer.candidate,
                udpSendTargetSpecs.empty() ? options.udpLocalPort : static_cast<uint16_t>(0),
                probeSession,
                SignalingPeerGroup(peer.peerId),
                peer.displayName);
            udpSendTargetSpecs.push_back(target);

            bool active = false;
            if (udpSender) {
                udpSender->SetViewerName(target.group, target.displayName);
                active = udpSender->AddAdditionalTarget(
                    screenshare::UdpSenderEndpoint{peer.candidate.ip, peer.candidate.port, target.group});
            }

            std::cout
                << "signaling_live_sender_peer=added"
                << " room=" << options.signalingRoomId
                << " peer_id=" << peer.peerId
                << " endpoint=" << endpoint
                << " name=" << (peer.displayName.empty() ? "none" : LogTokenEncode(peer.displayName))
                << " active=" << (udpSender ? (active ? "yes" : "duplicate") : "pending")
                << "\n";
        }
    };

    auto applyAdaptiveBitrate = [&]() {
        if (!adaptiveBitrateEnabled || !udpSender || !streamEncoder || !bitrateAdvisor.configured()) {
            return;
        }

        const uint32_t recommendedBitrate = bitrateAdvisor.recommendedBitrate();
        if (recommendedBitrate == 0 || streamBitrate == 0) {
            bitrateAdaptationStatus = "waiting";
            return;
        }
        const bool shouldReduce =
            std::strcmp(bitrateAdvisor.action(), "reduce") == 0 &&
            recommendedBitrate < streamBitrate;
        const bool shouldIncrease =
            std::strcmp(bitrateAdvisor.action(), "increase") == 0 &&
            recommendedBitrate > streamBitrate;
        if (!shouldReduce && !shouldIncrease) {
            bitrateAdaptationStatus = "holding";
            return;
        }
        if (recommendedBitrate == lastBitrateAdaptationAttempt) {
            return;
        }

        lastBitrateAdaptationAttempt = recommendedBitrate;
        if (streamEncoder->TryUpdateBitrate(recommendedBitrate)) {
            streamBitrate = recommendedBitrate;
            updateUdpPacingBitrate();
            ++bitrateAdaptations;
            bitrateAdaptationStatus = shouldIncrease ? "applied_increase" : "applied_reduce";
            std::cout
                << "Adaptive bitrate applied bitrate_mbps=" << Mbps(streamBitrate)
                << " direction=" << (shouldIncrease ? "increase" : "reduce")
                << " reason=" << bitrateAdvisor.reason()
                << "\n";
        } else {
            ++bitrateAdaptationFailures;
            bitrateAdaptationStatus = "unsupported";
            std::cerr
                << "Adaptive bitrate update unsupported by active encoder; keeping bitrate_mbps="
                << Mbps(streamBitrate)
                << "\n";
        }
    };

    auto restartCapturePipeline = [&](const char* reason) {
        try {
            if (streamEncoder) {
                sendStreamPackets(streamEncoder->Drain());
                resetStreamEncoder();
            }

            ConfigureCapturePayloads(config, options, streamEncoderPreference);
            capturer.Stop();
            capturer.Start(config);
            hasFrame = false;
            std::cout
                << "Runtime capture restarted"
                << " source=" << (config.sourceType == screenshare::CaptureSourceType::Window ? "window" : "display")
                << " display=" << config.displayIndex
                << " window=0x" << std::hex << config.windowHandle << std::dec
                << " output="
                << (config.targetWidth > 0 && config.targetHeight > 0 ?
                    std::to_string(config.targetWidth) + "x" + std::to_string(config.targetHeight) :
                    std::string("native"))
                << " fps=" << runtimeFps
                << " reason=" << reason
                << "\n";
        } catch (const std::exception& error) {
            std::cerr << "Runtime capture restart failed: " << error.what() << "\n";
            throw;
        }
    };

    auto restartStreamOutput = [&](int width, int height, double scale, const char* direction, const char* reason) {
        try {
            config.targetWidth = width;
            config.targetHeight = height;
            restartCapturePipeline(reason);
            resolutionCooldownRemaining = options.adaptResolutionCooldownSeconds;
            resolutionStableFeedbackReports = 0;
            resolutionReductionPressureReports = 0;
            ++resolutionAdaptations;
            if (std::strcmp(direction, "manual") == 0) {
                resolutionAdaptationStatus = "manual";
            } else if (std::strcmp(direction, "auto") == 0) {
                resolutionAdaptationStatus = "waiting";
            } else {
                resolutionAdaptationStatus =
                    std::strcmp(direction, "increase") == 0 ? "applied_increase" : "applied_reduce";
            }
            std::cout
                << "Runtime resolution applied output="
                << (width > 0 && height > 0 ? std::to_string(width) + "x" + std::to_string(height) : std::string("native"))
                << " scale=" << scale
                << " direction=" << direction
                << " bitrate_mbps=" << Mbps(streamBitrate)
                << " reason=" << reason
                << "\n";
        } catch (const std::exception& error) {
            ++resolutionAdaptationFailures;
            resolutionAdaptationStatus = "failed";
            std::cerr << "Adaptive resolution update failed: " << error.what() << "\n";
            throw;
        }
    };

    auto restartStreamAtResolution = [&](size_t tierIndex, const char* direction, const char* reason) {
        if (tierIndex >= adaptiveResolutionTiers.size()) {
            return;
        }

        const auto& tier = adaptiveResolutionTiers[tierIndex];
        restartStreamOutput(tier.width, tier.height, tier.scale, direction, reason);
        adaptiveResolutionTierIndex = tierIndex;
    };

    auto applyAdaptiveResolution = [&](const screenshare::UdpSenderStats& udpStats) {
        if (!adaptiveResolutionEnabled || adaptiveResolutionTiers.size() < 2 || !streamEncoder || !bitrateAdvisor.configured()) {
            return;
        }

        if (resolutionCooldownRemaining > 0) {
            --resolutionCooldownRemaining;
            resolutionAdaptationStatus = "cooldown";
            return;
        }

        const bool atBitrateFloor =
            streamBitrate > 0 &&
            bitrateAdvisor.minBitrate() > 0 &&
            streamBitrate <= bitrateAdvisor.minBitrate();
        const bool pressureAtFloor =
            atBitrateFloor &&
            (std::strcmp(bitrateAdvisor.action(), "reduce") == 0 ||
             std::strcmp(bitrateAdvisor.reason(), "min_bitrate") == 0);
        const bool senderQueueVeryStale =
            udpStats.pendingQueueDelayMs >= ResolutionSenderQueuePressureMs ||
            (udpStats.pendingQueueDelayMs == 0 &&
             udpStats.pendingDatagrams >= SenderQueuePressureDatagrams * 2);
        const bool bitrateReducedEnoughForResolution =
            bitrateAdvisor.targetBitrate() > 0 &&
            streamBitrate > 0 &&
            static_cast<uint64_t>(streamBitrate) * 100 <=
                static_cast<uint64_t>(bitrateAdvisor.targetBitrate()) *
                ResolutionBitrateReducedPercentBeforeQueueScale;
        const bool queuePressureReason =
            std::strcmp(bitrateAdvisor.action(), "reduce") == 0 ||
            std::strcmp(bitrateAdvisor.reason(), "queue_pressure") == 0 ||
            std::strcmp(bitrateAdvisor.reason(), "queue_stabilizing") == 0 ||
            std::strcmp(bitrateAdvisor.reason(), "reduce_cooldown") == 0 ||
            std::strcmp(bitrateAdvisor.reason(), "min_bitrate") == 0;
        const bool pressureAtReducedBitrate =
            senderQueueVeryStale &&
            bitrateReducedEnoughForResolution &&
            queuePressureReason;
        const bool reductionPressure =
            std::strcmp(bitrateAdvisor.action(), "reduce") == 0 ||
            std::strcmp(bitrateAdvisor.reason(), "reduce_cooldown") == 0;

        if (pressureAtFloor || pressureAtReducedBitrate) {
            resolutionStableFeedbackReports = 0;
            ++resolutionReductionPressureReports;
            if (resolutionReductionPressureReports < ResolutionPressureReportsBeforeReduce) {
                resolutionAdaptationStatus = "stabilizing_reduce";
                return;
            }
            if (adaptiveResolutionTierIndex + 1 < adaptiveResolutionTiers.size()) {
                restartStreamAtResolution(
                    adaptiveResolutionTierIndex + 1,
                    "reduce",
                    pressureAtReducedBitrate ? "sender_queue_pressure" : bitrateAdvisor.reason());
                return;
            }
            resolutionAdaptationStatus = "holding";
            return;
        }

        if (reductionPressure) {
            resolutionStableFeedbackReports = 0;
            resolutionReductionPressureReports = 0;
            resolutionAdaptationStatus = "holding";
            return;
        }

        if (adaptiveResolutionTierIndex > 0) {
            const bool pendingBitrateIncrease =
                std::strcmp(bitrateAdvisor.action(), "increase") == 0 &&
                streamBitrate < bitrateAdvisor.recommendedBitrate();
            const bool stableForUpscale =
                streamBitrate > bitrateAdvisor.minBitrate() &&
                !pendingBitrateIncrease &&
                std::strcmp(bitrateAdvisor.reason(), "waiting_for_feedback") != 0 &&
                std::strcmp(bitrateAdvisor.reason(), "min_bitrate") != 0 &&
                std::strcmp(bitrateAdvisor.reason(), "queue_stabilizing") != 0 &&
                std::strcmp(bitrateAdvisor.reason(), "preview_stabilizing") != 0;

            if (stableForUpscale) {
                ++resolutionStableFeedbackReports;
                resolutionReductionPressureReports = 0;
            } else {
                resolutionStableFeedbackReports = 0;
            }

            if (resolutionStableFeedbackReports < ResolutionStableReportsBeforeUpscale) {
                resolutionAdaptationStatus =
                    resolutionStableFeedbackReports == 0 ? "holding" : "stabilizing_increase";
                return;
            }
            restartStreamAtResolution(adaptiveResolutionTierIndex - 1, "increase", "stable_resolution_feedback");
            return;
        }

        resolutionStableFeedbackReports = 0;
        resolutionReductionPressureReports = 0;
        resolutionAdaptationStatus = "holding";
    };

    auto targetFrameTime = std::chrono::microseconds(1'000'000 / runtimeFps);
    auto nextFrameAt = Clock::now();

    auto applyRuntimeStreamSettingsControl = [&]() {
        const auto request = runtimeControl.TakeStreamSettingsRequest();
        if (!request) {
            return;
        }

        bool restartCapture = false;
        if (request->roomName) {
            if (liveSignalingRuntime) {
                liveSignalingRuntime->UpdateRoomName(*request->roomName);
            }
            std::cout << "runtime_room_name=updated\n";
        }
        if (request->captureSourceType) {
            const auto requestedSource = *request->captureSourceType == screenshare::RuntimeCaptureSourceType::Window
                ? screenshare::CaptureSourceType::Window
                : screenshare::CaptureSourceType::Display;
            if (config.sourceType != requestedSource) {
                config.sourceType = requestedSource;
                restartCapture = true;
                adaptiveResolutionTiers.clear();
                adaptiveResolutionTierIndex = 0;
                std::cout << "runtime_capture_source="
                          << (config.sourceType == screenshare::CaptureSourceType::Window ? "window" : "display")
                          << "\n";
            }
        }
        if (request->displayIndex && *request->displayIndex >= 0 && config.displayIndex != *request->displayIndex) {
            config.displayIndex = *request->displayIndex;
            restartCapture = true;
            adaptiveResolutionTiers.clear();
            adaptiveResolutionTierIndex = 0;
            std::cout << "runtime_display=" << config.displayIndex << "\n";
        }
        if (request->windowHandle && *request->windowHandle != 0 && config.windowHandle != *request->windowHandle) {
            config.windowHandle = *request->windowHandle;
            restartCapture = true;
            adaptiveResolutionTiers.clear();
            adaptiveResolutionTierIndex = 0;
            std::cout << "runtime_window=0x" << std::hex << config.windowHandle << std::dec << "\n";
        }
        bool audioProcessChanged = false;
        if (request->windowProcessId && runtimeAudioProcessId != *request->windowProcessId) {
            runtimeAudioProcessId = *request->windowProcessId;
            audioProcessChanged = true;
            std::cout << "runtime_window_process=" << runtimeAudioProcessId << "\n";
        }
        if (request->fps && *request->fps > 0 && *request->fps <= 240 && runtimeFps != *request->fps) {
            runtimeFps = *request->fps;
            config.targetFps = runtimeFps;
            targetFrameTime = std::chrono::microseconds(1'000'000 / runtimeFps);
            streamKeyframeIntervalFrames =
                options.keyframeIntervalSeconds <= 0 ?
                0 :
                static_cast<uint32_t>(options.keyframeIntervalSeconds * runtimeFps);
            nextFrameAt = Clock::now();
            restartCapture = true;
            std::cout << "runtime_fps=" << runtimeFps << "\n";
        }
        if (request->bitrateBps) {
            const bool requestedAutoBitrate = *request->bitrateBps == 0;
            uint32_t requestedBitrate = *request->bitrateBps;
            if (requestedAutoBitrate) {
                requestedBitrate = hasFrame ?
                    SelectBitrate(options, lastFrame.width, lastFrame.height) :
                    SelectBitrate(options, config.targetWidth, config.targetHeight);
            }
            if (requestedBitrate == 0) {
                requestedBitrate = streamTargetBitrate;
            }
            if (requestedBitrate == 0) {
                std::cout << "runtime_bitrate_mode=auto pending=true\n";
            } else if (autoBitrateEnabled != requestedAutoBitrate || streamTargetBitrate != requestedBitrate) {
                autoBitrateEnabled = requestedAutoBitrate;
                streamTargetBitrate = requestedBitrate;
                lastBitrateAdaptationAttempt = 0;
                bitrateAdvisor.Configure(
                    streamTargetBitrate,
                    SelectAdaptiveMinBitrate(options, streamTargetBitrate, adaptiveResolutionTiers),
                    static_cast<uint32_t>(options.adaptReduceCooldownSeconds));
                if (streamEncoder && streamBitrate != streamTargetBitrate) {
                    if (streamEncoder->TryUpdateBitrate(streamTargetBitrate)) {
                        streamBitrate = streamTargetBitrate;
                        updateUdpPacingBitrate();
                        ++bitrateAdaptations;
                        bitrateAdaptationStatus = autoBitrateEnabled ? "auto" : "manual";
                        std::cout
                            << "runtime_bitrate_mbps=" << Mbps(streamBitrate)
                            << " runtime_bitrate_mode=" << (autoBitrateEnabled ? "auto" : "manual")
                            << "\n";
                    } else {
                        ++bitrateAdaptationFailures;
                        bitrateAdaptationStatus = "unsupported";
                        std::cerr
                            << "Runtime bitrate update unsupported by active encoder; keeping bitrate_mbps="
                            << Mbps(streamBitrate)
                            << "\n";
                    }
                } else {
                    std::cout
                        << "runtime_bitrate_target_mbps=" << Mbps(streamTargetBitrate)
                        << " runtime_bitrate_mode=" << (autoBitrateEnabled ? "auto" : "manual")
                        << "\n";
                }
            }
        }
        if (request->adaptBitrate) {
            adaptiveBitrateEnabled = *request->adaptBitrate;
            bitrateAdaptationStatus = adaptiveBitrateEnabled ? "waiting" : "disabled";
            std::cout
                << "runtime_adaptive_bitrate="
                << (adaptiveBitrateEnabled ? "enabled" : "disabled")
                << "\n";
        }
        if (request->adaptFps) {
            std::cout
                << "runtime_adaptive_fps="
                << (*request->adaptFps ? "requested" : "disabled")
                << " status=unsupported\n";
        }
        if (request->videoPaused && runtimeVideoPaused != *request->videoPaused) {
            runtimeVideoPaused = *request->videoPaused;
            nextFrameAt = Clock::now();
            std::cout
                << "runtime_video=" << (runtimeVideoPaused ? "paused" : "running")
                << "\n";
        }
        if (request->captureSystemAudio ||
            request->hostAudioMuted ||
            request->audioDeviceId ||
            request->captureSourceType ||
            request->windowProcessId) {
            bool nextAudioCapture = runtimeAudioCaptureEnabled;
            if (request->captureSystemAudio) {
                nextAudioCapture = *request->captureSystemAudio;
            }
            if (request->hostAudioMuted && *request->hostAudioMuted) {
                nextAudioCapture = false;
            }
            std::string nextAudioDeviceId = runtimeAudioDeviceId;
            if (request->audioDeviceId) {
                nextAudioDeviceId = *request->audioDeviceId;
            }
            const auto previousAudioSource = runtimeAudioSource;
            bool audioDeviceChanged = false;
            if (nextAudioDeviceId != runtimeAudioDeviceId) {
                runtimeAudioDeviceId = std::move(nextAudioDeviceId);
                audioDeviceChanged = true;
            }
            runtimeAudioSource = selectRuntimeAudioSource();
            if (nextAudioCapture != runtimeAudioCaptureEnabled ||
                runtimeAudioSource != previousAudioSource ||
                audioProcessChanged ||
                audioDeviceChanged) {
                runtimeAudioCaptureEnabled = nextAudioCapture;
                restartAudioCaptureWorker();
                std::cout
                    << "runtime_system_audio=" << (runtimeAudioCaptureEnabled ? "enabled" : "disabled")
                    << " runtime_audio_source=" << screenshare::AudioCaptureSourceName(runtimeAudioSource)
                    << (runtimeAudioDeviceId.empty() ? "" : " runtime_audio_device=selected")
                    << (runtimeAudioSource == screenshare::AudioCaptureSource::ProcessOutput ?
                        std::string(" runtime_audio_process=") + std::to_string(runtimeAudioProcessId) :
                        std::string())
                    << "\n";
            }
        }
        if (request->adaptResolution && !request->resolution) {
            adaptiveResolutionEnabled = *request->adaptResolution;
            if (!adaptiveResolutionEnabled) {
                adaptiveResolutionTiers.clear();
                adaptiveResolutionTierIndex = 0;
                resolutionAdaptationStatus = "disabled";
            } else {
                resolutionAdaptationStatus = "waiting";
            }
            std::cout
                << "runtime_adaptive_resolution="
                << (adaptiveResolutionEnabled ? "enabled" : "disabled")
                << "\n";
        }
        if (request->resolution) {
            const auto& resolution = *request->resolution;
            switch (resolution.mode) {
            case screenshare::RuntimeResolutionMode::Auto:
                {
                const bool requestedAdaptiveResolution = request->adaptResolution.value_or(true);
                if (!restartCapture &&
                    adaptiveResolutionEnabled == requestedAdaptiveResolution &&
                    config.targetWidth == 0 &&
                    config.targetHeight == 0) {
                    return;
                }
                adaptiveResolutionEnabled = requestedAdaptiveResolution;
                adaptiveResolutionTiers.clear();
                adaptiveResolutionTierIndex = 0;
                restartStreamOutput(0, 0, 1.0, adaptiveResolutionEnabled ? "auto" : "manual", "runtime_control");
                if (!adaptiveResolutionEnabled) {
                    resolutionAdaptationStatus = "disabled";
                }
                std::cout
                    << "runtime_resolution_mode=" << (adaptiveResolutionEnabled ? "auto" : "native")
                    << "\n";
                break;
                }
            case screenshare::RuntimeResolutionMode::Native:
                if (!restartCapture && !adaptiveResolutionEnabled && config.targetWidth == 0 && config.targetHeight == 0) {
                    return;
                }
                adaptiveResolutionEnabled = false;
                adaptiveResolutionTiers.clear();
                adaptiveResolutionTierIndex = 0;
                restartStreamOutput(0, 0, 1.0, "manual", "runtime_control_native");
                std::cout << "runtime_resolution_mode=native\n";
                break;
            case screenshare::RuntimeResolutionMode::Fixed:
                if (!restartCapture &&
                    !adaptiveResolutionEnabled &&
                    config.targetWidth == resolution.width &&
                    config.targetHeight == resolution.height) {
                    return;
                }
                adaptiveResolutionEnabled = false;
                adaptiveResolutionTiers.clear();
                adaptiveResolutionTierIndex = 0;
                restartStreamOutput(resolution.width, resolution.height, 1.0, "manual", "runtime_control_fixed");
                std::cout
                    << "runtime_resolution_mode=fixed"
                    << " runtime_resolution=" << resolution.width << "x" << resolution.height
                    << "\n";
                break;
            }
        }
        if (restartCapture && !request->resolution) {
            restartCapturePipeline("runtime_control");
        }
    };

    auto keepRunning = [&]() {
        if (runtimeControl.StopRequested()) {
            return false;
        }
        return options.seconds == 0 || Clock::now() - startedAt < std::chrono::seconds(options.seconds);
    };

    while (keepRunning()) {
        std::this_thread::sleep_until(nextFrameAt);
        nextFrameAt += targetFrameTime;
        drainLiveSignalingSendTargets();
        applyRuntimeStreamSettingsControl();

        std::optional<screenshare::CapturedFrame> frame;
        if (!runtimeVideoPaused) {
            const auto captureStartedAt = Clock::now();
            frame = capturer.TryCaptureFrame(std::chrono::milliseconds(0));
            const double captureMs = std::chrono::duration<double, std::milli>(Clock::now() - captureStartedAt).count();
            intervalCaptureMs += captureMs;
            ++intervalCaptureCalls;
            totalCaptureMs += captureMs;
            ++totalCaptureCalls;
        }
        if (frame) {
            ++totalDesktopUpdates;
            ++intervalDesktopUpdates;
            const bool outputSizeChanged =
                hasFrame &&
                (lastFrame.width != frame->width || lastFrame.height != frame->height);
            if (outputSizeChanged) {
                if (streamEncoder) {
                    resetStreamEncoder();
                }
                if (adaptiveResolutionEnabled && config.targetWidth == 0 && config.targetHeight == 0) {
                    adaptiveResolutionTiers.clear();
                    adaptiveResolutionTierIndex = 0;
                    resolutionAdaptationStatus = "waiting";
                }
                if (autoBitrateEnabled) {
                    streamTargetBitrate = SelectBitrate(options, frame->width, frame->height);
                    streamBitrate = 0;
                    bitrateAdvisor.Configure(
                        streamTargetBitrate,
                        SelectAdaptiveMinBitrate(options, streamTargetBitrate, adaptiveResolutionTiers),
                        static_cast<uint32_t>(options.adaptReduceCooldownSeconds));
                }
                std::cout
                    << "capture_output_resized"
                    << " old=" << lastFrame.width << "x" << lastFrame.height
                    << " new=" << frame->width << "x" << frame->height
                    << " stream_encoder=restarting"
                    << "\n";
            }
            lastSourceWidth = frame->sourceWidth;
            lastSourceHeight = frame->sourceHeight;
            lastOutputWidth = frame->width;
            lastOutputHeight = frame->height;
            lastSourceFormat = frame->sourceFormat;
            lastOutputFormat = frame->format;
            lastDisplayColorSpace = frame->displayColorSpace;
            lastDisplayHdrActive = frame->displayHdrActive;
            lastColorConversionMode = frame->colorConversionMode;
            lastNv12GeneratedOnGpu = frame->nv12GeneratedOnGpu;
            lastNv12TextureAvailable = frame->nv12Texture != nullptr;
            lastFrame = std::move(*frame);
            hasFrame = true;
            if (adaptiveResolutionEnabled && adaptiveResolutionTiers.empty()) {
                adaptiveResolutionTiers = BuildResolutionTiers(
                    lastFrame.width,
                    lastFrame.height,
                    options.adaptResolutionMinScale);
                adaptiveResolutionTierIndex = 0;
                std::cout << "Adaptive resolution tiers:";
                for (const auto& tier : adaptiveResolutionTiers) {
                    std::cout << " " << tier.width << "x" << tier.height << "@" << tier.scale;
                }
                std::cout << "\n";
            }
        }

        if (!runtimeVideoPaused && hasFrame) {
            ++totalOutputFrames;
            ++intervalOutputFrames;

            if (!frame) {
                ++totalRepeatedFrames;
                ++intervalRepeatedFrames;
            }

            if (!options.recordPath.empty()) {
                if (!fileEncoder) {
                    screenshare::H264EncoderConfig encoderConfig;
                    encoderConfig.outputPath = screenshare::Widen(options.recordPath);
                    encoderConfig.width = lastFrame.width;
                    encoderConfig.height = lastFrame.height;
                    encoderConfig.fps = runtimeFps;
                    encoderConfig.bitrate = SelectBitrate(options, lastFrame.width, lastFrame.height);

                    const std::filesystem::path recordPath(options.recordPath);
                    if (recordPath.has_parent_path()) {
                        std::filesystem::create_directories(recordPath.parent_path());
                    }

                    fileEncoder = std::make_unique<screenshare::H264FileEncoder>();
                    fileEncoder->Start(encoderConfig);
                    std::cout
                        << "File encoder output=" << encoderConfig.width << "x" << encoderConfig.height
                        << " bitrate_mbps=" << Mbps(encoderConfig.bitrate)
                        << "\n";
                }

                fileEncoder->WriteFrame(lastFrame);
                ++fileEncodedFrames;
            }

            if (options.streamEncode) {
                if (!streamEncoder) {
                    if (streamTargetBitrate == 0) {
                        streamTargetBitrate = SelectBitrate(options, lastFrame.width, lastFrame.height);
                    }
                    const uint32_t encoderStartBitrate = streamBitrate == 0 ? streamTargetBitrate : streamBitrate;
                    if (options.adaptMinBitrateProvided && options.adaptMinBitrate > streamTargetBitrate) {
                        throw std::runtime_error("--adapt-min-bitrate-mbps cannot be greater than the selected stream bitrate");
                    }

                    auto startStreamEncoder = [&](screenshare::H264StreamEncoderBackend backend) {
                        if (backend == screenshare::H264StreamEncoderBackend::Hardware &&
                            (!lastFrame.d3dDevice || !lastFrame.nv12Texture)) {
                            throw std::runtime_error("hardware stream encoder requires a captured D3D11 NV12 texture");
                        }

                        screenshare::H264StreamEncoderConfig encoderConfig;
                        encoderConfig.width = lastFrame.width;
                        encoderConfig.height = lastFrame.height;
                        encoderConfig.fps = runtimeFps;
                        encoderConfig.bitrate = encoderStartBitrate;
                        encoderConfig.keyframeIntervalFrames = streamKeyframeIntervalFrames;
                        encoderConfig.startFrameIndex = static_cast<int64_t>(streamEncodedFrames);
                        streamBitrate = encoderConfig.bitrate;
                        if (!bitrateAdvisor.configured()) {
                            const uint32_t adaptiveMinBitrate = SelectAdaptiveMinBitrate(
                                options,
                                streamTargetBitrate,
                                adaptiveResolutionTiers);
                            bitrateAdvisor.Configure(
                                streamTargetBitrate,
                                adaptiveMinBitrate,
                                static_cast<uint32_t>(options.adaptReduceCooldownSeconds));
                        }
                        encoderConfig.backend = backend;
                        if (encoderConfig.backend == screenshare::H264StreamEncoderBackend::Hardware) {
                            encoderConfig.d3dDevice = lastFrame.d3dDevice;
                        }

                        auto encoder = std::make_unique<screenshare::H264StreamEncoder>();
                        encoder->Start(encoderConfig);
                        streamEncoderWidth = encoderConfig.width;
                        streamEncoderHeight = encoderConfig.height;

                        std::cout
                            << "Stream encoder output=" << encoderConfig.width << "x" << encoderConfig.height
                            << " bitrate_mbps=" << Mbps(encoderConfig.bitrate)
                            << " keyframe_interval_frames=" << encoderConfig.keyframeIntervalFrames
                            << " preference=" << StreamEncoderPreferenceName(streamEncoderPreference)
                            << " backend=" << screenshare::H264StreamEncoderBackendName(encoder->backend())
                            << " input=" << screenshare::H264StreamEncoderInputModeName(encoder->lastInputMode())
                            << " encoder=\"" << encoder->encoderName() << "\""
                            << "\n";

                        return encoder;
                    };

                    if (streamEncoderPreference == StreamEncoderPreference::Software) {
                        streamEncoder = startStreamEncoder(screenshare::H264StreamEncoderBackend::Software);
                    } else {
                        try {
                            streamEncoder = startStreamEncoder(screenshare::H264StreamEncoderBackend::Hardware);
                        } catch (const std::exception& error) {
                            if (streamEncoderPreference == StreamEncoderPreference::Hardware) {
                                throw;
                            }

                            std::cerr
                                << "Hardware stream encoder unavailable; falling back to software: "
                                << error.what()
                                << "\n";
                            streamEncoderPreference = StreamEncoderPreference::Software;

                            if (!HasSoftwareStreamInput(lastFrame)) {
                                ConfigureCapturePayloads(config, options, streamEncoderPreference);
                                capturer.Stop();
                                capturer.Start(config);
                                hasFrame = false;
                                resetStreamEncoder();
                                std::cerr << "Restarted capture with CPU-visible NV12 for software stream encoding.\n";
                                continue;
                            }

                            streamEncoder = startStreamEncoder(screenshare::H264StreamEncoderBackend::Software);
                        }
                    }

                }

                ensureUdpSenderForTargets();

                if (streamEncoder &&
                    (streamEncoderWidth != lastFrame.width || streamEncoderHeight != lastFrame.height)) {
                    resetStreamEncoder();
                    continue;
                }

                const auto streamEncodeStartedAt = Clock::now();
                const auto packets = streamEncoder->EncodeFrame(lastFrame);
                const double streamEncodeMs =
                    std::chrono::duration<double, std::milli>(Clock::now() - streamEncodeStartedAt).count();
                intervalStreamEncodeMs += streamEncodeMs;
                ++intervalStreamEncodeCalls;
                totalStreamEncodeMs += streamEncodeMs;
                ++totalStreamEncodeCalls;
                ++streamEncodedFrames;
                sendStreamPackets(packets);
            }
        }

        if (audioCaptureWorker) {
            audioCaptureWorker->ThrowIfFailed();
            const auto audioStats = audioCaptureWorker->stats();
            if (audioStats.payloadBitrate > 0 && audioStats.payloadBitrate != audioPacingBitrate) {
                audioPacingBitrate = audioStats.payloadBitrate;
                updateUdpPacingBitrate();
                if (!audioPacingApplied) {
                    audioPacingApplied = true;
                    std::cout
                        << "Audio capture format=\""
                        << audioStats.deviceName
                        << "\" "
                        << audioStats.sampleRate << "x" << audioStats.channels
                        << "x" << audioStats.bitsPerSample
                        << "/" << screenshare::udp_protocol::AudioSampleFormatName(audioStats.sampleFormat)
                        << " codec=" << screenshare::udp_protocol::AudioCodecName(audioStats.codec)
                        << " payload_bitrate_mbps=" << Mbps(static_cast<uint32_t>(
                            std::min<uint64_t>(audioStats.payloadBitrate, std::numeric_limits<uint32_t>::max())))
                        << " udp_pacing_mbps=" << Mbps(combinedUdpPacingBitrate())
                        << "\n";
                }
            }
        }

        const auto now = Clock::now();
        if (now - lastReportAt >= std::chrono::seconds(1)) {
            const double elapsed = std::chrono::duration<double>(now - lastReportAt).count();
            const double outputFps = static_cast<double>(intervalOutputFrames) / elapsed;
            const double desktopUpdateFps = static_cast<double>(intervalDesktopUpdates) / elapsed;
            const double captureAvgMs =
                intervalCaptureCalls == 0 ? 0.0 : intervalCaptureMs / static_cast<double>(intervalCaptureCalls);
            const double streamEncodeAvgMs =
                intervalStreamEncodeCalls == 0 ? 0.0 : intervalStreamEncodeMs / static_cast<double>(intervalStreamEncodeCalls);
            if (udpSender) {
                RecordLatestReceiverFeedback(
                    reportContext,
                    DrainUdpFeedback(*udpSender, std::chrono::milliseconds(0)));
            }
            const screenshare::UdpSenderStats udpStatsNow =
                udpSender ? udpSender->stats() : screenshare::UdpSenderStats{};
            const AudioCaptureWorkerStats audioCaptureStatsNow =
                audioCaptureWorker ? audioCaptureWorker->stats() : AudioCaptureWorkerStats{};
            if (udpSender) {
                bitrateAdvisor.Update(udpStatsNow);
                applyAdaptiveBitrate();
                applyAdaptiveResolution(udpStatsNow);
            }
            std::cout
                << "source=" << lastSourceWidth << "x" << lastSourceHeight
                << " session=" << options.sessionId
                << " session_fingerprint=" << FormatSessionFingerprint(options.sessionFingerprint)
                << " source_format=" << screenshare::DxgiFormatName(lastSourceFormat)
                << " display_color_space=" << screenshare::DxgiColorSpaceName(lastDisplayColorSpace)
                << " display_hdr=" << (lastDisplayHdrActive ? "yes" : "no")
                << " color_conversion=" << screenshare::CaptureColorConversionName(lastColorConversionMode)
                << " output=" << lastOutputWidth << "x" << lastOutputHeight
                << " output_format=" << screenshare::DxgiFormatName(lastOutputFormat)
                << " resolution_scale=" << (adaptiveResolutionTiers.empty() ? 1.0 : adaptiveResolutionTiers[adaptiveResolutionTierIndex].scale)
                << " resolution_tier=" << (adaptiveResolutionTiers.empty() ? 0 : adaptiveResolutionTierIndex)
                << " resolution_tiers=" << adaptiveResolutionTiers.size()
                << " resolution_adaptation=" << resolutionAdaptationStatus
                << " resolution_adaptations=" << resolutionAdaptations
                << " resolution_adaptation_failures=" << resolutionAdaptationFailures
                << " resolution_cooldown=" << resolutionCooldownRemaining
                << " resolution_stable_feedback=" << resolutionStableFeedbackReports
                << " resolution_stable_required=" << ResolutionStableReportsBeforeUpscale
                << " resolution_reduce_pressure=" << resolutionReductionPressureReports
                << " resolution_reduce_required=" << ResolutionPressureReportsBeforeReduce
                << " nv12=" << (lastNv12TextureAvailable ? "gpu_texture" : (lastNv12GeneratedOnGpu ? "gpu_readback" : "cpu_or_none"))
                << " stream_input=" << (streamEncoder ? screenshare::H264StreamEncoderInputModeName(streamEncoder->lastInputMode()) : "none")
                << " video_paused=" << (runtimeVideoPaused ? "yes" : "no")
                << " stream_bitrate_mbps=" << Mbps(streamBitrate)
                << " stream_queue=" << (streamEncoder ? streamEncoder->queuedInputCount() : 0)
                << " stream_dropped=" << (streamEncoder ? streamEncoder->droppedInputFrames() : 0)
                << " output_fps=" << outputFps
                << " desktop_update_fps=" << desktopUpdateFps
                << " capture_avg_ms=" << captureAvgMs
                << " stream_encode_avg_ms=" << streamEncodeAvgMs
                << " repeated_frames=" << intervalRepeatedFrames
                << " total_output_frames=" << totalOutputFrames
                << " total_desktop_updates=" << totalDesktopUpdates
                << " file_encoded_frames=" << fileEncodedFrames
                << " stream_encoded_frames=" << streamEncodedFrames
                << " stream_packets=" << streamPackets
                << " stream_bytes=" << streamBytes
                << " udp_targets=" << (udpSender ? udpSender->targetCount() : 0)
                << " udp_active_targets=" << (udpSender ? udpSender->activeTargetCount() : 0)
                << " udp_failed_targets=" << (udpSender ? udpSender->failedTargetCount() : 0)
                << " udp_datagrams=" << udpStatsNow.datagramsSent
                << " udp_queued=" << udpStatsNow.datagramsQueued
                << " udp_pending=" << udpStatsNow.pendingDatagrams
                << " udp_peak_pending=" << udpStatsNow.peakPendingDatagrams
                << " udp_queue_ms=" << udpStatsNow.pendingQueueDelayMs
                << " udp_peak_queue_ms=" << udpStatsNow.peakQueueDelayMs
                << " udp_dropped_frames=" << udpStatsNow.framesDropped
                << " udp_wire_bytes=" << udpStatsNow.wireBytesSent
                << " audio_capture="
                << (runtimeAudioCaptureEnabled ?
                    (audioCaptureStatsNow.unavailable ?
                        "unavailable" :
                        (audioCaptureStatsNow.started ? "running" : "starting")) :
                    "disabled")
                << " audio_capture_source=" << screenshare::AudioCaptureSourceName(audioCaptureStatsNow.source)
                << " audio_capture_unavailable=" << (audioCaptureStatsNow.unavailable ? "yes" : "no")
                << " audio_capture_packets=" << audioCaptureStatsNow.packets
                << " audio_capture_frames=" << audioCaptureStatsNow.frames
                << " audio_capture_bytes=" << audioCaptureStatsNow.bytes
                << " audio_capture_empty_polls=" << audioCaptureStatsNow.emptyPolls
                << " audio_capture_discontinuities=" << audioCaptureStatsNow.discontinuities
                << " audio_capture_timestamp_errors=" << audioCaptureStatsNow.timestampErrors
                << " audio_capture_qpc=" << audioCaptureStatsNow.latestQpcPosition
                << " audio_codec=" << screenshare::udp_protocol::AudioCodecName(audioCaptureStatsNow.codec)
                << " audio_payload_bitrate_mbps=" << Mbps(static_cast<uint32_t>(
                    std::min<uint64_t>(audioCaptureStatsNow.payloadBitrate, std::numeric_limits<uint32_t>::max())))
                << " audio_udp_packets=" << udpStatsNow.audioPacketsSent
                << " audio_udp_frames=" << udpStatsNow.audioFramesSent
                << " audio_udp_datagrams=" << udpStatsNow.audioDatagramsQueued
                << " audio_udp_dropped_packets=" << udpStatsNow.audioPacketsDropped
                << " udp_pacing_bitrate_mbps=" << Mbps(combinedUdpPacingBitrate())
                << " udp_feedback_packets=" << udpStatsNow.feedbackPacketsReceived
                << " udp_feedback_invalid=" << udpStatsNow.invalidFeedbackPackets
                << " udp_feedback_access_rejected=" << udpStatsNow.feedbackAccessRejected
                << " udp_feedback_crypto_rejected=" << udpStatsNow.feedbackCryptoRejected
                << " udp_nat_probe_packets=" << udpStatsNow.natProbePacketsReceived
                << " udp_nat_retargets=" << udpStatsNow.natProbeRetargets
                << " udp_nat_retarget_rejected=" << udpStatsNow.natProbeRetargetRejected
                << " udp_nat_probe_targets=" << udpStatsNow.natProbeTargetCount
                << " udp_nat_retarget_active=" << (udpStatsNow.natProbeRetargetActive ? "yes" : "no")
                << " udp_nat_retarget_endpoint="
                << (udpStatsNow.natProbeRetargetEndpoint.empty() ? "none" : udpStatsNow.natProbeRetargetEndpoint)
                << " nat_status=" << SenderNatStatus(options, udpStatsNow)
                << " nat_hint=" << SenderNatHint(options, udpStatsNow)
                << " udp_encryption=" << (udpStatsNow.encryptionEnabled ? "enabled" : "disabled")
                << " udp_feedback_health="
                << (udpStatsNow.hasFeedback ?
                    screenshare::udp_protocol::FeedbackHealthStateName(udpStatsNow.latestFeedback.healthState) :
                    "none")
                << " udp_feedback_completed_frames=" << (udpStatsNow.hasFeedback ? udpStatsNow.latestFeedback.completedFrames : 0)
                << " udp_feedback_resyncs=" << (udpStatsNow.hasFeedback ? udpStatsNow.latestFeedback.decodeResyncs : 0)
                << " udp_feedback_skipped_packets=" << (udpStatsNow.hasFeedback ? udpStatsNow.latestFeedback.decodeSkippedPackets : 0)
                << " udp_feedback_session=" << FeedbackSessionText(udpStatsNow)
                << " udp_feedback_access=" << FeedbackAccessText(udpStatsNow)
                << " bitrate_advice_mbps=" << Mbps(bitrateAdvisor.recommendedBitrate())
                << " bitrate_advice_min_mbps=" << Mbps(bitrateAdvisor.minBitrate())
                << " bitrate_advice_action=" << bitrateAdvisor.action()
                << " bitrate_advice_reason=" << bitrateAdvisor.reason()
                << " bitrate_advice_cooldown=" << bitrateAdvisor.reduceCooldownRemaining()
                << " bitrate_advice_suppressed=" << bitrateAdvisor.suppressedReductions()
                << " bitrate_adaptation=" << bitrateAdaptationStatus
                << " bitrate_adaptations=" << bitrateAdaptations
                << " bitrate_adaptation_failures=" << bitrateAdaptationFailures
                << "\n" << std::flush;
            if (udpSender) {
                PrintViewerSnapshots(*udpSender);
                std::cout << std::flush;
            }
            intervalOutputFrames = 0;
            intervalDesktopUpdates = 0;
            intervalRepeatedFrames = 0;
            intervalCaptureMs = 0.0;
            intervalCaptureCalls = 0;
            intervalStreamEncodeMs = 0.0;
            intervalStreamEncodeCalls = 0;
            lastReportAt = now;
        }
    }

    const auto captureFinishedAt = Clock::now();
    const bool stopWasRequested = runtimeControl.StopRequested();

    if (stopWasRequested && liveSignalingRuntime) {
        liveSignalingRuntime->Stop();
    }

    if (streamEncoder && !stopWasRequested) {
        const auto drainedPackets = streamEncoder->Drain();
        sendStreamPackets(drainedPackets);
    }

    if (audioCaptureWorker) {
        audioCaptureWorker->Stop();
        audioCaptureWorker->ThrowIfFailed();
    }

    if (udpSender && !stopWasRequested) {
        udpSender->Flush();
        RecordLatestReceiverFeedback(
            reportContext,
            DrainUdpFeedback(*udpSender, std::chrono::milliseconds(100)));
    }

    const size_t streamQueuedInputs = streamEncoder ? streamEncoder->queuedInputCount() : 0;
    const uint64_t streamDroppedInputFrames = streamEncoder ? streamEncoder->droppedInputFrames() : 0;
    const AudioCaptureWorkerStats finalAudioCaptureStats =
        audioCaptureWorker ? audioCaptureWorker->stats() : AudioCaptureWorkerStats{};
    const screenshare::UdpSenderStats udpStats = udpSender ? udpSender->stats() : screenshare::UdpSenderStats{};
    if (udpSender) {
        bitrateAdvisor.Update(udpStats);
    }
    if (!options.capturedBmpPath.empty() && hasFrame) {
        WriteCapturedFrameBmp(options.capturedBmpPath, lastFrame);
        capturedBmpWritten = true;
    }
    audioCaptureWorker.reset();
    udpSender.reset();
    streamEncoder.reset();
    fileEncoder.reset();
    capturer.Stop();

    const double totalElapsed = std::chrono::duration<double>(captureFinishedAt - startedAt).count();
    std::cout
        << "Done. Average output FPS: " << (static_cast<double>(totalOutputFrames) / totalElapsed)
        << ", session: " << options.sessionId
        << ", session fingerprint: " << FormatSessionFingerprint(options.sessionFingerprint)
        << ", average desktop update FPS: " << (static_cast<double>(totalDesktopUpdates) / totalElapsed)
        << ", average capture ms: " << (totalCaptureCalls == 0 ? 0.0 : totalCaptureMs / static_cast<double>(totalCaptureCalls))
        << ", average stream encode ms: " << (totalStreamEncodeCalls == 0 ? 0.0 : totalStreamEncodeMs / static_cast<double>(totalStreamEncodeCalls))
        << ", repeated frames: " << totalRepeatedFrames
        << ", file encoded frames: " << fileEncodedFrames
        << ", stream encoded frames: " << streamEncodedFrames
        << ", stream bitrate Mbps: " << Mbps(streamBitrate)
        << ", resolution scale: " << (adaptiveResolutionTiers.empty() ? 1.0 : adaptiveResolutionTiers[adaptiveResolutionTierIndex].scale)
        << ", resolution adaptation: " << resolutionAdaptationStatus
        << ", resolution adaptations: " << resolutionAdaptations
        << ", resolution adaptation failures: " << resolutionAdaptationFailures
        << ", resolution stable feedback: " << resolutionStableFeedbackReports
        << ", resolution stable required: " << ResolutionStableReportsBeforeUpscale
        << ", resolution reduce pressure: " << resolutionReductionPressureReports
        << ", resolution reduce required: " << ResolutionPressureReportsBeforeReduce
        << ", stream packets: " << streamPackets
        << ", stream bytes: " << streamBytes
        << ", stream queued inputs: " << streamQueuedInputs
        << ", stream dropped inputs: " << streamDroppedInputFrames
        << ", UDP queued datagrams: " << udpStats.datagramsQueued
        << ", UDP targets: " << (udpSender ? udpSender->targetCount() : 0)
        << ", UDP active targets: " << (udpSender ? udpSender->activeTargetCount() : 0)
        << ", UDP failed targets: " << (udpSender ? udpSender->failedTargetCount() : 0)
        << ", UDP datagrams: " << udpStats.datagramsSent
        << ", UDP pending datagrams: " << udpStats.pendingDatagrams
        << ", UDP peak pending datagrams: " << udpStats.peakPendingDatagrams
        << ", UDP queue ms: " << udpStats.pendingQueueDelayMs
        << ", UDP peak queue ms: " << udpStats.peakQueueDelayMs
        << ", UDP dropped frames: " << udpStats.framesDropped
        << ", UDP dropped datagrams: " << udpStats.datagramsDropped
        << ", UDP wire bytes: " << udpStats.wireBytesSent
        << ", audio capture: " << (runtimeAudioCaptureEnabled ? (finalAudioCaptureStats.started ? "done" : "not-started") : "disabled")
        << ", audio capture packets: " << finalAudioCaptureStats.packets
        << ", audio capture frames: " << finalAudioCaptureStats.frames
        << ", audio capture bytes: " << finalAudioCaptureStats.bytes
        << ", audio capture empty polls: " << finalAudioCaptureStats.emptyPolls
        << ", audio capture discontinuities: " << finalAudioCaptureStats.discontinuities
        << ", audio capture timestamp errors: " << finalAudioCaptureStats.timestampErrors
        << ", audio capture qpc: " << finalAudioCaptureStats.latestQpcPosition
        << ", audio codec: " << screenshare::udp_protocol::AudioCodecName(finalAudioCaptureStats.codec)
        << ", audio payload bitrate Mbps: " << Mbps(static_cast<uint32_t>(
            std::min<uint64_t>(finalAudioCaptureStats.payloadBitrate, std::numeric_limits<uint32_t>::max())))
        << ", audio UDP packets: " << udpStats.audioPacketsSent
        << ", audio UDP frames: " << udpStats.audioFramesSent
        << ", audio UDP datagrams: " << udpStats.audioDatagramsQueued
        << ", audio UDP dropped packets: " << udpStats.audioPacketsDropped
        << ", UDP pacing bitrate Mbps: " << Mbps(combinedUdpPacingBitrate())
        << ", UDP feedback packets: " << udpStats.feedbackPacketsReceived
        << ", UDP invalid feedback packets: " << udpStats.invalidFeedbackPackets
        << ", UDP feedback access rejected: " << udpStats.feedbackAccessRejected
        << ", UDP feedback crypto rejected: " << udpStats.feedbackCryptoRejected
        << ", UDP NAT probe packets: " << udpStats.natProbePacketsReceived
        << ", UDP NAT retargets: " << udpStats.natProbeRetargets
        << ", UDP NAT retarget rejected: " << udpStats.natProbeRetargetRejected
        << ", UDP NAT probe targets: " << udpStats.natProbeTargetCount
        << ", UDP NAT retarget active: " << (udpStats.natProbeRetargetActive ? "yes" : "no")
        << ", UDP NAT retarget endpoint: "
        << (udpStats.natProbeRetargetEndpoint.empty() ? "none" : udpStats.natProbeRetargetEndpoint)
        << ", NAT status: " << SenderNatStatus(options, udpStats)
        << ", NAT hint: " << SenderNatHint(options, udpStats)
        << ", UDP encryption: " << (udpStats.encryptionEnabled ? "enabled" : "disabled")
        << ", UDP feedback health: "
        << (udpStats.hasFeedback ?
            screenshare::udp_protocol::FeedbackHealthStateName(udpStats.latestFeedback.healthState) :
            "none")
        << ", UDP feedback completed frames: " << (udpStats.hasFeedback ? udpStats.latestFeedback.completedFrames : 0)
        << ", UDP feedback resyncs: " << (udpStats.hasFeedback ? udpStats.latestFeedback.decodeResyncs : 0)
        << ", UDP feedback skipped packets: " << (udpStats.hasFeedback ? udpStats.latestFeedback.decodeSkippedPackets : 0)
        << ", UDP feedback session: " << FeedbackSessionText(udpStats)
        << ", UDP feedback access: " << FeedbackAccessText(udpStats)
        << ", bitrate advice Mbps: " << Mbps(bitrateAdvisor.recommendedBitrate())
        << ", bitrate advice min Mbps: " << Mbps(bitrateAdvisor.minBitrate())
        << ", bitrate advice action: " << bitrateAdvisor.action()
        << ", bitrate advice reason: " << bitrateAdvisor.reason()
        << ", bitrate advice cooldown: " << bitrateAdvisor.reduceCooldownRemaining()
        << ", bitrate advice suppressed reductions: " << bitrateAdvisor.suppressedReductions()
        << ", bitrate adaptation: " << bitrateAdaptationStatus
        << ", bitrate adaptations: " << bitrateAdaptations
        << ", bitrate adaptation failures: " << bitrateAdaptationFailures
        << ", captured BMP written: " << (capturedBmpWritten ? "yes" : "no")
        << "\n";
}

void PrepareLiveSignaling(Options& options)
{
    if (!options.signalingLive) {
        return;
    }

    const uint16_t localPort = options.shareRoom ? options.udpLocalPort : options.udpReceivePort;
    if (localPort == 0) {
        throw std::invalid_argument("Live signaling needs a non-zero local UDP port");
    }

    screenshare::StunQueryConfig stunConfig;
    stunConfig.server = options.signalingStunServer;
    stunConfig.timeout = std::chrono::milliseconds(options.signalingTimeoutMs);
    stunConfig.localPort = localPort;

    std::cout
        << "signaling_live_stun=starting"
        << " server=" << stunConfig.server.host << ":" << stunConfig.server.port
        << " local_port=" << localPort
        << " room=" << options.signalingRoomId
        << " peer_id=" << options.signalingPeerId
        << "\n";
    const auto stun = screenshare::QueryPublicUdpEndpoint(stunConfig);

    options.signalingLocalCandidate = screenshare::SignalingCandidate{
        "srflx",
        stun.publicAddress,
        stun.publicPort,
        "udp"};
    options.signalingLocalCandidateAvailable = true;
    if (IsUsableHostSignalingAddress(stun.localAddress) && stun.localPort != 0) {
        options.signalingHostCandidate = screenshare::SignalingCandidate{
            "host",
            stun.localAddress,
            stun.localPort,
            "udp"};
        options.signalingHostCandidateAvailable = true;
    }
    const screenshare::SignalingPeerState peer = BuildLiveSignalingPeerState(options);

    screenshare::SignalingClientConfig clientConfig;
    clientConfig.serverUrl = options.signalingServerUrl;
    clientConfig.timeout = std::chrono::milliseconds(options.signalingTimeoutMs);
    screenshare::SignalingClient client(std::move(clientConfig));

    std::map<std::string, screenshare::SignalingPeer> peersById;
    const auto response = client.Join(options.signalingRoomId, peer);
    ApplySignalingRoomAccessKey(options, response);
    MergeSignalingPeers(peersById, response);
    const int polls = 1;

    std::cout
        << "signaling_live_join=ok"
        << " room=" << options.signalingRoomId
        << " peer_id=" << options.signalingPeerId
        << " local_udp_endpoint=" << stun.localAddress << ":" << stun.localPort
        << " public_udp_endpoint=" << stun.publicAddress << ":" << stun.publicPort
        << " host_candidate=" << (options.signalingHostCandidateAvailable ? "yes" : "no")
        << " polls=" << polls
        << " peers=" << peersById.size()
        << "\n";

    std::vector<UdpSendTargetSpec> sendTargets;
    std::vector<screenshare::UdpNatProbeTarget> probeTargets;
    const uint64_t probeSession = NatProbeSessionFingerprint(options);
    for (const auto& [peerId, peerInfo] : peersById) {
        for (const auto& candidate : peerInfo.candidates) {
            const std::string endpoint = SignalingCandidateEndpoint(candidate);
            std::cout
                << "signaling_live_peer id=" << peerId
                << " endpoint=" << endpoint
                << " name=" << (peerInfo.metadata.name.empty() ? "none" : LogTokenEncode(peerInfo.metadata.name))
                << " type=" << candidate.type
                << " protocol=" << candidate.protocol
                << "\n";

            if (options.shareRoom) {
                sendTargets.push_back(SignalingSendTargetSpec(
                    candidate,
                    sendTargets.empty() ? localPort : static_cast<uint16_t>(0),
                    probeSession,
                    SignalingPeerGroup(peerId),
                    SignalingPeerDisplayName(peerInfo)));
            } else {
                probeTargets.push_back(screenshare::UdpNatProbeTarget{
                    candidate.ip,
                    candidate.port,
                    false});
            }
        }
    }

    if (options.shareRoom) {
        if (sendTargets.empty()) {
            std::cout
                << "signaling_live_warning=no_peers_yet"
                << " room=" << options.signalingRoomId
                << " hint=share_will_wait_for_signaling_peers\n";
            return;
        }
        options.udpSendTargetSpecs = std::move(sendTargets);
        options.udpSendTargets.clear();
        for (const auto& target : options.udpSendTargetSpecs) {
            options.udpSendTargets.push_back(target.target);
        }
        options.udpSendTarget = options.udpSendTargets.front();
        options.udpSendTargetFromPeerInvite = true;
        options.udpSendPeerInviteEndpoint = "signaling";
    } else {
        options.signalingNatProbeTargets = std::move(probeTargets);
        if (options.signalingNatProbeTargets.empty()) {
            std::cout
                << "signaling_live_warning=no_peers_yet"
                << " room=" << options.signalingRoomId
                << " hint=start_share_within_worker_peer_ttl_or_increase_setup_time\n";
        }
    }
}

void RunUdpReceiverStats(
    const Options& options,
    const ScreenShareRunContext& context,
    screenshare::ISessionRuntimeControl& runtimeControl)
{
    std::optional<screenshare::NatInvite> peerInvite;
    if (!options.peerInvite.empty()) {
        peerInvite = ParseValidatedPeerInvite(options);
    }

    screenshare::UdpReceiver receiver;
    screenshare::UdpReceiverConfig config;
    config.port = options.udpReceivePort;
    config.simulatedLossPercent = options.simulateLossPercent;
    config.simulatedJitter = std::chrono::milliseconds(options.simulateJitterMs);
    config.accessCodeFingerprint = options.accessCodeFingerprint;
    config.encryptionKey = options.accessCodeKey;
    config.natProbeInterval = std::chrono::milliseconds(options.natProbeIntervalMs);
    config.natProbeSessionFingerprint = NatProbeSessionFingerprint(options);
    bool peerInviteHasDistinctLocalEndpoint = false;
    if (peerInvite) {
        config.natProbeTargets.push_back(screenshare::UdpNatProbeTarget{
            peerInvite->publicEndpoint.host,
            peerInvite->publicEndpoint.port,
            false});
        peerInviteHasDistinctLocalEndpoint =
            !peerInvite->localEndpoint.host.empty() &&
            peerInvite->localEndpoint.port != 0 &&
            (peerInvite->localEndpoint.host != peerInvite->publicEndpoint.host ||
             peerInvite->localEndpoint.port != peerInvite->publicEndpoint.port);
        if (peerInviteHasDistinctLocalEndpoint) {
            config.natProbeTargets.push_back(screenshare::UdpNatProbeTarget{
                peerInvite->localEndpoint.host,
                peerInvite->localEndpoint.port,
                true});
        }
    }
    config.natProbeTargets.insert(
        config.natProbeTargets.end(),
        options.signalingNatProbeTargets.begin(),
        options.signalingNatProbeTargets.end());
    receiver.Open(config);
    std::unique_ptr<LiveSignalingRuntime> liveSignalingRuntime;
    if (options.signalingLive && !options.shareRoom) {
        liveSignalingRuntime = std::make_unique<LiveSignalingRuntime>();
        liveSignalingRuntime->Start(options);
    }
    std::map<std::string, std::string> signalingPeerByEndpoint;
    std::string activeMediaPeerId;
    bool hostLeftNotified = false;

    auto drainLiveSignalingProbeTargets = [&]() {
        if (!liveSignalingRuntime) {
            return 0;
        }

        const auto updateActiveMediaPeer = [&]() {
            if (!activeMediaPeerId.empty()) {
                return;
            }
            const std::string latestMediaEndpoint = receiver.stats().latestMediaEndpoint;
            if (latestMediaEndpoint.empty()) {
                return;
            }
            const auto peer = signalingPeerByEndpoint.find(latestMediaEndpoint);
            if (peer != signalingPeerByEndpoint.end()) {
                activeMediaPeerId = peer->second;
            }
        };

        updateActiveMediaPeer();
        for (const auto& peer : liveSignalingRuntime->DrainRemovedPeers()) {
            const std::string endpoint = SignalingCandidateEndpoint(peer.candidate);
            const std::string latestMediaEndpoint = receiver.stats().latestMediaEndpoint;
            const bool activeMediaEndpoint = !latestMediaEndpoint.empty() && endpoint == latestMediaEndpoint;
            const bool activeMediaPeer = !activeMediaPeerId.empty() && peer.peerId == activeMediaPeerId;
            if (activeMediaEndpoint && activeMediaPeerId.empty()) {
                activeMediaPeerId = peer.peerId;
            }
            signalingPeerByEndpoint.erase(endpoint);

            std::cout
                << "signaling_live_receiver_peer=removed"
                << " room=" << options.signalingRoomId
                << " peer_id=" << peer.peerId
                << " endpoint=" << endpoint
                << " active_media=" << (activeMediaEndpoint || activeMediaPeer ? "yes" : "no")
                << "\n";

            if (!hostLeftNotified && (activeMediaEndpoint || activeMediaPeer)) {
                hostLeftNotified = true;
                std::cout
                    << "watch_host_left=peer_left"
                    << " room=" << options.signalingRoomId
                    << " peer_id=" << peer.peerId
                    << " endpoint=" << endpoint
                    << "\n";
            }
        }

        int added = 0;
        for (const auto& peer : liveSignalingRuntime->DrainDiscoveredPeers()) {
            const std::string endpoint = SignalingCandidateEndpoint(peer.candidate);
            signalingPeerByEndpoint[endpoint] = peer.peerId;
            const screenshare::UdpNatProbeTarget target{
                peer.candidate.ip,
                peer.candidate.port,
                false};
            if (!receiver.AddNatProbeTarget(target)) {
                continue;
            }
            ++added;
            std::cout
                << "signaling_live_receiver_peer=added"
                << " room=" << options.signalingRoomId
                << " peer_id=" << peer.peerId
                << " endpoint=" << endpoint
                << "\n";
        }
        updateActiveMediaPeer();
        return added;
    };

    std::unique_ptr<screenshare::LanDiscoveryResponder> lanDiscoveryResponder;
    if (options.lanAdvertise) {
        screenshare::LanDiscoveryAdvertiseConfig advertiseConfig;
        advertiseConfig.discoveryPort = options.lanDiscoveryPort;
        advertiseConfig.sharePort = options.udpReceivePort;
        advertiseConfig.name = options.lanName;
        advertiseConfig.sessionId = options.sessionId;
        advertiseConfig.sessionFingerprint = options.sessionFingerprint;
        advertiseConfig.accessCodeFingerprint = options.accessCodeFingerprint;
        lanDiscoveryResponder = std::make_unique<screenshare::LanDiscoveryResponder>();
        lanDiscoveryResponder->Start(advertiseConfig);
    }

    std::ofstream h264Dump;
    const bool shouldDumpH264 = !options.h264DumpPath.empty();
    uint64_t h264DumpPackets = 0;
    uint64_t h264DumpBytes = 0;
    uint64_t nextH264DumpFrameId = 0;
    bool hasH264DumpStartFrame = false;
    std::map<uint64_t, screenshare::UdpCompletedFrame> h264DumpBacklog;
    if (shouldDumpH264) {
        const std::filesystem::path dumpPath(options.h264DumpPath);
        if (dumpPath.has_parent_path()) {
            std::filesystem::create_directories(dumpPath.parent_path());
        }

        h264Dump.open(dumpPath, std::ios::binary | std::ios::trunc);
        if (!h264Dump) {
            throw std::runtime_error("Failed to open H.264 dump file: " + options.h264DumpPath);
        }
    }

    WarnIfPlaintextUdpSession(options);

    std::cout
        << "Listening for UDP H.264 and audio packet fragments on port " << options.udpReceivePort
        << ", session " << options.sessionId
        << " (" << FormatSessionFingerprint(options.sessionFingerprint) << ")";
    if (options.accessCodeProvided) {
        std::cout << ", access code required, UDP encryption enabled";
    }
    if (!options.h264DumpPath.empty()) {
        std::cout << ", dumping H.264 to " << options.h264DumpPath;
    }
    if (options.decodeH264) {
        std::cout << ", decoding H.264";
    }
    if (!options.decodedBmpPath.empty()) {
        std::cout << ", dumping latest decoded BMP to " << options.decodedBmpPath;
    }
    if (options.previewWindow) {
        std::cout << ", previewing decoded frames";
    }
    if (options.audioPlayback) {
        std::cout
            << ", playing received audio"
            << ", audio latency " << options.audioPlaybackLatencyMs << " ms"
            << ", muted " << (options.audioPlaybackMuted ? "yes" : "no")
            << ", volume " << static_cast<int>(std::lround(options.audioPlaybackVolumePercent)) << "%";
    }
    if (options.avSync) {
        std::cout
            << ", A/V sync correction "
            << (options.avSyncExplicit ? "enabled" : "auto-enabled");
    }
    if (options.lanAdvertise) {
        std::cout
            << ", LAN discoverable as \"" << options.lanName << "\""
            << " on discovery port " << options.lanDiscoveryPort;
    }
    if (peerInvite) {
        std::cout
            << ", NAT punch probes to " << FormatNatEndpoint(peerInvite->publicEndpoint);
        if (peerInviteHasDistinctLocalEndpoint) {
            std::cout << " and " << FormatNatEndpoint(peerInvite->localEndpoint);
        }
        std::cout << " every " << options.natProbeIntervalMs << " ms";
    }
    if (options.simulateLossPercent > 0.0f || options.simulateJitterMs > 0) {
        std::cout
            << ", simulating loss " << options.simulateLossPercent << "%"
            << ", jitter up to " << options.simulateJitterMs << " ms";
    }
    std::cout << ".\n";

    auto writeH264DumpFrame = [&](const screenshare::UdpCompletedFrame& frame) {
        h264Dump.write(
            reinterpret_cast<const char*>(frame.bytes.data()),
            static_cast<std::streamsize>(frame.bytes.size()));
        if (!h264Dump) {
            throw std::runtime_error("Failed to write H.264 dump file: " + options.h264DumpPath);
        }

        ++h264DumpPackets;
        h264DumpBytes += frame.bytes.size();
    };

    auto maybeStartOrderedBacklog = [](std::map<uint64_t, screenshare::UdpCompletedFrame>& backlog,
                                       bool& hasStartFrame,
                                       uint64_t& nextFrameId,
                                       bool forceStart) {
        if (hasStartFrame) {
            return true;
        }
        if (backlog.empty()) {
            return false;
        }

        const uint64_t firstFrameId = backlog.begin()->first;
        if (firstFrameId == 0 || forceStart || backlog.size() >= OrderedReceiverStartThresholdFrames) {
            nextFrameId = firstFrameId;
            hasStartFrame = true;
            return true;
        }

        return false;
    };

    auto flushH264DumpBacklog = [&](bool forceStart = false) {
        if (!maybeStartOrderedBacklog(h264DumpBacklog, hasH264DumpStartFrame, nextH264DumpFrameId, forceStart)) {
            return;
        }

        while (true) {
            const auto next = h264DumpBacklog.find(nextH264DumpFrameId);
            if (next == h264DumpBacklog.end()) {
                break;
            }

            writeH264DumpFrame(next->second);
            h264DumpBacklog.erase(next);
            ++nextH264DumpFrameId;
        }
    };

    std::unique_ptr<screenshare::H264StreamDecoder> h264Decoder;
    std::unique_ptr<screenshare::ReceiverPreviewWindow> previewWindow;
    uint64_t h264DecodePackets = 0;
    uint64_t h264DecodedFrames = 0;
    uint64_t h264DecodedBytes = 0;
    uint64_t h264DecodeResyncs = 0;
    uint64_t h264DecodeDecoderRestarts = 0;
    uint64_t h264DecodeSkippedPackets = 0;
    double intervalH264DecodeMs = 0.0;
    uint64_t intervalH264DecodeCalls = 0;
    double totalH264DecodeMs = 0.0;
    uint64_t totalH264DecodeCalls = 0;
    int h264DecodedWidth = 0;
    int h264DecodedHeight = 0;
    uint64_t nextH264DecodeFrameId = 0;
    bool hasH264DecodeStartFrame = false;
    std::map<uint64_t, screenshare::UdpCompletedFrame> h264DecodeBacklog;
    std::optional<screenshare::DecodedFrameInfo> latestDecodedFrame;
    uint64_t previewPlayoutResets = 0;

    if (options.decodeH264) {
        h264Decoder = std::make_unique<screenshare::H264StreamDecoder>();
        h264Decoder->Start();
    }
    using Clock = std::chrono::steady_clock;
    PreviewPlayoutBuffer previewPlayout{
        std::chrono::milliseconds(options.previewLatencyMs),
        std::chrono::milliseconds(options.previewMaxLateMs),
    };
    AudioPlayoutBuffer audioPlayout{std::chrono::milliseconds(options.audioPlaybackLatencyMs)};
    AvSyncDiagnostics avSync;
    bool avSyncCorrectionApplied = !options.avSync;
    int avSyncPreviewBiasMs = 0;
    int avSyncAudioBiasMs = 0;
    std::optional<uint64_t> avSyncStartQpc100ns;
    uint64_t avSyncVideoStartDrops = 0;
    uint64_t avSyncAudioStartDrops = 0;
    uint64_t avSyncAudioCatchupDrops = 0;
    uint64_t avSyncAudioGateBypasses = 0;
    bool previewHeldForAudioCatchup = false;
    bool avSyncPlaybackStartAligned = false;
    uint64_t avSyncPlaybackStartQpc100ns = 0;
    double avSyncPlayoutAudioAheadMs = 0.0;
    std::string avSyncCorrectionStatus = options.avSync ? "waiting" : "disabled";
    bool avSyncVideoOnlyFallback = false;
    std::unique_ptr<screenshare::WasapiRenderer> audioRenderer;
    std::unique_ptr<screenshare::OpusAudioDecoder> opusAudioDecoder;
    std::optional<screenshare::AudioPlaybackFormat> activeAudioPlaybackFormat;
    std::string audioPlaybackStatus = options.audioPlayback ? "waiting" : "disabled";
    bool audioPlaybackMuted = options.audioPlaybackMuted;
    float audioPlaybackVolume = std::clamp(options.audioPlaybackVolumePercent / 100.0f, 0.0f, 2.0f);
    uint64_t audioPlaybackStarts = 0;
    uint64_t audioPlaybackFormatChanges = 0;

    std::string latestReceiverHealthTitle =
        "waiting | res 0x0 | fps 0.0 | lat 0/0ms | q 0/0/0 | resync 0 | skip 0 | drops 0/0 | reset 0 | shown 0 | av wait";

    auto audioPlaybackTitle = [&]() {
        return FormatAudioPlaybackTitle(options.audioPlayback, audioPlaybackStatus, audioPlaybackMuted, audioPlaybackVolume);
    };

    auto updatePreviewTitle = [&]() {
        if (previewWindow) {
            previewWindow->SetStatusText(latestReceiverHealthTitle + " | " + audioPlaybackTitle());
        }
    };

    auto applyAudioControls = [&]() {
        if (audioRenderer) {
            audioRenderer->SetMuted(audioPlaybackMuted);
            audioRenderer->SetVolume(audioPlaybackVolume);
        }
    };

    auto printAudioControls = [&]() {
        const int percent = static_cast<int>(std::lround(audioPlaybackVolume * 100.0f));
        std::cout
            << "Audio playback " << (audioPlaybackMuted ? "muted" : "unmuted")
            << ", volume=" << percent << "%\n";
    };

    auto toggleAudioMute = [&]() {
        if (!options.audioPlayback) {
            std::cout << "Audio playback is not enabled.\n";
            return;
        }
        audioPlaybackMuted = !audioPlaybackMuted;
        applyAudioControls();
        updatePreviewTitle();
        printAudioControls();
    };

    auto adjustAudioVolume = [&](int deltaPercent) {
        if (!options.audioPlayback) {
            std::cout << "Audio playback is not enabled.\n";
            return;
        }
        audioPlaybackVolume = std::clamp(
            audioPlaybackVolume + static_cast<float>(deltaPercent) / 100.0f,
            0.0f,
            2.0f);
        if (audioPlaybackVolume > 0.0f) {
            audioPlaybackMuted = false;
        }
        applyAudioControls();
        updatePreviewTitle();
        printAudioControls();
    };

    auto applyAudioPlaybackSettingsRequest = [&]() {
        auto request = runtimeControl.TakeAudioPlaybackSettingsRequest();
        if (!request || !options.audioPlayback) {
            return;
        }
        if (request->muted) {
            audioPlaybackMuted = *request->muted;
        }
        if (request->volumePercent) {
            audioPlaybackVolume = std::clamp(static_cast<float>(*request->volumePercent) / 100.0f, 0.0f, 2.0f);
            if (audioPlaybackVolume > 0.0f && request->muted == std::nullopt) {
                audioPlaybackMuted = false;
            }
        }
        applyAudioControls();
        updatePreviewTitle();
        printAudioControls();
    };

    auto ensurePreviewWindow = [&]() {
        if (!options.previewWindow || previewWindow) {
            return;
        }
        previewWindow = std::make_unique<screenshare::ReceiverPreviewWindow>();
        screenshare::ReceiverPreviewControlCallbacks callbacks;
        callbacks.toggleAudioMute = toggleAudioMute;
        callbacks.adjustAudioVolumePercent = adjustAudioVolume;
        previewWindow->SetControlCallbacks(std::move(callbacks));
        updatePreviewTitle();
        previewWindow->Show();
    };

    auto restartPreviewPlayoutClock = [&]() {
        if (previewWindow) {
            previewPlayout.ClearPendingAndRestartClock();
            ++previewPlayoutResets;
            avSyncPlaybackStartAligned = false;
            avSyncPlaybackStartQpc100ns = 0;
        }
    };

    auto ensureAudioRenderer = [&](const screenshare::UdpCompletedAudioPacket& packet) {
        const auto packetFormat = AudioPlaybackFormatFromPacket(packet);
        if (activeAudioPlaybackFormat && screenshare::SameAudioPlaybackFormat(*activeAudioPlaybackFormat, packetFormat)) {
            return;
        }

        if (activeAudioPlaybackFormat) {
            ++audioPlaybackFormatChanges;
        }
        audioPlayout.Clear();
        audioRenderer = std::make_unique<screenshare::WasapiRenderer>();

        screenshare::AudioPlaybackConfig playbackConfig;
        playbackConfig.format = packetFormat;
        playbackConfig.bufferDuration = std::chrono::milliseconds(
            std::clamp(options.audioPlaybackLatencyMs + 50, 50, 500));
        playbackConfig.muted = audioPlaybackMuted;
        playbackConfig.volume = audioPlaybackVolume;
        audioRenderer->Start(playbackConfig);
        activeAudioPlaybackFormat = packetFormat;
        audioPlaybackStatus = "buffering";
        ++audioPlaybackStarts;

        std::cout
            << "Audio playback started on \""
            << screenshare::Narrow(audioRenderer->deviceName())
            << "\", format=" << screenshare::AudioPlaybackFormatName(packetFormat)
            << ", buffer_frames=" << audioRenderer->bufferFrames()
            << ", muted=" << (audioPlaybackMuted ? "yes" : "no")
            << ", volume_percent=" << static_cast<int>(std::lround(audioPlaybackVolume * 100.0f))
            << "\n";
    };

    auto audioRendererPadding100ns = [&]() -> uint64_t {
        if (!audioRenderer || !activeAudioPlaybackFormat || activeAudioPlaybackFormat->sampleRate == 0) {
            return 0;
        }

        static_cast<void>(audioRenderer->AvailableFrames());
        const auto rendererStats = audioRenderer->stats();
        return static_cast<uint64_t>(rendererStats.lastPaddingFrames) * 10'000'000ULL /
               static_cast<uint64_t>(activeAudioPlaybackFormat->sampleRate);
    };

    auto audioRendererPaddingMs = [&]() -> double {
        return static_cast<double>(audioRendererPadding100ns()) / 10'000.0;
    };

    auto audioRendererBufferDuration = [&]() -> std::chrono::milliseconds {
        if (!audioRenderer || !activeAudioPlaybackFormat || activeAudioPlaybackFormat->sampleRate == 0) {
            return audioPlayout.targetLatency();
        }

        const uint64_t durationMs =
            static_cast<uint64_t>(audioRenderer->bufferFrames()) * 1000ULL /
            static_cast<uint64_t>(activeAudioPlaybackFormat->sampleRate);
        return std::chrono::milliseconds(static_cast<int64_t>(durationMs));
    };

    auto estimateAvSyncPlayoutAudioAheadMs = [&](const AvSyncSnapshot& snapshot) -> double {
        if (avSyncVideoOnlyFallback) {
            return 0.0;
        }

        if (!snapshot.ready) {
            return 0.0;
        }

        if (previewWindow &&
            previewPlayout.hasPresentedTimestamp() &&
            audioPlayout.hasRenderedQpc()) {
            const uint64_t padding100ns = audioRendererPadding100ns();
            const uint64_t renderedAudioEnd100ns = audioPlayout.lastRenderedEndQpc100ns();
            const uint64_t audibleAudio100ns =
                renderedAudioEnd100ns > padding100ns ? renderedAudioEnd100ns - padding100ns : 0;
            return static_cast<double>(
                static_cast<int64_t>(audibleAudio100ns) -
                static_cast<int64_t>(previewPlayout.lastPresentedTimestamp100ns())) / 10'000.0;
        }

        const double mediaAudioAheadMs =
            snapshot.senderClockReady ? snapshot.senderTimelineAudioAheadMs : snapshot.audioAheadMs;
        const double videoDelayMs =
            previewWindow ? static_cast<double>(previewPlayout.initialDelay().count()) : 0.0;
        double audioDelayMs = 0.0;
        if (options.audioPlayback) {
            audioDelayMs += static_cast<double>(audioPlayout.QueuedDuration().count());
            audioDelayMs += audioRenderer ? audioRendererPaddingMs() :
                static_cast<double>(audioPlayout.targetLatency().count());
        }

        return mediaAudioAheadMs + videoDelayMs - audioDelayMs;
    };

    auto avSyncAudioGatingEnabled = [&]() {
        return options.avSync && options.audioPlayback && !avSyncVideoOnlyFallback;
    };

    auto maxPreviewPresentationTimestamp100ns = [&]() -> std::optional<int64_t> {
        if (!avSyncAudioGatingEnabled() ||
            !previewWindow ||
            !audioRenderer ||
            !audioPlayout.hasRenderedQpc()) {
            return std::nullopt;
        }

        const uint64_t padding100ns = audioRendererPadding100ns();
        if (padding100ns == 0 && audioPlayout.queuedPacketCount() == 0) {
            return std::nullopt;
        }
        const uint64_t renderedAudioEnd100ns = audioPlayout.lastRenderedEndQpc100ns();
        if (renderedAudioEnd100ns <= padding100ns) {
            return std::nullopt;
        }

        constexpr uint64_t videoLeadSafetyLimit100ns = 250ULL * 10'000ULL;
        constexpr uint64_t maxTimestamp =
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        const uint64_t audibleAudio100ns = renderedAudioEnd100ns - padding100ns;
        if (audibleAudio100ns >= maxTimestamp) {
            return std::numeric_limits<int64_t>::max();
        }

        const uint64_t cappedSafetyLimit = std::min(
            videoLeadSafetyLimit100ns,
            maxTimestamp - audibleAudio100ns);
        return static_cast<int64_t>(audibleAudio100ns + cappedSafetyLimit);
    };

    auto dropQueuedAudioBehindPreview = [&]() {
        if (!avSyncAudioGatingEnabled() ||
            !previewWindow ||
            !audioRenderer ||
            !previewPlayout.hasPresentedTimestamp() ||
            !audioPlayout.hasRenderedQpc()) {
            return;
        }

        const int64_t presentedVideoTimestamp100ns = previewPlayout.lastPresentedTimestamp100ns();
        if (presentedVideoTimestamp100ns <= 0) {
            return;
        }

        const uint64_t padding100ns = audioRendererPadding100ns();
        const uint64_t renderedAudioEnd100ns = audioPlayout.lastRenderedEndQpc100ns();
        const uint64_t audibleAudio100ns =
            renderedAudioEnd100ns > padding100ns ? renderedAudioEnd100ns - padding100ns : 0;
        const int64_t audioLag100ns =
            presentedVideoTimestamp100ns - static_cast<int64_t>(audibleAudio100ns);
        constexpr int64_t catchupThreshold100ns = 150LL * 10'000LL;
        if (audioLag100ns <= catchupThreshold100ns) {
            return;
        }

        constexpr uint64_t targetTolerance100ns = 20ULL * 10'000ULL;
        uint64_t targetAudioQpc100ns = static_cast<uint64_t>(presentedVideoTimestamp100ns);
        if (targetAudioQpc100ns <= std::numeric_limits<uint64_t>::max() - padding100ns) {
            targetAudioQpc100ns += padding100ns;
        }
        if (targetAudioQpc100ns > targetTolerance100ns) {
            targetAudioQpc100ns -= targetTolerance100ns;
        }

        avSyncAudioCatchupDrops += audioPlayout.DropBeforeQpc(targetAudioQpc100ns);
    };

    auto presentPreviewReady = [&](bool flush) {
        if (!previewWindow) {
            return;
        }

        std::optional<int64_t> maxPresentationTimestamp;
        if (!flush) {
            maxPresentationTimestamp = maxPreviewPresentationTimestamp100ns();
        }
        const uint64_t syncWaitsBefore = previewPlayout.syncWaits();
        previewPlayout.PresentReady(
            *previewWindow,
            Clock::now(),
            flush,
            maxPresentationTimestamp);
        if (!flush && previewPlayout.syncWaits() != syncWaitsBefore) {
            previewHeldForAudioCatchup = true;
        }
        if (!flush) {
            dropQueuedAudioBehindPreview();
        }
    };

    auto mediaPlaybackAllowed = [&]() {
        return !options.avSync || avSyncCorrectionApplied;
    };

    auto avSyncPlayoutReady = [&](const AvSyncSnapshot& snapshot) {
        const bool audioRequired = avSyncAudioGatingEnabled();
        return (snapshot.ready || avSyncVideoOnlyFallback) &&
               (!audioRequired || (audioPlayout.started() && audioPlayout.hasRenderedQpc())) &&
               (!previewWindow || previewPlayout.hasPresentedTimestamp());
    };

    auto avSyncStateName = [&](const AvSyncSnapshot& snapshot) {
        if (snapshot.ready) {
            return "measuring";
        }
        return avSyncVideoOnlyFallback ? "video-only" : "waiting";
    };

    auto maybeApplyAvSyncCorrection = [&]() {
        if (!options.avSync || avSyncCorrectionApplied) {
            return;
        }

        const AvSyncSnapshot snapshot = avSync.snapshot();
        if (!snapshot.senderClockReady) {
            return;
        }

        const uint64_t startQpc = std::max(
            snapshot.firstVideoSenderQpc100ns,
            snapshot.firstAudioQpc100ns);
        avSyncStartQpc100ns = startQpc;
        avSyncVideoStartDrops += previewPlayout.DropBeforeTimestamp(static_cast<int64_t>(startQpc));
        avSyncAudioStartDrops += audioPlayout.DropBeforeQpc(startQpc);

        if (options.avSyncExplicit) {
            const int biasMs = std::clamp(
                static_cast<int>(std::llround(std::abs(snapshot.initialAudioSenderOffsetMs))),
                0,
                MaxAvSyncCorrectionBiasMs);
            if (snapshot.initialAudioSenderOffsetMs > 0.0) {
                audioPlayout.AddTargetLatencyBias(std::chrono::milliseconds(biasMs));
                avSyncAudioBiasMs = biasMs;
            } else if (snapshot.initialAudioSenderOffsetMs < 0.0) {
                previewPlayout.AddInitialDelayBias(std::chrono::milliseconds(biasMs));
                avSyncPreviewBiasMs = biasMs;
            }
        }

        avSyncCorrectionApplied = true;
        avSyncCorrectionStatus = "applied";
        std::cout
            << "A/V sync correction applied initial_audio_offset_ms=" << snapshot.initialAudioSenderOffsetMs
            << " start_qpc=" << *avSyncStartQpc100ns
            << " video_start_drops=" << avSyncVideoStartDrops
            << " audio_start_drops=" << avSyncAudioStartDrops
            << " preview_bias_ms=" << avSyncPreviewBiasMs
            << " audio_bias_ms=" << avSyncAudioBiasMs
            << " max_bias_ms=" << (options.avSyncExplicit ? MaxAvSyncCorrectionBiasMs : 0)
            << "\n";
    };

    auto maybeAllowVideoOnlyAvSync = [&]() {
        if (!options.avSync ||
            options.avSyncExplicit ||
            avSyncCorrectionApplied ||
            avSyncVideoOnlyFallback) {
            return;
        }

        const AvSyncSnapshot snapshot = avSync.snapshot();
        if (!snapshot.hasVideo ||
            snapshot.hasAudio ||
            snapshot.videoFrames < AvSyncVideoOnlyFallbackFrames) {
            return;
        }

        avSyncVideoOnlyFallback = true;
        avSyncCorrectionApplied = true;
        avSyncCorrectionStatus = "video_only_no_audio";
        std::cout
            << "A/V sync continuing video without audio after "
            << snapshot.videoFrames
            << " video frames and no audio packets.\n";
    };

    auto drainCompletedAudioPackets = [&]() {
        while (auto audioPacket = receiver.PopAudioPacket()) {
            avSync.ObserveAudioPacket(*audioPacket);
            if (options.audioPlayback) {
                if (audioPacket->codec == screenshare::udp_protocol::AudioCodec::Opus) {
                    if (!opusAudioDecoder) {
                        opusAudioDecoder = std::make_unique<screenshare::OpusAudioDecoder>();
                        opusAudioDecoder->Start();
                    }
                    *audioPacket = DecodeOpusAudioPacketForPlayback(*audioPacket, *opusAudioDecoder);
                } else if (audioPacket->codec != screenshare::udp_protocol::AudioCodec::Raw) {
                    continue;
                }
                if (avSyncStartQpc100ns &&
                    audioPacket->qpcPosition != 0 &&
                    audioPacket->qpcPosition < *avSyncStartQpc100ns) {
                    ++avSyncAudioStartDrops;
                    continue;
                }
                ensureAudioRenderer(*audioPacket);
                audioPlayout.Enqueue(std::move(*audioPacket));
            }
        }
        maybeApplyAvSyncCorrection();
        maybeAllowVideoOnlyAvSync();

        if (options.audioPlayback && audioRenderer && mediaPlaybackAllowed()) {
            if (avSyncAudioGatingEnabled() && previewWindow) {
                if (!previewPlayout.clockStarted()) {
                    audioPlaybackStatus = "video-wait";
                    return;
                }
                if (!avSyncPlaybackStartAligned) {
                    avSyncPlaybackStartQpc100ns = static_cast<uint64_t>(previewPlayout.firstTimestamp100ns());
                    avSyncAudioStartDrops += audioPlayout.DropBeforeQpc(avSyncPlaybackStartQpc100ns);
                    avSyncPlaybackStartAligned = true;
                }

                const auto now = Clock::now();
                const auto audioLead = audioRendererBufferDuration();
                if (now + audioLead < previewPlayout.firstPresentationAt()) {
                    audioPlaybackStatus = "video-wait";
                    return;
                }
            }
            dropQueuedAudioBehindPreview();
            const bool previewWaitingForAudioCatchup =
                avSyncAudioGatingEnabled() &&
                previewWindow &&
                previewHeldForAudioCatchup;
            std::optional<uint64_t> maxAudioQpcPosition;
            if (avSyncAudioGatingEnabled() &&
                previewWindow &&
                previewPlayout.hasPresentedTimestamp() &&
                !previewWaitingForAudioCatchup) {
                const uint64_t padding100ns = audioRendererPadding100ns();
                constexpr uint64_t audioLeadTolerance100ns = 20ULL * 10'000ULL;
                maxAudioQpcPosition =
                    static_cast<uint64_t>(previewPlayout.lastPresentedTimestamp100ns()) +
                    padding100ns +
                    audioLeadTolerance100ns;
            } else if (previewWaitingForAudioCatchup) {
                ++avSyncAudioGateBypasses;
            }
            const uint64_t syncWaitsBefore = audioPlayout.syncWaits();
            audioPlayout.RenderReady(*audioRenderer, maxAudioQpcPosition);
            if (previewWaitingForAudioCatchup) {
                previewHeldForAudioCatchup = false;
            }
            if (audioPlayout.syncWaits() != syncWaitsBefore) {
                audioPlaybackStatus = "sync-wait";
                return;
            }
            audioPlaybackStatus = audioPlayout.started() ? "playing" : "buffering";
        } else if (options.audioPlayback && audioRenderer && options.avSync && !avSyncCorrectionApplied) {
            audioPlaybackStatus = "sync-wait";
        }
    };

    auto emitDecodedVideoFrame = [&](const screenshare::DecodedFrameInfo& decodedFrame) {
        if (!options.emitVideoFrames || !context.videoFrameHandler) {
            return;
        }

        screenshare::SessionEvent::VideoFrame frame;
        frame.width = decodedFrame.width;
        frame.height = decodedFrame.height;
        frame.codedWidth = decodedFrame.codedWidth;
        frame.codedHeight = decodedFrame.codedHeight;
        frame.timestamp100ns = decodedFrame.timestamp100ns;
        frame.duration100ns = decodedFrame.duration100ns;
        frame.nv12.resize(decodedFrame.data.size());
        if (!frame.nv12.empty()) {
            std::memcpy(frame.nv12.data(), decodedFrame.data.data(), frame.nv12.size());
        }
        context.videoFrameHandler(std::move(frame));
    };

    auto countDecodedFrames = [&](std::vector<screenshare::DecodedFrameInfo> decodedFrames, bool presentReady = true) {
        h264DecodedFrames += decodedFrames.size();
        for (auto& decodedFrame : decodedFrames) {
            if (avSyncStartQpc100ns &&
                decodedFrame.timestamp100ns < static_cast<int64_t>(*avSyncStartQpc100ns)) {
                ++avSyncVideoStartDrops;
                continue;
            }
            const bool decodedOutputChanged =
                h264DecodedWidth > 0 &&
                h264DecodedHeight > 0 &&
                (decodedFrame.width != h264DecodedWidth || decodedFrame.height != h264DecodedHeight);
            if (decodedOutputChanged) {
                restartPreviewPlayoutClock();
            }

            h264DecodedBytes += decodedFrame.bytes;
            h264DecodedWidth = decodedFrame.width;
            h264DecodedHeight = decodedFrame.height;
            if (!options.decodedBmpPath.empty()) {
                latestDecodedFrame = decodedFrame;
            }
            emitDecodedVideoFrame(decodedFrame);
            if (options.previewWindow) {
                ensurePreviewWindow();
            }
            if (previewWindow) {
                previewPlayout.Enqueue(std::move(decodedFrame));
            }
        }

        if (previewWindow && presentReady && mediaPlaybackAllowed()) {
            presentPreviewReady(false);
        }
    };

    auto decodeH264Frame = [&](const screenshare::UdpCompletedFrame& frame) {
        screenshare::EncodedPacket packet;
        packet.timestamp100ns =
            options.avSync && frame.senderQpc100ns != 0 ?
                static_cast<int64_t>(frame.senderQpc100ns) :
                static_cast<int64_t>(frame.timestamp100ns);
        packet.bytes = frame.bytes;
        const auto decodeStart = Clock::now();
        auto decodedFrames = h264Decoder->DecodePacket(packet);
        const double decodeMs = std::chrono::duration<double, std::milli>(Clock::now() - decodeStart).count();
        if (std::isfinite(decodeMs) && decodeMs >= 0.0) {
            intervalH264DecodeMs += decodeMs;
            ++intervalH264DecodeCalls;
            totalH264DecodeMs += decodeMs;
            ++totalH264DecodeCalls;
        }
        countDecodedFrames(std::move(decodedFrames));
        ++h264DecodePackets;
    };

    auto restartH264DecoderForRecovery = [&]() {
        h264Decoder->Start();
        ++h264DecodeDecoderRestarts;
        restartPreviewPlayoutClock();
    };

    auto tryRecoverH264DecodeGap = [&](bool forceRecovery = false) {
        if (!hasH264DecodeStartFrame || h264DecodeBacklog.empty()) {
            return false;
        }
        if (h264DecodeBacklog.find(nextH264DecodeFrameId) != h264DecodeBacklog.end()) {
            return false;
        }
        if (!forceRecovery && h264DecodeBacklog.size() < OrderedReceiverRecoveryThresholdFrames) {
            return false;
        }

        auto recoveryFrame = h264DecodeBacklog.end();
        screenshare::H264AccessUnitInfo recoveryInfo;
        for (auto candidate = h264DecodeBacklog.lower_bound(nextH264DecodeFrameId);
             candidate != h264DecodeBacklog.end();
             ++candidate) {
            const auto info = screenshare::InspectH264AccessUnit(std::span<const std::byte>(
                candidate->second.bytes.data(),
                candidate->second.bytes.size()));
            if (info.hasIdrSlice) {
                recoveryFrame = candidate;
                recoveryInfo = info;
                break;
            }
        }

        if (recoveryFrame == h264DecodeBacklog.end()) {
            return false;
        }

        uint64_t skippedPackets = 0;
        if (recoveryFrame->first > nextH264DecodeFrameId) {
            skippedPackets += recoveryFrame->first - nextH264DecodeFrameId;
        }
        for (auto old = h264DecodeBacklog.begin(); old != recoveryFrame;) {
            old = h264DecodeBacklog.erase(old);
        }

        nextH264DecodeFrameId = recoveryFrame->first;
        h264DecodeSkippedPackets += skippedPackets;
        ++h264DecodeResyncs;

        if (recoveryInfo.hasSps && recoveryInfo.hasPps) {
            restartH264DecoderForRecovery();
        } else {
            restartPreviewPlayoutClock();
        }

        return true;
    };

    auto flushH264DecodeBacklog = [&](bool forceStart = false) {
        if (!maybeStartOrderedBacklog(h264DecodeBacklog, hasH264DecodeStartFrame, nextH264DecodeFrameId, forceStart)) {
            return;
        }

        while (true) {
            const auto next = h264DecodeBacklog.find(nextH264DecodeFrameId);
            if (next == h264DecodeBacklog.end()) {
                if (tryRecoverH264DecodeGap(forceStart)) {
                    continue;
                }
                break;
            }

            decodeH264Frame(next->second);
            h264DecodeBacklog.erase(next);
            ++nextH264DecodeFrameId;
        }
    };

    const auto startedAt = Clock::now();
    auto lastReportAt = startedAt;
    uint64_t lastDatagramsReceived = 0;
    uint64_t lastFramesCompleted = 0;
    uint64_t lastAudioPacketsCompleted = 0;
    uint64_t lastSimulatedDroppedDatagrams = 0;
    uint64_t lastInvalidDatagrams = 0;
    uint64_t lastIncompleteFramesDropped = 0;
    uint64_t lastH264DecodeResyncs = 0;
    uint64_t lastH264DecodeSkippedPackets = 0;
    uint64_t lastPreviewLateDrops = 0;
    uint64_t lastPreviewOverflowDrops = 0;
    uint64_t latestFrameId = 0;
    uint64_t latestFrameSenderQpc100ns = 0;
    uint64_t latestFrameBytes = 0;
    uint16_t latestFragmentCount = 0;
    uint64_t feedbackSequence = 0;
    uint64_t receiverStreamRestarts = 0;
    uint64_t receiverStreamStaleFrames = 0;
    uint64_t currentStreamStartSenderQpc100ns = 0;
    bool hasCompletedFrame = false;
    bool hasReceivedStreamTraffic = false;
    bool waitingForStreamLogged = false;
    bool previewBlankedForNoStream = false;
    auto lastStreamActivityAt = startedAt;
    bool hasNatProbeSetup = peerInvite.has_value() || !options.signalingNatProbeTargets.empty();

    auto detectReceiverStreamRestart = [&](const screenshare::UdpCompletedFrame& frame) {
        if (!hasCompletedFrame || frame.frameId >= latestFrameId) {
            return false;
        }

        if (frame.senderQpc100ns != 0 && latestFrameSenderQpc100ns != 0) {
            return frame.senderQpc100ns > latestFrameSenderQpc100ns;
        }

        constexpr uint64_t RestartFrameIdBackstepThreshold = 30;
        return latestFrameId - frame.frameId >= RestartFrameIdBackstepThreshold;
    };

    auto isStaleReceiverStreamFrame = [&](const screenshare::UdpCompletedFrame& frame) {
        if (currentStreamStartSenderQpc100ns == 0 || frame.senderQpc100ns == 0) {
            return false;
        }
        return frame.senderQpc100ns < currentStreamStartSenderQpc100ns;
    };

    auto resetReceiverStreamState = [&](const screenshare::UdpCompletedFrame& firstFrame) {
        ++receiverStreamRestarts;
        std::cout
            << "receiver_stream_restart"
            << " previous_frame=" << latestFrameId
            << " new_frame=" << firstFrame.frameId
            << " previous_sender_qpc=" << latestFrameSenderQpc100ns
            << " new_sender_qpc=" << firstFrame.senderQpc100ns
            << "\n";

        if (previewWindow) {
            restartPreviewPlayoutClock();
        }
        if (h264Decoder) {
            h264Decoder->Start();
            ++h264DecodeDecoderRestarts;
        }

        receiver.ResetMediaQueues();
        h264DecodeBacklog.clear();
        hasH264DecodeStartFrame = false;
        nextH264DecodeFrameId = 0;
        h264DumpBacklog.clear();
        hasH264DumpStartFrame = false;
        nextH264DumpFrameId = 0;
        latestDecodedFrame.reset();
        h264DecodedWidth = 0;
        h264DecodedHeight = 0;

        audioPlayout.Clear();
        audioRenderer.reset();
        activeAudioPlaybackFormat.reset();
        opusAudioDecoder.reset();
        audioPlaybackStatus = options.audioPlayback ? "waiting" : "disabled";

        avSync.Clear();
        avSyncCorrectionApplied = !options.avSync;
        avSyncStartQpc100ns.reset();
        avSyncPreviewBiasMs = 0;
        avSyncAudioBiasMs = 0;
        avSyncVideoStartDrops = 0;
        avSyncAudioStartDrops = 0;
        avSyncAudioCatchupDrops = 0;
        avSyncAudioGateBypasses = 0;
        previewHeldForAudioCatchup = false;
        avSyncPlaybackStartAligned = false;
        avSyncPlaybackStartQpc100ns = 0;
        avSyncPlayoutAudioAheadMs = 0.0;
        avSyncCorrectionStatus = options.avSync ? "waiting" : "disabled";
        avSyncVideoOnlyFallback = false;

        latestFrameId = 0;
        latestFrameSenderQpc100ns = 0;
        currentStreamStartSenderQpc100ns = firstFrame.senderQpc100ns;
        latestFrameBytes = 0;
        latestFragmentCount = 0;
        hasCompletedFrame = false;
        previewBlankedForNoStream = false;
        lastStreamActivityAt = Clock::now();
    };

    auto updateReportBaselines = [&](const screenshare::UdpReceiverStats& stats,
                                     uint64_t previewLateDrops,
                                     uint64_t previewOverflowDrops,
                                     Clock::time_point now) {
        lastDatagramsReceived = stats.datagramsReceived;
        lastFramesCompleted = stats.framesCompleted;
        lastAudioPacketsCompleted = stats.audioPacketsCompleted;
        lastSimulatedDroppedDatagrams = stats.simulatedDatagramsDropped;
        lastInvalidDatagrams = stats.invalidDatagrams;
        lastIncompleteFramesDropped = stats.incompleteFramesDropped;
        lastH264DecodeResyncs = h264DecodeResyncs;
        lastH264DecodeSkippedPackets = h264DecodeSkippedPackets;
        lastPreviewLateDrops = previewLateDrops;
        lastPreviewOverflowDrops = previewOverflowDrops;
        lastReportAt = now;
    };

    bool previewCloseRequested = false;
    auto shouldContinue = [&]() {
        if (previewWindow && !previewWindow->PumpMessages()) {
            if (!previewCloseRequested) {
                previewCloseRequested = true;
                std::cout << "preview_closed=stop\n" << std::flush;
            }
            return false;
        }
        if (runtimeControl.StopRequested()) {
            return false;
        }
        if ((options.previewWindow || options.emitVideoFrames) && options.seconds == 0) {
            return true;
        }
        return Clock::now() - startedAt < std::chrono::seconds(options.seconds);
    };

    while (shouldContinue()) {
        applyAudioPlaybackSettingsRequest();
        if (drainLiveSignalingProbeTargets() > 0) {
            hasNatProbeSetup = true;
        }
        if (previewWindow && mediaPlaybackAllowed()) {
            presentPreviewReady(false);
        }
        drainCompletedAudioPackets();

        auto receiveTimeout = previewWindow ?
            previewPlayout.ReceiveTimeout(Clock::now()) :
            std::chrono::milliseconds(100);
        if (options.audioPlayback) {
            receiveTimeout = std::min(receiveTimeout, std::chrono::milliseconds(5));
        }
        if (auto frame = receiver.ReceiveFrame(receiveTimeout)) {
            if (detectReceiverStreamRestart(*frame)) {
                resetReceiverStreamState(*frame);
            }
            if (isStaleReceiverStreamFrame(*frame)) {
                ++receiverStreamStaleFrames;
                continue;
            }
            if (currentStreamStartSenderQpc100ns == 0 && frame->senderQpc100ns != 0) {
                currentStreamStartSenderQpc100ns = frame->senderQpc100ns;
            }
            avSync.ObserveVideoFrame(*frame);
            maybeApplyAvSyncCorrection();
            latestFrameId = frame->frameId;
            latestFrameSenderQpc100ns = frame->senderQpc100ns;
            latestFrameBytes = frame->bytes.size();
            latestFragmentCount = frame->fragmentCount;
            hasCompletedFrame = true;
            previewBlankedForNoStream = false;
            lastStreamActivityAt = Clock::now();

            if (h264Decoder) {
                if (!hasH264DecodeStartFrame || frame->frameId >= nextH264DecodeFrameId) {
                    h264DecodeBacklog.emplace(frame->frameId, *frame);
                    flushH264DecodeBacklog();
                }
            }

            if (shouldDumpH264) {
                if (!hasH264DumpStartFrame || frame->frameId >= nextH264DumpFrameId) {
                    h264DumpBacklog.emplace(frame->frameId, std::move(*frame));
                    flushH264DumpBacklog();
                }
            }
        }
        drainCompletedAudioPackets();

        if (previewWindow && mediaPlaybackAllowed()) {
            presentPreviewReady(false);
        }

        const auto now = Clock::now();
        if (now - lastReportAt >= std::chrono::seconds(1)) {
            const double elapsed = std::chrono::duration<double>(now - lastReportAt).count();
            const auto& stats = receiver.stats();
            const double datagramsPerSecond = static_cast<double>(stats.datagramsReceived - lastDatagramsReceived) / elapsed;
            const double completedFps = static_cast<double>(stats.framesCompleted - lastFramesCompleted) / elapsed;
            const double h264DecodeAvgMs =
                intervalH264DecodeCalls == 0 ? 0.0 : intervalH264DecodeMs / static_cast<double>(intervalH264DecodeCalls);
            const uint64_t previewLateDrops = previewWindow ? previewPlayout.lateDrops() : 0;
            const uint64_t previewOverflowDrops = previewWindow ? previewPlayout.overflowDrops() : 0;
            const AvSyncSnapshot avSyncNow = avSync.snapshot();
            avSyncPlayoutAudioAheadMs = estimateAvSyncPlayoutAudioAheadMs(avSyncNow);
            const bool avSyncPlayoutReadyNow = avSyncPlayoutReady(avSyncNow);
            const screenshare::AudioPlaybackStats audioRendererStats =
                audioRenderer ? audioRenderer->stats() : screenshare::AudioPlaybackStats{};
            const ReceiverHealthSnapshot health{
                stats.framesCompleted,
                completedFps,
                receiver.pendingFrameCount(),
                h264DecodeBacklog.size(),
                previewWindow ? previewPlayout.queuedFrameCount() : 0,
                stats.simulatedDatagramsDropped,
                stats.invalidDatagrams,
                stats.incompleteFramesDropped,
                h264DecodeResyncs,
                h264DecodeSkippedPackets,
                previewWindow ? previewWindow->framesPresented() : 0,
                previewLateDrops,
                previewOverflowDrops,
                h264DecodedWidth,
                h264DecodedHeight,
                previewWindow ? previewPlayoutResets : 0,
                previewWindow ? static_cast<int>(previewPlayout.initialDelay().count()) : 0,
                previewWindow ? static_cast<int>(previewPlayout.maxLateFrameAge().count()) : 0,
                stats.simulatedDatagramsDropped - lastSimulatedDroppedDatagrams,
                stats.invalidDatagrams - lastInvalidDatagrams,
                stats.incompleteFramesDropped - lastIncompleteFramesDropped,
                h264DecodeResyncs - lastH264DecodeResyncs,
                h264DecodeSkippedPackets - lastH264DecodeSkippedPackets,
                previewLateDrops - lastPreviewLateDrops,
                previewOverflowDrops - lastPreviewOverflowDrops,
            };

            if (previewWindow) {
                latestReceiverHealthTitle = FormatReceiverHealthTitle(
                    health,
                    avSyncNow,
                    avSyncPlayoutAudioAheadMs,
                    avSyncPlayoutReadyNow);
                updatePreviewTitle();
            }
            receiver.SendFeedback(BuildReceiverFeedbackSnapshot(
                health,
                feedbackSequence++,
                options.sessionFingerprint,
                options.accessCodeFingerprint));

            const bool hasIncomingActivity =
                stats.datagramsReceived != lastDatagramsReceived ||
                stats.framesCompleted != lastFramesCompleted ||
                stats.audioPacketsCompleted != lastAudioPacketsCompleted;
            if (!hasIncomingActivity) {
                if (hasReceivedStreamTraffic &&
                    previewWindow &&
                    !previewBlankedForNoStream &&
                    now - lastStreamActivityAt >= std::chrono::seconds(2)) {
                    previewPlayout.ClearPendingAndRestartClock();
                    previewWindow->ClearFrame();
                    latestReceiverHealthTitle = "disconnected | waiting for stream | res 0x0 | fps 0.0 | q 0/0/0";
                    updatePreviewTitle();
                    previewBlankedForNoStream = true;
                }
                if (!waitingForStreamLogged) {
                    std::cout
                        << "waiting_for_stream"
                        << " session=" << options.sessionId
                        << " session_fingerprint=" << FormatSessionFingerprint(options.sessionFingerprint)
                        << " seen_stream=" << (hasReceivedStreamTraffic ? "yes" : "no")
                        << " nat_status=" << ReceiverNatStatus(hasNatProbeSetup, stats, hasReceivedStreamTraffic)
                        << " nat_hint=" << ReceiverNatHint(hasNatProbeSetup, stats, hasReceivedStreamTraffic)
                        << " udp_datagrams=" << stats.datagramsReceived
                        << " accepted_datagrams=" << stats.datagramsAccepted
                        << " nat_probe_public_sent=" << stats.natProbePublicPacketsSent
                        << " nat_probe_local_sent=" << stats.natProbeLocalPacketsSent
                        << " nat_probe_errors=" << stats.natProbeSendErrors
                        << " stream_restarts=" << receiverStreamRestarts
                        << " stream_stale_frames=" << receiverStreamStaleFrames
                        << " completed_frames=" << stats.framesCompleted
                        << " audio_packets=" << stats.audioPacketsCompleted
                        << "\n" << std::flush;
                    waitingForStreamLogged = true;
                }
                updateReportBaselines(stats, previewLateDrops, previewOverflowDrops, now);
                continue;
            }

            hasReceivedStreamTraffic = true;
            lastStreamActivityAt = now;
            previewBlankedForNoStream = false;
            waitingForStreamLogged = false;

            std::cout
                << "udp_datagrams=" << stats.datagramsReceived
                << " session=" << options.sessionId
                << " session_fingerprint=" << FormatSessionFingerprint(options.sessionFingerprint)
                << " receiver_health=" << ReceiverHealthState(health)
                << " nat_status=" << ReceiverNatStatus(hasNatProbeSetup, stats, hasReceivedStreamTraffic)
                << " nat_hint=" << ReceiverNatHint(hasNatProbeSetup, stats, hasReceivedStreamTraffic)
                << " udp_datagrams_per_second=" << datagramsPerSecond
                << " accepted_datagrams=" << stats.datagramsAccepted
                << " access_rejected_datagrams=" << stats.accessRejectedDatagrams
                << " crypto_rejected_datagrams=" << stats.cryptoRejectedDatagrams
                << " simulated_dropped=" << stats.simulatedDatagramsDropped
                << " simulated_delayed=" << stats.simulatedDatagramsDelayed
                << " simulated_delay_pending=" << receiver.delayedDatagramCount()
                << " feedback_sent=" << stats.feedbackPacketsSent
                << " feedback_errors=" << stats.feedbackSendErrors
                << " feedback_encrypted=" << stats.encryptedFeedbackPacketsSent
                << " nat_probe_public_sent=" << stats.natProbePublicPacketsSent
                << " nat_probe_local_sent=" << stats.natProbeLocalPacketsSent
                << " nat_probe_errors=" << stats.natProbeSendErrors
                << " stream_restarts=" << receiverStreamRestarts
                << " stream_stale_frames=" << receiverStreamStaleFrames
                << " audio_datagrams=" << stats.audioDatagramsAccepted
                << " audio_packets=" << stats.audioPacketsCompleted
                << " audio_queued_packets=" << stats.audioPacketsQueued
                << " audio_queue_dropped=" << stats.audioQueuedPacketsDropped
                << " audio_frames=" << stats.audioFramesCompleted
                << " audio_bytes=" << stats.audioCompletedPacketBytes
                << " audio_pending_packets=" << receiver.pendingAudioPacketCount()
                << " audio_completed_queue=" << receiver.completedAudioPacketCount()
                << " audio_incomplete_dropped=" << stats.audioIncompletePacketsDropped
                << " audio_duplicate_fragments=" << stats.audioDuplicateFragments
                << " audio_silent_packets=" << stats.audioSilentPackets
                << " audio_discontinuities=" << stats.audioDiscontinuities
                << " audio_timestamp_errors=" << stats.audioTimestampErrors
                << " audio_format_changes=" << stats.audioFormatChanges
                << " audio_format=" << stats.audioSampleRate << "x" << stats.audioChannels
                << "x" << stats.audioBitsPerSample
                << "/" << screenshare::udp_protocol::AudioSampleFormatName(stats.audioSampleFormat)
                << " audio_codec=" << screenshare::udp_protocol::AudioCodecName(stats.audioCodec)
                << " audio_playback=" << audioPlaybackStatus
                << " audio_playback_muted=" << (audioPlaybackMuted ? "yes" : "no")
                << " audio_playback_volume_percent=" << static_cast<int>(std::lround(audioPlaybackVolume * 100.0f))
                << " audio_playback_latency_ms=" << (options.audioPlayback ? audioPlayout.targetLatency().count() : 0)
                << " audio_playback_queue=" << (options.audioPlayback ? audioPlayout.queuedPacketCount() : 0)
                << " audio_playback_queue_ms=" << (options.audioPlayback ? audioPlayout.QueuedDuration().count() : 0)
                << " audio_playback_packets=" << (options.audioPlayback ? audioPlayout.packetsRendered() : 0)
                << " audio_playback_frames=" << (options.audioPlayback ? audioPlayout.framesRendered() : 0)
                << " audio_playback_starts=" << audioPlaybackStarts
                << " audio_playback_format_changes=" << audioPlaybackFormatChanges
                << " audio_playback_drops=" << (options.audioPlayback ?
                    audioPlayout.lateDrops() + audioPlayout.duplicateDrops() + audioPlayout.overflowDrops() + audioPlayout.oversizedDrops() :
                    0)
                << " audio_playback_latency_drops=" << (options.audioPlayback ? audioPlayout.latencyDrops() : 0)
                << " audio_playback_sync_drops=" << (options.audioPlayback ? audioPlayout.syncDrops() : 0)
                << " audio_playback_sync_waits=" << (options.audioPlayback ? audioPlayout.syncWaits() : 0)
                << " audio_playback_missing=" << (options.audioPlayback ? audioPlayout.missingPacketsSkipped() : 0)
                << " audio_playback_backpressure=" << (options.audioPlayback ? audioPlayout.renderBackpressure() : 0)
                << " audio_render_buffer_full=" << audioRendererStats.bufferFullEvents
                << " audio_render_padding=" << audioRendererStats.lastPaddingFrames
                << " av_sync=" << avSyncStateName(avSyncNow)
                << " av_audio_ahead_ms=" << avSyncNow.audioAheadMs
                << " av_audio_elapsed_ms=" << avSyncNow.audioElapsedMs
                << " av_video_elapsed_ms=" << avSyncNow.videoElapsedMs
                << " av_sender_clock=" << (avSyncNow.senderClockReady ? "ready" : "waiting")
                << " av_initial_audio_offset_ms=" << avSyncNow.initialAudioSenderOffsetMs
                << " av_sender_audio_ahead_ms=" << avSyncNow.senderClockAudioAheadMs
                << " av_sender_timeline_audio_ahead_ms=" << avSyncNow.senderTimelineAudioAheadMs
                << " av_playout_ready=" << (avSyncPlayoutReadyNow ? "yes" : "no")
                << " av_playout_audio_ahead_ms=" << avSyncPlayoutAudioAheadMs
                << " av_sync_correction=" << avSyncCorrectionStatus
                << " av_sync_start_qpc=" << (avSyncStartQpc100ns ? *avSyncStartQpc100ns : 0)
                << " av_sync_playback_start_aligned=" << (avSyncPlaybackStartAligned ? "yes" : "no")
                << " av_sync_playback_start_qpc=" << avSyncPlaybackStartQpc100ns
                << " av_sync_video_start_drops=" << avSyncVideoStartDrops
                << " av_sync_audio_start_drops=" << avSyncAudioStartDrops
                << " av_sync_audio_catchup_drops=" << avSyncAudioCatchupDrops
                << " av_sync_audio_gate_bypasses=" << avSyncAudioGateBypasses
                << " av_sync_preview_bias_ms=" << avSyncPreviewBiasMs
                << " av_sync_audio_bias_ms=" << avSyncAudioBiasMs
                << " av_video_frames=" << avSyncNow.videoFrames
                << " av_audio_packets=" << avSyncNow.audioPackets
                << " av_ignored_audio_packets=" << avSyncNow.ignoredAudioPackets
                << " av_latest_video_timestamp=" << avSyncNow.latestVideoTimestamp100ns
                << " av_latest_video_sender_qpc=" << avSyncNow.latestVideoSenderQpc100ns
                << " av_latest_audio_qpc=" << avSyncNow.latestAudioQpc100ns
                << " invalid_datagrams=" << stats.invalidDatagrams
                << " duplicate_fragments=" << stats.duplicateFragments
                << " completed_frames=" << stats.framesCompleted
                << " completed_fps=" << completedFps
                << " pending_frames=" << receiver.pendingFrameCount()
                << " incomplete_dropped=" << stats.incompleteFramesDropped
                << " payload_bytes=" << stats.payloadBytesReceived
                << " completed_bytes=" << stats.completedFrameBytes
                << " dumped_h264_packets=" << h264DumpPackets
                << " dumped_h264_bytes=" << h264DumpBytes
                << " pending_h264_dump_packets=" << h264DumpBacklog.size()
                << " h264_decode_packets=" << h264DecodePackets
                << " h264_decode_avg_ms=" << h264DecodeAvgMs
                << " h264_decoded_frames=" << h264DecodedFrames
                << " h264_decoded_bytes=" << h264DecodedBytes
                << " h264_decode_resyncs=" << h264DecodeResyncs
                << " h264_decode_restarts=" << h264DecodeDecoderRestarts
                << " h264_decode_skipped_packets=" << h264DecodeSkippedPackets
                << " h264_decoded_output=" << h264DecodedWidth << "x" << h264DecodedHeight
                << " pending_h264_decode_packets=" << h264DecodeBacklog.size()
                << " preview_frames_presented=" << (previewWindow ? previewWindow->framesPresented() : 0)
                << " preview_queue=" << (previewWindow ? previewPlayout.queuedFrameCount() : 0)
                << " video_playout_delay_avg_ms=" << (previewWindow ? previewPlayout.averagePresentDelayMs() : 0.0)
                << " video_playout_delay_max_ms=" << (previewWindow ? previewPlayout.maxPresentDelayMs() : 0.0)
                << " video_playout_delay_last_ms=" << (previewWindow ? previewPlayout.lastPresentDelayMs() : 0.0)
                << " preview_latency_ms=" << (previewWindow ? previewPlayout.initialDelay().count() : 0)
                << " preview_max_late_ms=" << (previewWindow ? previewPlayout.maxLateFrameAge().count() : 0)
                << " preview_playout_resets=" << (previewWindow ? previewPlayoutResets : 0)
                << " preview_late_drops=" << (previewWindow ? previewPlayout.lateDrops() : 0)
                << " preview_overflow_drops=" << (previewWindow ? previewPlayout.overflowDrops() : 0)
                << " preview_sync_drops=" << (previewWindow ? previewPlayout.syncDrops() : 0)
                << " preview_sync_waits=" << (previewWindow ? previewPlayout.syncWaits() : 0);

            if (hasCompletedFrame) {
                std::cout
                    << " latest_frame=" << latestFrameId
                    << " latest_frame_bytes=" << latestFrameBytes
                    << " latest_fragments=" << latestFragmentCount;
            }

            std::cout << "\n" << std::flush;

            updateReportBaselines(stats, previewLateDrops, previewOverflowDrops, now);
            intervalH264DecodeMs = 0.0;
            intervalH264DecodeCalls = 0;
        }
    }

    if (previewCloseRequested) {
        receiver.Close();
        std::cout << "watch_stop=preview_closed\n" << std::flush;
        return;
    }

    drainCompletedAudioPackets();
    screenshare::UdpReceiverStats stats = receiver.stats();
    if (h264Decoder) {
        flushH264DecodeBacklog(true);
        auto drainedFrames = h264Decoder->Drain();
        countDecodedFrames(std::move(drainedFrames), false);
        h264Decoder.reset();
    }
    if (shouldDumpH264) {
        flushH264DumpBacklog(true);
    }
    if (previewWindow) {
        presentPreviewReady(true);
    }
    drainCompletedAudioPackets();

    bool decodedBmpWritten = false;
    if (!options.decodedBmpPath.empty() && latestDecodedFrame) {
        WriteDecodedFrameBmp(options.decodedBmpPath, *latestDecodedFrame);
        decodedBmpWritten = true;
    }
    const size_t delayedDatagrams = receiver.delayedDatagramCount();
    const size_t pendingAudioPackets = receiver.pendingAudioPacketCount();
    const size_t completedAudioPackets = receiver.completedAudioPacketCount();
    const double totalElapsed = std::chrono::duration<double>(Clock::now() - startedAt).count();
    const uint64_t finalPreviewLateDrops = previewWindow ? previewPlayout.lateDrops() : 0;
    const uint64_t finalPreviewOverflowDrops = previewWindow ? previewPlayout.overflowDrops() : 0;
    const AvSyncSnapshot finalAvSync = avSync.snapshot();
    avSyncPlayoutAudioAheadMs = estimateAvSyncPlayoutAudioAheadMs(finalAvSync);
    const bool finalAvSyncPlayoutReady = avSyncPlayoutReady(finalAvSync);
    const ReceiverHealthSnapshot finalHealth{
        stats.framesCompleted,
        static_cast<double>(stats.framesCompleted) / totalElapsed,
        receiver.pendingFrameCount(),
        h264DecodeBacklog.size(),
        previewWindow ? previewPlayout.queuedFrameCount() : 0,
        stats.simulatedDatagramsDropped,
        stats.invalidDatagrams,
        stats.incompleteFramesDropped,
        h264DecodeResyncs,
        h264DecodeSkippedPackets,
        previewWindow ? previewWindow->framesPresented() : 0,
        finalPreviewLateDrops,
        finalPreviewOverflowDrops,
        h264DecodedWidth,
        h264DecodedHeight,
        previewWindow ? previewPlayoutResets : 0,
        previewWindow ? static_cast<int>(previewPlayout.initialDelay().count()) : 0,
        previewWindow ? static_cast<int>(previewPlayout.maxLateFrameAge().count()) : 0,
        stats.simulatedDatagramsDropped - lastSimulatedDroppedDatagrams,
        stats.invalidDatagrams - lastInvalidDatagrams,
        stats.incompleteFramesDropped - lastIncompleteFramesDropped,
        h264DecodeResyncs - lastH264DecodeResyncs,
        h264DecodeSkippedPackets - lastH264DecodeSkippedPackets,
        finalPreviewLateDrops - lastPreviewLateDrops,
        finalPreviewOverflowDrops - lastPreviewOverflowDrops,
    };
    if (previewWindow) {
        latestReceiverHealthTitle = FormatReceiverHealthTitle(
            finalHealth,
            finalAvSync,
            avSyncPlayoutAudioAheadMs,
            finalAvSyncPlayoutReady);
        updatePreviewTitle();
    }
    receiver.SendFeedback(BuildReceiverFeedbackSnapshot(
        finalHealth,
        feedbackSequence++,
        options.sessionFingerprint,
        options.accessCodeFingerprint));
    stats = receiver.stats();
    const screenshare::AudioPlaybackStats finalAudioRendererStats =
        audioRenderer ? audioRenderer->stats() : screenshare::AudioPlaybackStats{};
    receiver.Close();

    std::cout
        << "Done. UDP datagrams: " << stats.datagramsReceived
        << ", session: " << options.sessionId
        << ", session fingerprint: " << FormatSessionFingerprint(options.sessionFingerprint)
        << ", receiver health: " << ReceiverHealthState(finalHealth)
        << ", NAT status: " << ReceiverNatStatus(hasNatProbeSetup, stats, hasReceivedStreamTraffic)
        << ", NAT hint: " << ReceiverNatHint(hasNatProbeSetup, stats, hasReceivedStreamTraffic)
        << ", accepted datagrams: " << stats.datagramsAccepted
        << ", access rejected datagrams: " << stats.accessRejectedDatagrams
        << ", crypto rejected datagrams: " << stats.cryptoRejectedDatagrams
        << ", simulated dropped datagrams: " << stats.simulatedDatagramsDropped
        << ", simulated delayed datagrams: " << stats.simulatedDatagramsDelayed
        << ", pending simulated delayed datagrams: " << delayedDatagrams
        << ", feedback packets sent: " << stats.feedbackPacketsSent
        << ", feedback send errors: " << stats.feedbackSendErrors
        << ", encrypted feedback packets sent: " << stats.encryptedFeedbackPacketsSent
        << ", NAT probe public packets sent: " << stats.natProbePublicPacketsSent
        << ", NAT probe local packets sent: " << stats.natProbeLocalPacketsSent
        << ", NAT probe send errors: " << stats.natProbeSendErrors
        << ", receiver stream restarts: " << receiverStreamRestarts
        << ", receiver stale stream frames: " << receiverStreamStaleFrames
        << ", audio datagrams: " << stats.audioDatagramsAccepted
        << ", audio packets: " << stats.audioPacketsCompleted
        << ", audio queued packets: " << stats.audioPacketsQueued
        << ", audio queue dropped: " << stats.audioQueuedPacketsDropped
        << ", audio frames: " << stats.audioFramesCompleted
        << ", audio bytes: " << stats.audioCompletedPacketBytes
        << ", pending audio packets: " << pendingAudioPackets
        << ", completed audio queue: " << completedAudioPackets
        << ", incomplete audio packets dropped: " << stats.audioIncompletePacketsDropped
        << ", audio duplicate fragments: " << stats.audioDuplicateFragments
        << ", audio silent packets: " << stats.audioSilentPackets
        << ", audio discontinuities: " << stats.audioDiscontinuities
        << ", audio timestamp errors: " << stats.audioTimestampErrors
        << ", audio format changes: " << stats.audioFormatChanges
        << ", audio format: " << stats.audioSampleRate << "x" << stats.audioChannels
        << "x" << stats.audioBitsPerSample
        << "/" << screenshare::udp_protocol::AudioSampleFormatName(stats.audioSampleFormat)
        << ", audio codec: " << screenshare::udp_protocol::AudioCodecName(stats.audioCodec)
        << ", audio playback: " << audioPlaybackStatus
        << ", audio playback muted: " << (audioPlaybackMuted ? "yes" : "no")
        << ", audio playback volume percent: " << static_cast<int>(std::lround(audioPlaybackVolume * 100.0f))
        << ", audio playback latency ms: " << (options.audioPlayback ? audioPlayout.targetLatency().count() : 0)
        << ", audio playback queued packets: " << (options.audioPlayback ? audioPlayout.queuedPacketCount() : 0)
        << ", audio playback queued ms: " << (options.audioPlayback ? audioPlayout.QueuedDuration().count() : 0)
        << ", audio playback packets: " << (options.audioPlayback ? audioPlayout.packetsRendered() : 0)
        << ", audio playback frames: " << (options.audioPlayback ? audioPlayout.framesRendered() : 0)
        << ", audio playback starts: " << audioPlaybackStarts
        << ", audio playback format changes: " << audioPlaybackFormatChanges
        << ", audio playback drops: " << (options.audioPlayback ?
            audioPlayout.lateDrops() + audioPlayout.duplicateDrops() + audioPlayout.overflowDrops() + audioPlayout.oversizedDrops() :
            0)
        << ", audio playback latency drops: " << (options.audioPlayback ? audioPlayout.latencyDrops() : 0)
        << ", audio playback sync drops: " << (options.audioPlayback ? audioPlayout.syncDrops() : 0)
        << ", audio playback sync waits: " << (options.audioPlayback ? audioPlayout.syncWaits() : 0)
        << ", audio playback missing packets: " << (options.audioPlayback ? audioPlayout.missingPacketsSkipped() : 0)
        << ", audio playback backpressure: " << (options.audioPlayback ? audioPlayout.renderBackpressure() : 0)
        << ", audio render buffer full events: " << finalAudioRendererStats.bufferFullEvents
        << ", A/V sync: " << avSyncStateName(finalAvSync)
        << ", A/V audio ahead ms: " << finalAvSync.audioAheadMs
        << ", A/V audio elapsed ms: " << finalAvSync.audioElapsedMs
        << ", A/V video elapsed ms: " << finalAvSync.videoElapsedMs
        << ", A/V sender clock: " << (finalAvSync.senderClockReady ? "ready" : "waiting")
        << ", A/V initial audio offset ms: " << finalAvSync.initialAudioSenderOffsetMs
        << ", A/V sender audio ahead ms: " << finalAvSync.senderClockAudioAheadMs
        << ", A/V sender timeline audio ahead ms: " << finalAvSync.senderTimelineAudioAheadMs
        << ", A/V playout ready: " << (finalAvSyncPlayoutReady ? "yes" : "no")
        << ", A/V playout audio ahead ms: " << avSyncPlayoutAudioAheadMs
        << ", A/V sync correction: " << avSyncCorrectionStatus
        << ", A/V sync start qpc: " << (avSyncStartQpc100ns ? *avSyncStartQpc100ns : 0)
        << ", A/V sync playback start aligned: " << (avSyncPlaybackStartAligned ? "yes" : "no")
        << ", A/V sync playback start qpc: " << avSyncPlaybackStartQpc100ns
        << ", A/V sync video start drops: " << avSyncVideoStartDrops
        << ", A/V sync audio start drops: " << avSyncAudioStartDrops
        << ", A/V sync audio catchup drops: " << avSyncAudioCatchupDrops
        << ", A/V sync audio gate bypasses: " << avSyncAudioGateBypasses
        << ", A/V sync preview bias ms: " << avSyncPreviewBiasMs
        << ", A/V sync audio bias ms: " << avSyncAudioBiasMs
        << ", A/V video frames: " << finalAvSync.videoFrames
        << ", A/V audio packets: " << finalAvSync.audioPackets
        << ", A/V ignored audio packets: " << finalAvSync.ignoredAudioPackets
        << ", A/V latest video timestamp: " << finalAvSync.latestVideoTimestamp100ns
        << ", A/V latest video sender qpc: " << finalAvSync.latestVideoSenderQpc100ns
        << ", A/V latest audio qpc: " << finalAvSync.latestAudioQpc100ns
        << ", invalid datagrams: " << stats.invalidDatagrams
        << ", duplicate fragments: " << stats.duplicateFragments
        << ", completed frames: " << stats.framesCompleted
        << ", average completed FPS: " << (static_cast<double>(stats.framesCompleted) / totalElapsed)
        << ", incomplete frames dropped: " << stats.incompleteFramesDropped
        << ", payload bytes: " << stats.payloadBytesReceived
        << ", completed bytes: " << stats.completedFrameBytes
        << ", dumped H.264 packets: " << h264DumpPackets
        << ", dumped H.264 bytes: " << h264DumpBytes
        << ", pending H.264 dump packets: " << h264DumpBacklog.size()
        << ", H.264 decode packets: " << h264DecodePackets
        << ", average H.264 decode ms: "
        << (totalH264DecodeCalls == 0 ? 0.0 : totalH264DecodeMs / static_cast<double>(totalH264DecodeCalls))
        << ", H.264 decoded frames: " << h264DecodedFrames
        << ", H.264 decoded bytes: " << h264DecodedBytes
        << ", H.264 decode resyncs: " << h264DecodeResyncs
        << ", H.264 decoder restarts: " << h264DecodeDecoderRestarts
        << ", H.264 decode skipped packets: " << h264DecodeSkippedPackets
        << ", H.264 decoded output: " << h264DecodedWidth << "x" << h264DecodedHeight
        << ", pending H.264 decode packets: " << h264DecodeBacklog.size()
        << ", preview frames presented: " << (previewWindow ? previewWindow->framesPresented() : 0)
        << ", preview queued frames: " << (previewWindow ? previewPlayout.queuedFrameCount() : 0)
        << ", video playout delay avg ms: " << (previewWindow ? previewPlayout.averagePresentDelayMs() : 0.0)
        << ", video playout delay max ms: " << (previewWindow ? previewPlayout.maxPresentDelayMs() : 0.0)
        << ", video playout delay last ms: " << (previewWindow ? previewPlayout.lastPresentDelayMs() : 0.0)
        << ", preview latency ms: " << (previewWindow ? previewPlayout.initialDelay().count() : 0)
        << ", preview max late ms: " << (previewWindow ? previewPlayout.maxLateFrameAge().count() : 0)
        << ", preview playout resets: " << (previewWindow ? previewPlayoutResets : 0)
        << ", preview late drops: " << (previewWindow ? previewPlayout.lateDrops() : 0)
        << ", preview overflow drops: " << (previewWindow ? previewPlayout.overflowDrops() : 0)
        << ", preview sync drops: " << (previewWindow ? previewPlayout.syncDrops() : 0)
        << ", preview sync waits: " << (previewWindow ? previewPlayout.syncWaits() : 0)
        << ", decoded BMP written: " << (decodedBmpWritten ? "yes" : "no")
        << "\n";
}

} // namespace

namespace screenshare_runtime_internal {

int ExecuteSessionRuntimeOptions(
    Options& options,
    SavedReportContext& reportContext,
    const ScreenShareRunContext& context)
{
    PrepareLiveSignaling(options);
    screenshare::FileSessionRuntimeControl fileRuntimeControl(
        options.stopFilePath,
        options.controlFilePath);
    screenshare::ISessionRuntimeControl& runtimeControl =
        context.runtimeControl != nullptr ? *context.runtimeControl : fileRuntimeControl;
    reportContext.sessionId = options.sessionId;
    reportContext.sessionFingerprint = options.sessionFingerprint;
    reportContext.accessCodeRequired = options.accessCodeProvided;
    reportContext.encryptionEnabled = options.accessCodeKey.has_value();

    if (options.audioCapture && options.udpSendTarget.empty() && !options.shareRoom) {
        RunAudioCaptureStats(options, reportContext, runtimeControl);
    } else if (options.udpReceivePort != 0) {
        RunUdpReceiverStats(options, context, runtimeControl);
    } else {
        RunCaptureStats(options, reportContext, runtimeControl);
    }
    return 0;
}

int ExecuteShareSessionConfig(
    const screenshare::ShareSessionConfig& config,
    SavedReportContext& reportContext,
    const ScreenShareRunContext& context)
{
    Options options = BuildShareSessionOptions(config, reportContext.sessionId);
    return ExecuteSessionRuntimeOptions(options, reportContext, context);
}

int ExecuteWatchSessionConfig(
    const screenshare::WatchSessionConfig& config,
    SavedReportContext& reportContext,
    const ScreenShareRunContext& context)
{
    Options options = BuildWatchSessionOptions(config, reportContext.sessionId);
    return ExecuteSessionRuntimeOptions(options, reportContext, context);
}

} // namespace screenshare_runtime_internal
