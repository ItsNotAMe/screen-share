#include "capture/DesktopCapturer.h"
#include "codec/H264Bitstream.h"
#include "codec/H264EncoderProbe.h"
#include "codec/H264FileEncoder.h"
#include "codec/H264StreamDecoder.h"
#include "codec/H264StreamEncoder.h"
#include "render/ReceiverPreviewWindow.h"
#include "transport/UdpReceiver.h"
#include "transport/UdpSender.h"
#include "video/Nv12Convert.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

enum class StreamEncoderPreference {
    Auto,
    Software,
    Hardware,
};

constexpr size_t OrderedReceiverStartThresholdFrames = 30;
constexpr size_t OrderedReceiverRecoveryThresholdFrames = 30;
constexpr size_t ReceiverHealthPendingFrameWarning = 8;
constexpr uint64_t SenderQueuePressureDatagrams = 128;

struct Options {
    bool listDisplays = false;
    bool listH264Encoders = false;
    int displayIndex = 0;
    int width = 0;
    int height = 0;
    int fps = 60;
    int seconds = 10;
    screenshare::CaptureBackend captureBackend = screenshare::CaptureBackend::WindowsGraphicsCapture;
    uint32_t bitrate = 0;
    int keyframeIntervalSeconds = 2;
    bool keyframeIntervalProvided = false;
    std::string recordPath;
    std::string capturedBmpPath;
    bool streamEncode = false;
    bool streamEncoderPreferenceProvided = false;
    StreamEncoderPreference streamEncoderPreference = StreamEncoderPreference::Auto;
    std::string udpSendTarget;
    bool udpPacing = true;
    bool udpPacingOptionProvided = false;
    bool adaptBitrate = false;
    bool wgcBorderRequired = false;
    bool hdrToSdr = true;
    float hdrSdrWhiteNits = 203.0f;
    float hdrSdrBgraExposure = 0.88f;
    uint16_t udpReceivePort = 0;
    std::string h264DumpPath;
    bool decodeH264 = false;
    std::string decodedBmpPath;
    bool previewWindow = false;
    float simulateLossPercent = 0.0f;
    bool simulateLossProvided = false;
    int simulateJitterMs = 0;
    bool simulateJitterProvided = false;
};

void PrintHelp()
{
    std::cout
        << "ScreenShare native C++ capture prototype\n\n"
        << "Usage:\n"
        << "  ScreenShare --list\n"
        << "  ScreenShare --list-h264-encoders [--width W --height H] [--fps FPS] [--bitrate-mbps Mbps]\n"
        << "  ScreenShare --udp-recv PORT [--seconds S] [--dump-h264 PATH] [--decode-h264]\n"
        << "              [--dump-decoded-bmp PATH] [--preview]\n"
        << "              [--simulate-loss-percent P] [--simulate-jitter-ms MS]\n"
        << "  ScreenShare [--display N] [--width W --height H] [--fps FPS] [--seconds S]\n"
        << "              [--record PATH] [--stream-encode] [--stream-encoder auto|software|hardware]\n"
        << "              [--udp-send HOST:PORT] [--no-udp-pacing] [--adapt-bitrate]\n"
        << "              [--dump-capture-bmp PATH]\n"
        << "              [--capture-backend dxgi|wgc]\n"
        << "              [--bitrate-mbps Mbps] [--keyframe-interval S]\n"
        << "              [--wgc-border] [--no-hdr-to-sdr]\n"
        << "              [--hdr-sdr-white-nits N] [--hdr-sdr-exposure N]\n\n"
        << "Examples:\n"
        << "  ScreenShare --list\n"
        << "  ScreenShare --list-h264-encoders --width 1920 --height 1080 --fps 60\n"
        << "  ScreenShare --display 0 --width 1920 --height 1080 --fps 60 --seconds 15\n"
        << "  ScreenShare --display 0 --fps 60 --seconds 15 --record native.mp4\n"
        << "  ScreenShare --udp-recv 5000 --seconds 15 --dump-decoded-bmp receiver.bmp\n"
        << "  ScreenShare --udp-recv 5000 --preview\n"
        << "  ScreenShare --display 0 --width 1280 --height 720 --fps 60 --seconds 15 --udp-send 127.0.0.1:5000\n";
}

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

int ParseInt(const char* value, const char* name)
{
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        throw std::invalid_argument(std::string("Invalid integer for ") + name + ": " + value);
    }
    return static_cast<int>(parsed);
}

uint32_t ParseBitrateMbps(const char* value)
{
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || *end != '\0' || parsed <= 0.0) {
        throw std::invalid_argument(std::string("Invalid value for --bitrate-mbps: ") + value);
    }
    return static_cast<uint32_t>(parsed * 1'000'000.0);
}

float ParseFloat(const char* value, const char* name)
{
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || *end != '\0') {
        throw std::invalid_argument(std::string("Invalid value for ") + name + ": " + value);
    }
    return static_cast<float>(parsed);
}

screenshare::CaptureBackend ParseCaptureBackend(const char* value)
{
    const std::string backend = value;
    if (backend == "dxgi" || backend == "desktop-duplication") {
        return screenshare::CaptureBackend::DesktopDuplication;
    }
    if (backend == "wgc" || backend == "windows-graphics-capture") {
        return screenshare::CaptureBackend::WindowsGraphicsCapture;
    }

    throw std::invalid_argument(std::string("Invalid value for --capture-backend: ") + value);
}

StreamEncoderPreference ParseStreamEncoderPreference(const char* value)
{
    const std::string preference = value;
    if (preference == "auto") {
        return StreamEncoderPreference::Auto;
    }
    if (preference == "software") {
        return StreamEncoderPreference::Software;
    }
    if (preference == "hardware") {
        return StreamEncoderPreference::Hardware;
    }

    throw std::invalid_argument(std::string("Invalid value for --stream-encoder: ") + value);
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

class PreviewPlayoutBuffer {
public:
    using Clock = std::chrono::steady_clock;

    void Enqueue(screenshare::DecodedFrameInfo frame)
    {
        int64_t key = frame.timestamp100ns;
        while (frames_.contains(key)) {
            ++key;
        }

        frames_.emplace(key, std::move(frame));
        while (frames_.size() > maxQueuedFrames_) {
            frames_.erase(frames_.begin());
            ++overflowDrops_;
        }
    }

    void PresentReady(screenshare::ReceiverPreviewWindow& previewWindow, Clock::time_point now, bool flush)
    {
        if (frames_.empty()) {
            return;
        }
        EnsureClockStarted(now);

        while (!frames_.empty()) {
            const auto frameTime = PresentationTime(frames_.begin()->first);
            if (!flush && now < frameTime) {
                break;
            }
            if (!flush && frames_.size() > 1 && now - frameTime > maxLateFrameAge_) {
                frames_.erase(frames_.begin());
                ++lateDrops_;
                continue;
            }

            auto frame = std::move(frames_.begin()->second);
            frames_.erase(frames_.begin());
            previewWindow.PresentFrame(frame);
        }
    }

    void ClearPendingAndRestartClock()
    {
        frames_.clear();
        clockStarted_ = false;
        firstTimestamp100ns_ = 0;
        firstPresentationAt_ = {};
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

    std::map<int64_t, screenshare::DecodedFrameInfo> frames_;
    Clock::time_point firstPresentationAt_{};
    int64_t firstTimestamp100ns_ = 0;
    bool clockStarted_ = false;
    uint64_t lateDrops_ = 0;
    uint64_t overflowDrops_ = 0;
    size_t maxQueuedFrames_ = 180;
    std::chrono::milliseconds initialDelay_{100};
    std::chrono::milliseconds maxLateFrameAge_{250};
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
    if (health.previewLateDrops > 0 || health.previewOverflowDrops > 0) {
        return screenshare::udp_protocol::FeedbackHealthState::PreviewDrop;
    }
    if (health.h264DecodeResyncs > 0 || health.h264DecodeSkippedPackets > 0) {
        return screenshare::udp_protocol::FeedbackHealthState::Recovering;
    }
    if (health.simulatedDroppedDatagrams > 0 || health.invalidDatagrams > 0 || health.incompleteDroppedFrames > 0) {
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
    uint64_t sequence)
{
    screenshare::udp_protocol::FeedbackSnapshot feedback;
    feedback.healthState = ReceiverFeedbackHealthState(health);
    feedback.sequence = sequence;
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

void DrainUdpFeedback(screenshare::UdpSender& udpSender, std::chrono::milliseconds firstTimeout)
{
    if (!udpSender.ReceiveFeedback(firstTimeout)) {
        return;
    }

    while (udpSender.ReceiveFeedback(std::chrono::milliseconds(0))) {
    }
}

class AdaptiveBitrateAdvisor {
public:
    void Configure(uint32_t targetBitrate)
    {
        targetBitrate_ = targetBitrate;
        recommendedBitrate_ = targetBitrate;
        minBitrate_ = std::max<uint32_t>(1'000'000, targetBitrate / 4);
        stableFeedbackCount_ = 0;
        hasFeedback_ = false;
        lastFeedbackSequence_ = 0;
        lastDropSignals_ = 0;
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

        const uint64_t dropSignals =
            feedback.droppedDatagrams +
            feedback.invalidDatagrams +
            feedback.incompleteFramesDropped +
            feedback.previewLateDrops +
            feedback.previewOverflowDrops;
        const bool newDropSignal = !hasFeedback_ ? dropSignals > 0 : dropSignals > lastDropSignals_;
        const bool newDecodeRecovery =
            (!hasFeedback_ ? feedback.decodeResyncs > 0 : feedback.decodeResyncs > lastResyncs_) ||
            (!hasFeedback_ ? feedback.decodeSkippedPackets > 0 : feedback.decodeSkippedPackets > lastSkippedPackets_);
        const bool queuePressure =
            feedback.healthState == screenshare::udp_protocol::FeedbackHealthState::Buffering ||
            feedback.pendingFrames >= ReceiverHealthPendingFrameWarning ||
            feedback.pendingDecodePackets >= OrderedReceiverRecoveryThresholdFrames ||
            (stats.pendingDatagrams >= SenderQueuePressureDatagrams &&
             stats.peakPendingDatagrams > 0 &&
             stats.pendingDatagrams >= stats.peakPendingDatagrams / 2);

        if (newDropSignal || newDecodeRecovery || queuePressure) {
            const uint32_t reduced = static_cast<uint32_t>(
                static_cast<uint64_t>(recommendedBitrate_) * 80 / 100);
            recommendedBitrate_ = std::max(minBitrate_, reduced);
            stableFeedbackCount_ = 0;
            action_ = "reduce";
            reason_ = newDecodeRecovery ? "receiver_recovery" : (newDropSignal ? "receiver_loss" : "queue_pressure");
        } else if (feedback.healthState == screenshare::udp_protocol::FeedbackHealthState::Ok &&
                   recommendedBitrate_ < targetBitrate_) {
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
            stableFeedbackCount_ = 0;
            action_ = "hold";
            reason_ = feedback.healthState == screenshare::udp_protocol::FeedbackHealthState::Ok ?
                "healthy" :
                "waiting_for_recovery";
        }

        hasFeedback_ = true;
        lastFeedbackSequence_ = feedback.sequence;
        lastDropSignals_ = dropSignals;
        lastResyncs_ = feedback.decodeResyncs;
        lastSkippedPackets_ = feedback.decodeSkippedPackets;
    }

    [[nodiscard]] uint32_t recommendedBitrate() const noexcept { return recommendedBitrate_; }
    [[nodiscard]] const char* action() const noexcept { return action_; }
    [[nodiscard]] const char* reason() const noexcept { return reason_; }
    [[nodiscard]] bool configured() const noexcept { return targetBitrate_ != 0; }

private:
    static constexpr uint32_t StableFeedbackReportsBeforeIncrease = 3;

    uint32_t targetBitrate_ = 0;
    uint32_t recommendedBitrate_ = 0;
    uint32_t minBitrate_ = 0;
    uint32_t stableFeedbackCount_ = 0;
    bool hasFeedback_ = false;
    uint64_t lastFeedbackSequence_ = 0;
    uint64_t lastDropSignals_ = 0;
    uint64_t lastResyncs_ = 0;
    uint64_t lastSkippedPackets_ = 0;
    const char* action_ = "hold";
    const char* reason_ = "waiting_for_feedback";
};

std::string FormatReceiverHealthTitle(const ReceiverHealthSnapshot& health)
{
    const uint64_t transportDrops =
        health.simulatedDroppedDatagrams + health.invalidDatagrams + health.incompleteDroppedFrames;
    const uint64_t previewDrops = health.previewLateDrops + health.previewOverflowDrops;

    std::ostringstream stream;
    stream << ReceiverHealthState(health)
           << " | fps " << std::fixed << std::setprecision(1) << health.completedFps
           << " | recvq " << health.pendingFrames
           << " | decq " << health.pendingDecodePackets
           << " | pvq " << health.previewQueuedFrames
           << " | resync " << health.h264DecodeResyncs
           << " | skip " << health.h264DecodeSkippedPackets
           << " | drops " << transportDrops << "/" << previewDrops
           << " | shown " << health.previewFramesPresented;
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

double Mbps(uint32_t bitrate)
{
    return static_cast<double>(bitrate) / 1'000'000.0;
}

Options ParseOptions(int argc, char** argv)
{
    Options options;
    bool secondsProvided = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        auto requireValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                throw std::invalid_argument(std::string("Missing value for ") + name);
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            PrintHelp();
            std::exit(0);
        }
        if (arg == "--list") {
            options.listDisplays = true;
        } else if (arg == "--list-h264-encoders") {
            options.listH264Encoders = true;
        } else if (arg == "--display") {
            options.displayIndex = ParseInt(requireValue("--display"), "--display");
        } else if (arg == "--width") {
            options.width = ParseInt(requireValue("--width"), "--width");
        } else if (arg == "--height") {
            options.height = ParseInt(requireValue("--height"), "--height");
        } else if (arg == "--fps") {
            options.fps = ParseInt(requireValue("--fps"), "--fps");
        } else if (arg == "--seconds") {
            options.seconds = ParseInt(requireValue("--seconds"), "--seconds");
            secondsProvided = true;
        } else if (arg == "--record") {
            options.recordPath = requireValue("--record");
        } else if (arg == "--dump-capture-bmp") {
            options.capturedBmpPath = requireValue("--dump-capture-bmp");
        } else if (arg == "--capture-backend") {
            options.captureBackend = ParseCaptureBackend(requireValue("--capture-backend"));
        } else if (arg == "--stream-encode") {
            options.streamEncode = true;
        } else if (arg == "--stream-encoder") {
            options.streamEncoderPreference = ParseStreamEncoderPreference(requireValue("--stream-encoder"));
            options.streamEncoderPreferenceProvided = true;
        } else if (arg == "--udp-send") {
            options.udpSendTarget = requireValue("--udp-send");
            options.streamEncode = true;
        } else if (arg == "--no-udp-pacing") {
            options.udpPacing = false;
            options.udpPacingOptionProvided = true;
        } else if (arg == "--adapt-bitrate") {
            options.adaptBitrate = true;
        } else if (arg == "--wgc-border") {
            options.wgcBorderRequired = true;
        } else if (arg == "--no-hdr-to-sdr") {
            options.hdrToSdr = false;
        } else if (arg == "--hdr-sdr-white-nits") {
            options.hdrSdrWhiteNits = ParseFloat(requireValue("--hdr-sdr-white-nits"), "--hdr-sdr-white-nits");
        } else if (arg == "--hdr-sdr-exposure") {
            options.hdrSdrBgraExposure = ParseFloat(requireValue("--hdr-sdr-exposure"), "--hdr-sdr-exposure");
        } else if (arg == "--udp-recv") {
            options.udpReceivePort = screenshare::ParseUdpReceivePort(requireValue("--udp-recv"));
        } else if (arg == "--dump-h264") {
            options.h264DumpPath = requireValue("--dump-h264");
        } else if (arg == "--decode-h264") {
            options.decodeH264 = true;
        } else if (arg == "--dump-decoded-bmp") {
            options.decodedBmpPath = requireValue("--dump-decoded-bmp");
            options.decodeH264 = true;
        } else if (arg == "--preview") {
            options.previewWindow = true;
            options.decodeH264 = true;
        } else if (arg == "--simulate-loss-percent") {
            options.simulateLossPercent = ParseFloat(requireValue("--simulate-loss-percent"), "--simulate-loss-percent");
            options.simulateLossProvided = true;
        } else if (arg == "--simulate-jitter-ms") {
            options.simulateJitterMs = ParseInt(requireValue("--simulate-jitter-ms"), "--simulate-jitter-ms");
            options.simulateJitterProvided = true;
        } else if (arg == "--bitrate-mbps") {
            options.bitrate = ParseBitrateMbps(requireValue("--bitrate-mbps"));
        } else if (arg == "--keyframe-interval") {
            options.keyframeIntervalSeconds = ParseInt(requireValue("--keyframe-interval"), "--keyframe-interval");
            options.keyframeIntervalProvided = true;
        } else {
            throw std::invalid_argument("Unknown argument: " + arg);
        }
    }

    if (options.fps <= 0 || options.fps > 240) {
        throw std::invalid_argument("--fps must be between 1 and 240");
    }
    if (options.previewWindow && !secondsProvided) {
        options.seconds = 0;
    }
    if (options.seconds < 0) {
        throw std::invalid_argument("--seconds must be non-negative");
    }
    if (options.seconds == 0 && !options.previewWindow) {
        throw std::invalid_argument("--seconds 0 is only supported with --preview");
    }
    if ((options.width == 0) != (options.height == 0)) {
        throw std::invalid_argument("--width and --height must be provided together");
    }
    if (options.width < 0 || options.height < 0) {
        throw std::invalid_argument("--width and --height must be positive");
    }
    if (options.width > 0 &&
        (options.listH264Encoders || !options.recordPath.empty() || options.streamEncode) &&
        ((options.width % 2) != 0 || (options.height % 2) != 0)) {
        throw std::invalid_argument("--list-h264-encoders, --record, and --stream-encode require even --width and --height for NV12");
    }
    if (!options.udpSendTarget.empty()) {
        static_cast<void>(screenshare::ParseUdpSenderTarget(options.udpSendTarget));
    }
    if (options.udpPacingOptionProvided && options.udpSendTarget.empty()) {
        throw std::invalid_argument("--no-udp-pacing requires --udp-send");
    }
    if (options.adaptBitrate && options.udpSendTarget.empty()) {
        throw std::invalid_argument("--adapt-bitrate requires --udp-send");
    }
    if (options.streamEncoderPreferenceProvided && !options.streamEncode) {
        throw std::invalid_argument("--stream-encoder requires --stream-encode or --udp-send");
    }
    if (options.keyframeIntervalProvided && !options.streamEncode) {
        throw std::invalid_argument("--keyframe-interval requires --stream-encode or --udp-send");
    }
    if (options.keyframeIntervalSeconds < 0 || options.keyframeIntervalSeconds > 30) {
        throw std::invalid_argument("--keyframe-interval must be between 0 and 30 seconds");
    }
    if (options.hdrSdrWhiteNits < 80.0f || options.hdrSdrWhiteNits > 1000.0f) {
        throw std::invalid_argument("--hdr-sdr-white-nits must be between 80 and 1000");
    }
    if (options.hdrSdrBgraExposure < 0.25f || options.hdrSdrBgraExposure > 2.0f) {
        throw std::invalid_argument("--hdr-sdr-exposure must be between 0.25 and 2.0");
    }
    if (options.simulateLossPercent < 0.0f || options.simulateLossPercent > 100.0f) {
        throw std::invalid_argument("--simulate-loss-percent must be between 0 and 100");
    }
    if (options.simulateJitterMs < 0 || options.simulateJitterMs > 5000) {
        throw std::invalid_argument("--simulate-jitter-ms must be between 0 and 5000");
    }
    if ((options.simulateLossProvided || options.simulateJitterProvided) && options.udpReceivePort == 0) {
        throw std::invalid_argument("--simulate-loss-percent and --simulate-jitter-ms require --udp-recv");
    }
    if (options.udpReceivePort != 0 &&
        (options.listDisplays || options.listH264Encoders || !options.recordPath.empty() || !options.capturedBmpPath.empty() ||
         options.streamEncode || options.streamEncoderPreferenceProvided || !options.udpSendTarget.empty() ||
         options.udpPacingOptionProvided || options.adaptBitrate || options.keyframeIntervalProvided)) {
        throw std::invalid_argument("--udp-recv cannot be combined with --list, --list-h264-encoders, --record, --dump-capture-bmp, --stream-encode, --stream-encoder, --udp-send, --no-udp-pacing, --adapt-bitrate, or --keyframe-interval");
    }
    if (options.listH264Encoders &&
        (options.listDisplays || !options.recordPath.empty() || !options.capturedBmpPath.empty() ||
         options.streamEncode || options.streamEncoderPreferenceProvided || !options.udpSendTarget.empty() ||
         options.udpPacingOptionProvided || options.adaptBitrate || options.keyframeIntervalProvided ||
         options.decodeH264 || options.previewWindow)) {
        throw std::invalid_argument("--list-h264-encoders can only be combined with --width, --height, --fps, and --bitrate-mbps");
    }
    if (!options.h264DumpPath.empty() && options.udpReceivePort == 0) {
        throw std::invalid_argument("--dump-h264 requires --udp-recv");
    }
    if (options.decodeH264 && options.udpReceivePort == 0) {
        throw std::invalid_argument("--decode-h264 requires --udp-recv");
    }
    if (!options.decodedBmpPath.empty() && options.udpReceivePort == 0) {
        throw std::invalid_argument("--dump-decoded-bmp requires --udp-recv");
    }
    if (options.previewWindow && options.udpReceivePort == 0) {
        throw std::invalid_argument("--preview requires --udp-recv");
    }

    return options;
}

void PrintDisplays()
{
    const auto displays = screenshare::DesktopCapturer::EnumerateDisplays();
    if (displays.empty()) {
        std::cout << "No displays found.\n";
        return;
    }

    for (const auto& display : displays) {
        const long width = display.right - display.left;
        const long height = display.bottom - display.top;

        std::cout
            << "[" << display.index << "] "
            << screenshare::Narrow(display.outputName)
            << " " << width << "x" << height
            << " at (" << display.left << "," << display.top << ")"
            << " adapter=\"" << screenshare::Narrow(display.adapterName) << "\""
            << " attached=" << (display.attachedToDesktop ? "yes" : "no")
            << "\n";
    }
}

void PrintH264Encoders(const Options& options)
{
    screenshare::H264EncoderProbeConfig config;
    config.width = options.width > 0 ? options.width : 1280;
    config.height = options.height > 0 ? options.height : 720;
    config.fps = options.fps;
    config.bitrate = SelectBitrate(options, config.width, config.height);

    const auto encoders = screenshare::ProbeH264Encoders(config);
    std::cout
        << "H.264 encoder probe for " << config.width << "x" << config.height
        << " at " << config.fps << " FPS, bitrate " << Mbps(config.bitrate) << " Mbps.\n";
    if (encoders.empty()) {
        std::cout << "No H.264 encoders found.\n";
        return;
    }

    for (size_t index = 0; index < encoders.size(); ++index) {
        const auto& encoder = encoders[index];
        std::cout
            << "[" << index << "] " << (encoder.friendlyName.empty() ? "(unnamed encoder)" : encoder.friendlyName)
            << "\n    kind=" << (encoder.hardware ? "hardware" : "software")
            << " async=" << (encoder.async ? "yes" : "no")
            << " async_unlocked=" << (encoder.asyncUnlocked ? "yes" : "no")
            << " d3d11_aware=" << (encoder.d3d11Aware ? "yes" : "no")
            << " d3d_manager=" << (encoder.d3dManagerAccepted ? "accepted" : (encoder.d3d11Aware ? "rejected" : "not_requested"))
            << " stream_types=" << (encoder.streamTypesAccepted ? "accepted" : "rejected")
            << "\n    clsid=" << (encoder.clsid.empty() ? "(unknown)" : encoder.clsid);
        if (!encoder.hardwareUrl.empty()) {
            std::cout << "\n    hardware_url=" << encoder.hardwareUrl;
        }
        if (!encoder.activationError.empty()) {
            std::cout << "\n    activation_error=" << encoder.activationError;
        }
        if (!encoder.d3dManagerError.empty()) {
            std::cout << "\n    d3d_manager_error=" << encoder.d3dManagerError;
        }
        if (!encoder.streamTypeError.empty()) {
            std::cout << "\n    stream_type_error=" << encoder.streamTypeError;
        }
        std::cout << "\n";
    }
}

void RunCaptureStats(const Options& options)
{
    StreamEncoderPreference streamEncoderPreference = options.streamEncoderPreference;

    screenshare::CaptureConfig config;
    config.displayIndex = options.displayIndex;
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

    std::cout << "Capturing display " << options.displayIndex
              << " at target " << options.fps << " FPS";

    if (options.width > 0 && options.height > 0) {
        std::cout << ", requested output " << options.width << "x" << options.height;
    } else {
        std::cout << ", requested output native resolution";
    }
    std::cout << ", capture backend " << CaptureBackendName(options.captureBackend);
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
        std::cout << ", UDP pacing " << (options.udpPacing ? "enabled" : "disabled");
        std::cout << ", adaptive bitrate " << (options.adaptBitrate ? "enabled" : "advice-only");
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
    uint64_t bitrateAdaptations = 0;
    uint64_t bitrateAdaptationFailures = 0;
    uint32_t lastBitrateAdaptationAttempt = 0;
    const char* bitrateAdaptationStatus = options.adaptBitrate ? "waiting" : "disabled";
    AdaptiveBitrateAdvisor bitrateAdvisor;
    const uint32_t streamKeyframeIntervalFrames =
        options.keyframeIntervalSeconds <= 0 ?
        0 :
        static_cast<uint32_t>(options.keyframeIntervalSeconds * options.fps);
    double intervalCaptureMs = 0.0;
    uint64_t intervalCaptureCalls = 0;
    double intervalStreamEncodeMs = 0.0;
    uint64_t intervalStreamEncodeCalls = 0;
    double totalCaptureMs = 0.0;
    uint64_t totalCaptureCalls = 0;
    double totalStreamEncodeMs = 0.0;
    uint64_t totalStreamEncodeCalls = 0;
    bool capturedBmpWritten = false;
    std::unique_ptr<screenshare::UdpSender> udpSender;

    auto applyAdaptiveBitrate = [&]() {
        if (!options.adaptBitrate || !udpSender || !streamEncoder || !bitrateAdvisor.configured()) {
            return;
        }

        const uint32_t recommendedBitrate = bitrateAdvisor.recommendedBitrate();
        if (recommendedBitrate == 0 || streamBitrate == 0) {
            bitrateAdaptationStatus = "waiting";
            return;
        }
        if (std::strcmp(bitrateAdvisor.action(), "reduce") != 0 || recommendedBitrate >= streamBitrate) {
            bitrateAdaptationStatus =
                recommendedBitrate > streamBitrate ? "holding_increase" : "holding";
            return;
        }
        if (recommendedBitrate == lastBitrateAdaptationAttempt) {
            return;
        }

        lastBitrateAdaptationAttempt = recommendedBitrate;
        if (streamEncoder->TryUpdateBitrate(recommendedBitrate)) {
            streamBitrate = recommendedBitrate;
            udpSender->SetPacingBitrate(streamBitrate);
            ++bitrateAdaptations;
            bitrateAdaptationStatus = "applied";
            std::cout
                << "Adaptive bitrate applied bitrate_mbps=" << Mbps(streamBitrate)
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

    const auto targetFrameTime = std::chrono::microseconds(1'000'000 / options.fps);
    auto nextFrameAt = Clock::now();

    while (Clock::now() - startedAt < std::chrono::seconds(options.seconds)) {
        std::this_thread::sleep_until(nextFrameAt);
        nextFrameAt += targetFrameTime;

        const auto captureStartedAt = Clock::now();
        const auto frame = capturer.TryCaptureFrame(std::chrono::milliseconds(0));
        const double captureMs = std::chrono::duration<double, std::milli>(Clock::now() - captureStartedAt).count();
        intervalCaptureMs += captureMs;
        ++intervalCaptureCalls;
        totalCaptureMs += captureMs;
        ++totalCaptureCalls;
        if (frame) {
            ++totalDesktopUpdates;
            ++intervalDesktopUpdates;
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
        }

        if (hasFrame) {
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
                    encoderConfig.fps = options.fps;
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
                    auto startStreamEncoder = [&](screenshare::H264StreamEncoderBackend backend) {
                        if (backend == screenshare::H264StreamEncoderBackend::Hardware &&
                            (!lastFrame.d3dDevice || !lastFrame.nv12Texture)) {
                            throw std::runtime_error("hardware stream encoder requires a captured D3D11 NV12 texture");
                        }

                        screenshare::H264StreamEncoderConfig encoderConfig;
                        encoderConfig.width = lastFrame.width;
                        encoderConfig.height = lastFrame.height;
                        encoderConfig.fps = options.fps;
                        encoderConfig.bitrate = SelectBitrate(options, lastFrame.width, lastFrame.height);
                        encoderConfig.keyframeIntervalFrames = streamKeyframeIntervalFrames;
                        streamBitrate = encoderConfig.bitrate;
                        if (!bitrateAdvisor.configured()) {
                            bitrateAdvisor.Configure(streamBitrate);
                        }
                        encoderConfig.backend = backend;
                        if (encoderConfig.backend == screenshare::H264StreamEncoderBackend::Hardware) {
                            encoderConfig.d3dDevice = lastFrame.d3dDevice;
                        }

                        auto encoder = std::make_unique<screenshare::H264StreamEncoder>();
                        encoder->Start(encoderConfig);

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
                                std::cerr << "Restarted capture with CPU-visible NV12 for software stream encoding.\n";
                                continue;
                            }

                            streamEncoder = startStreamEncoder(screenshare::H264StreamEncoderBackend::Software);
                        }
                    }

                    if (!options.udpSendTarget.empty()) {
                        auto udpConfig = screenshare::ParseUdpSenderTarget(options.udpSendTarget);
                        udpConfig.pacingEnabled = options.udpPacing;
                        udpConfig.pacingBitrate = streamBitrate;
                        udpSender = std::make_unique<screenshare::UdpSender>();
                        udpSender->Open(udpConfig);
                        std::cout
                            << "UDP sender pacing=" << (udpConfig.pacingEnabled ? "enabled" : "disabled")
                            << " bitrate_mbps=" << Mbps(udpConfig.pacingBitrate)
                            << " adaptive_bitrate=" << (options.adaptBitrate ? "enabled" : "advice-only")
                            << " max_queued_datagrams=" << udpConfig.maxQueuedDatagrams
                            << "\n";
                    }
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
                streamPackets += packets.size();
                for (const auto& packet : packets) {
                    streamBytes += packet.bytes.size();
                    if (udpSender) {
                        udpSender->SendFrame(packet);
                    }
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
                DrainUdpFeedback(*udpSender, std::chrono::milliseconds(0));
            }
            const screenshare::UdpSenderStats udpStatsNow =
                udpSender ? udpSender->stats() : screenshare::UdpSenderStats{};
            if (udpSender) {
                bitrateAdvisor.Update(udpStatsNow);
                applyAdaptiveBitrate();
            }
            std::cout
                << "source=" << lastSourceWidth << "x" << lastSourceHeight
                << " source_format=" << screenshare::DxgiFormatName(lastSourceFormat)
                << " display_color_space=" << screenshare::DxgiColorSpaceName(lastDisplayColorSpace)
                << " display_hdr=" << (lastDisplayHdrActive ? "yes" : "no")
                << " color_conversion=" << screenshare::CaptureColorConversionName(lastColorConversionMode)
                << " output=" << lastOutputWidth << "x" << lastOutputHeight
                << " output_format=" << screenshare::DxgiFormatName(lastOutputFormat)
                << " nv12=" << (lastNv12TextureAvailable ? "gpu_texture" : (lastNv12GeneratedOnGpu ? "gpu_readback" : "cpu_or_none"))
                << " stream_input=" << (streamEncoder ? screenshare::H264StreamEncoderInputModeName(streamEncoder->lastInputMode()) : "none")
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
                << " udp_datagrams=" << udpStatsNow.datagramsSent
                << " udp_queued=" << udpStatsNow.datagramsQueued
                << " udp_pending=" << udpStatsNow.pendingDatagrams
                << " udp_peak_pending=" << udpStatsNow.peakPendingDatagrams
                << " udp_dropped_frames=" << udpStatsNow.framesDropped
                << " udp_wire_bytes=" << udpStatsNow.wireBytesSent
                << " udp_feedback_packets=" << udpStatsNow.feedbackPacketsReceived
                << " udp_feedback_invalid=" << udpStatsNow.invalidFeedbackPackets
                << " udp_feedback_health="
                << (udpStatsNow.hasFeedback ?
                    screenshare::udp_protocol::FeedbackHealthStateName(udpStatsNow.latestFeedback.healthState) :
                    "none")
                << " udp_feedback_completed_frames=" << (udpStatsNow.hasFeedback ? udpStatsNow.latestFeedback.completedFrames : 0)
                << " udp_feedback_resyncs=" << (udpStatsNow.hasFeedback ? udpStatsNow.latestFeedback.decodeResyncs : 0)
                << " udp_feedback_skipped_packets=" << (udpStatsNow.hasFeedback ? udpStatsNow.latestFeedback.decodeSkippedPackets : 0)
                << " bitrate_advice_mbps=" << Mbps(bitrateAdvisor.recommendedBitrate())
                << " bitrate_advice_action=" << bitrateAdvisor.action()
                << " bitrate_advice_reason=" << bitrateAdvisor.reason()
                << " bitrate_adaptation=" << bitrateAdaptationStatus
                << " bitrate_adaptations=" << bitrateAdaptations
                << " bitrate_adaptation_failures=" << bitrateAdaptationFailures
                << "\n";
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

    if (streamEncoder) {
        const auto drainedPackets = streamEncoder->Drain();
        streamPackets += drainedPackets.size();
        for (const auto& packet : drainedPackets) {
            streamBytes += packet.bytes.size();
            if (udpSender) {
                udpSender->SendFrame(packet);
            }
        }
    }

    if (udpSender) {
        udpSender->Flush();
        DrainUdpFeedback(*udpSender, std::chrono::milliseconds(100));
    }

    const size_t streamQueuedInputs = streamEncoder ? streamEncoder->queuedInputCount() : 0;
    const uint64_t streamDroppedInputFrames = streamEncoder ? streamEncoder->droppedInputFrames() : 0;
    const screenshare::UdpSenderStats udpStats = udpSender ? udpSender->stats() : screenshare::UdpSenderStats{};
    if (udpSender) {
        bitrateAdvisor.Update(udpStats);
    }
    if (!options.capturedBmpPath.empty() && hasFrame) {
        WriteCapturedFrameBmp(options.capturedBmpPath, lastFrame);
        capturedBmpWritten = true;
    }
    udpSender.reset();
    streamEncoder.reset();
    fileEncoder.reset();
    capturer.Stop();

    const double totalElapsed = std::chrono::duration<double>(captureFinishedAt - startedAt).count();
    std::cout
        << "Done. Average output FPS: " << (static_cast<double>(totalOutputFrames) / totalElapsed)
        << ", average desktop update FPS: " << (static_cast<double>(totalDesktopUpdates) / totalElapsed)
        << ", average capture ms: " << (totalCaptureCalls == 0 ? 0.0 : totalCaptureMs / static_cast<double>(totalCaptureCalls))
        << ", average stream encode ms: " << (totalStreamEncodeCalls == 0 ? 0.0 : totalStreamEncodeMs / static_cast<double>(totalStreamEncodeCalls))
        << ", repeated frames: " << totalRepeatedFrames
        << ", file encoded frames: " << fileEncodedFrames
        << ", stream encoded frames: " << streamEncodedFrames
        << ", stream bitrate Mbps: " << Mbps(streamBitrate)
        << ", stream packets: " << streamPackets
        << ", stream bytes: " << streamBytes
        << ", stream queued inputs: " << streamQueuedInputs
        << ", stream dropped inputs: " << streamDroppedInputFrames
        << ", UDP queued datagrams: " << udpStats.datagramsQueued
        << ", UDP datagrams: " << udpStats.datagramsSent
        << ", UDP pending datagrams: " << udpStats.pendingDatagrams
        << ", UDP peak pending datagrams: " << udpStats.peakPendingDatagrams
        << ", UDP dropped frames: " << udpStats.framesDropped
        << ", UDP dropped datagrams: " << udpStats.datagramsDropped
        << ", UDP wire bytes: " << udpStats.wireBytesSent
        << ", UDP feedback packets: " << udpStats.feedbackPacketsReceived
        << ", UDP invalid feedback packets: " << udpStats.invalidFeedbackPackets
        << ", UDP feedback health: "
        << (udpStats.hasFeedback ?
            screenshare::udp_protocol::FeedbackHealthStateName(udpStats.latestFeedback.healthState) :
            "none")
        << ", UDP feedback completed frames: " << (udpStats.hasFeedback ? udpStats.latestFeedback.completedFrames : 0)
        << ", UDP feedback resyncs: " << (udpStats.hasFeedback ? udpStats.latestFeedback.decodeResyncs : 0)
        << ", UDP feedback skipped packets: " << (udpStats.hasFeedback ? udpStats.latestFeedback.decodeSkippedPackets : 0)
        << ", bitrate advice Mbps: " << Mbps(bitrateAdvisor.recommendedBitrate())
        << ", bitrate advice action: " << bitrateAdvisor.action()
        << ", bitrate advice reason: " << bitrateAdvisor.reason()
        << ", bitrate adaptation: " << bitrateAdaptationStatus
        << ", bitrate adaptations: " << bitrateAdaptations
        << ", bitrate adaptation failures: " << bitrateAdaptationFailures
        << ", captured BMP written: " << (capturedBmpWritten ? "yes" : "no")
        << "\n";
}

void RunUdpReceiverStats(const Options& options)
{
    screenshare::UdpReceiver receiver;
    screenshare::UdpReceiverConfig config;
    config.port = options.udpReceivePort;
    config.simulatedLossPercent = options.simulateLossPercent;
    config.simulatedJitter = std::chrono::milliseconds(options.simulateJitterMs);
    receiver.Open(config);

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

    std::cout << "Listening for UDP H.264 packet fragments on port " << options.udpReceivePort;
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
    int h264DecodedWidth = 0;
    int h264DecodedHeight = 0;
    uint64_t nextH264DecodeFrameId = 0;
    bool hasH264DecodeStartFrame = false;
    std::map<uint64_t, screenshare::UdpCompletedFrame> h264DecodeBacklog;
    std::optional<screenshare::DecodedFrameInfo> latestDecodedFrame;

    if (options.decodeH264) {
        h264Decoder = std::make_unique<screenshare::H264StreamDecoder>();
        h264Decoder->Start();
    }
    if (options.previewWindow) {
        previewWindow = std::make_unique<screenshare::ReceiverPreviewWindow>();
        previewWindow->SetStatusText("waiting | fps 0.0 | recvq 0 | decq 0 | pvq 0 | resync 0 | skip 0 | drops 0/0 | shown 0");
        previewWindow->Show();
    }

    using Clock = std::chrono::steady_clock;
    PreviewPlayoutBuffer previewPlayout;

    auto countDecodedFrames = [&](std::vector<screenshare::DecodedFrameInfo> decodedFrames, bool presentReady = true) {
        h264DecodedFrames += decodedFrames.size();
        for (auto& decodedFrame : decodedFrames) {
            h264DecodedBytes += decodedFrame.bytes;
            h264DecodedWidth = decodedFrame.width;
            h264DecodedHeight = decodedFrame.height;
            latestDecodedFrame = decodedFrame;
            if (previewWindow) {
                previewPlayout.Enqueue(std::move(decodedFrame));
            }
        }

        if (previewWindow && presentReady) {
            previewPlayout.PresentReady(*previewWindow, Clock::now(), false);
        }
    };

    auto decodeH264Frame = [&](const screenshare::UdpCompletedFrame& frame) {
        screenshare::EncodedPacket packet;
        packet.timestamp100ns = static_cast<int64_t>(frame.timestamp100ns);
        packet.bytes = frame.bytes;
        countDecodedFrames(h264Decoder->DecodePacket(packet));
        ++h264DecodePackets;
    };

    auto restartH264DecoderForRecovery = [&]() {
        h264Decoder->Start();
        ++h264DecodeDecoderRestarts;
        if (previewWindow) {
            previewPlayout.ClearPendingAndRestartClock();
        }
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
        } else if (previewWindow) {
            previewPlayout.ClearPendingAndRestartClock();
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
    uint64_t latestFrameId = 0;
    uint64_t latestFrameBytes = 0;
    uint16_t latestFragmentCount = 0;
    uint64_t feedbackSequence = 0;
    bool hasCompletedFrame = false;

    auto shouldContinue = [&]() {
        if (previewWindow && !previewWindow->PumpMessages()) {
            return false;
        }
        if (options.previewWindow && options.seconds == 0) {
            return true;
        }
        return Clock::now() - startedAt < std::chrono::seconds(options.seconds);
    };

    while (shouldContinue()) {
        if (previewWindow) {
            previewPlayout.PresentReady(*previewWindow, Clock::now(), false);
        }

        const auto receiveTimeout = previewWindow ?
            previewPlayout.ReceiveTimeout(Clock::now()) :
            std::chrono::milliseconds(100);
        if (auto frame = receiver.ReceiveFrame(receiveTimeout)) {
            latestFrameId = frame->frameId;
            latestFrameBytes = frame->bytes.size();
            latestFragmentCount = frame->fragmentCount;
            hasCompletedFrame = true;

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

        if (previewWindow) {
            previewPlayout.PresentReady(*previewWindow, Clock::now(), false);
        }

        const auto now = Clock::now();
        if (now - lastReportAt >= std::chrono::seconds(1)) {
            const double elapsed = std::chrono::duration<double>(now - lastReportAt).count();
            const auto& stats = receiver.stats();
            const double datagramsPerSecond = static_cast<double>(stats.datagramsReceived - lastDatagramsReceived) / elapsed;
            const double completedFps = static_cast<double>(stats.framesCompleted - lastFramesCompleted) / elapsed;
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
                previewWindow ? previewPlayout.lateDrops() : 0,
                previewWindow ? previewPlayout.overflowDrops() : 0,
            };

            if (previewWindow) {
                previewWindow->SetStatusText(FormatReceiverHealthTitle(health));
            }
            receiver.SendFeedback(BuildReceiverFeedbackSnapshot(health, feedbackSequence++));

            std::cout
                << "udp_datagrams=" << stats.datagramsReceived
                << " receiver_health=" << ReceiverHealthState(health)
                << " udp_datagrams_per_second=" << datagramsPerSecond
                << " accepted_datagrams=" << stats.datagramsAccepted
                << " simulated_dropped=" << stats.simulatedDatagramsDropped
                << " simulated_delayed=" << stats.simulatedDatagramsDelayed
                << " simulated_delay_pending=" << receiver.delayedDatagramCount()
                << " feedback_sent=" << stats.feedbackPacketsSent
                << " feedback_errors=" << stats.feedbackSendErrors
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
                << " h264_decoded_frames=" << h264DecodedFrames
                << " h264_decoded_bytes=" << h264DecodedBytes
                << " h264_decode_resyncs=" << h264DecodeResyncs
                << " h264_decode_restarts=" << h264DecodeDecoderRestarts
                << " h264_decode_skipped_packets=" << h264DecodeSkippedPackets
                << " h264_decoded_output=" << h264DecodedWidth << "x" << h264DecodedHeight
                << " pending_h264_decode_packets=" << h264DecodeBacklog.size()
                << " preview_frames_presented=" << (previewWindow ? previewWindow->framesPresented() : 0)
                << " preview_queue=" << (previewWindow ? previewPlayout.queuedFrameCount() : 0)
                << " preview_late_drops=" << (previewWindow ? previewPlayout.lateDrops() : 0)
                << " preview_overflow_drops=" << (previewWindow ? previewPlayout.overflowDrops() : 0);

            if (hasCompletedFrame) {
                std::cout
                    << " latest_frame=" << latestFrameId
                    << " latest_frame_bytes=" << latestFrameBytes
                    << " latest_fragments=" << latestFragmentCount;
            }

            std::cout << "\n";

            lastDatagramsReceived = stats.datagramsReceived;
            lastFramesCompleted = stats.framesCompleted;
            lastReportAt = now;
        }
    }

    screenshare::UdpReceiverStats stats = receiver.stats();
    if (h264Decoder) {
        flushH264DecodeBacklog(true);
        countDecodedFrames(h264Decoder->Drain(), false);
        h264Decoder.reset();
    }
    if (shouldDumpH264) {
        flushH264DumpBacklog(true);
    }
    if (previewWindow) {
        previewPlayout.PresentReady(*previewWindow, Clock::now(), true);
    }

    bool decodedBmpWritten = false;
    if (!options.decodedBmpPath.empty() && latestDecodedFrame) {
        WriteDecodedFrameBmp(options.decodedBmpPath, *latestDecodedFrame);
        decodedBmpWritten = true;
    }
    const size_t delayedDatagrams = receiver.delayedDatagramCount();
    const double totalElapsed = std::chrono::duration<double>(Clock::now() - startedAt).count();
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
        previewWindow ? previewPlayout.lateDrops() : 0,
        previewWindow ? previewPlayout.overflowDrops() : 0,
    };
    if (previewWindow) {
        previewWindow->SetStatusText(FormatReceiverHealthTitle(finalHealth));
    }
    receiver.SendFeedback(BuildReceiverFeedbackSnapshot(finalHealth, feedbackSequence++));
    stats = receiver.stats();
    receiver.Close();

    std::cout
        << "Done. UDP datagrams: " << stats.datagramsReceived
        << ", receiver health: " << ReceiverHealthState(finalHealth)
        << ", accepted datagrams: " << stats.datagramsAccepted
        << ", simulated dropped datagrams: " << stats.simulatedDatagramsDropped
        << ", simulated delayed datagrams: " << stats.simulatedDatagramsDelayed
        << ", pending simulated delayed datagrams: " << delayedDatagrams
        << ", feedback packets sent: " << stats.feedbackPacketsSent
        << ", feedback send errors: " << stats.feedbackSendErrors
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
        << ", H.264 decoded frames: " << h264DecodedFrames
        << ", H.264 decoded bytes: " << h264DecodedBytes
        << ", H.264 decode resyncs: " << h264DecodeResyncs
        << ", H.264 decoder restarts: " << h264DecodeDecoderRestarts
        << ", H.264 decode skipped packets: " << h264DecodeSkippedPackets
        << ", H.264 decoded output: " << h264DecodedWidth << "x" << h264DecodedHeight
        << ", pending H.264 decode packets: " << h264DecodeBacklog.size()
        << ", preview frames presented: " << (previewWindow ? previewWindow->framesPresented() : 0)
        << ", preview queued frames: " << (previewWindow ? previewPlayout.queuedFrameCount() : 0)
        << ", preview late drops: " << (previewWindow ? previewPlayout.lateDrops() : 0)
        << ", preview overflow drops: " << (previewWindow ? previewPlayout.overflowDrops() : 0)
        << ", decoded BMP written: " << (decodedBmpWritten ? "yes" : "no")
        << "\n";
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = ParseOptions(argc, argv);

        if (options.listDisplays) {
            PrintDisplays();
            return 0;
        }

        if (options.listH264Encoders) {
            PrintH264Encoders(options);
            return 0;
        }

        if (options.udpReceivePort != 0) {
            RunUdpReceiverStats(options);
            return 0;
        }

        RunCaptureStats(options);
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "Error: " << error.what() << "\n\n";
        PrintHelp();
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
