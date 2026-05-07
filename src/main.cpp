#include "DesktopCapturer.h"
#include "H264FileEncoder.h"
#include "H264StreamDecoder.h"
#include "H264StreamEncoder.h"
#include "UdpReceiver.h"
#include "UdpSender.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

struct Options {
    bool listDisplays = false;
    int displayIndex = 0;
    int width = 0;
    int height = 0;
    int fps = 60;
    int seconds = 10;
    uint32_t bitrate = 0;
    std::string recordPath;
    bool streamEncode = false;
    std::string udpSendTarget;
    uint16_t udpReceivePort = 0;
    std::string h264DumpPath;
    bool decodeH264 = false;
};

void PrintHelp()
{
    std::cout
        << "ScreenShare native C++ capture prototype\n\n"
        << "Usage:\n"
        << "  ScreenShare --list\n"
        << "  ScreenShare --udp-recv PORT [--seconds S] [--dump-h264 PATH] [--decode-h264]\n"
        << "  ScreenShare [--display N] [--width W --height H] [--fps FPS] [--seconds S]\n"
        << "              [--record PATH] [--stream-encode] [--udp-send HOST:PORT]\n"
        << "              [--bitrate-mbps Mbps]\n\n"
        << "Examples:\n"
        << "  ScreenShare --list\n"
        << "  ScreenShare --display 0 --width 1920 --height 1080 --fps 60 --seconds 15\n"
        << "  ScreenShare --display 0 --fps 60 --seconds 15 --record native.mp4\n"
        << "  ScreenShare --udp-recv 5000 --seconds 15 --decode-h264 --dump-h264 receiver.h264\n"
        << "  ScreenShare --display 0 --width 1280 --height 720 --fps 60 --seconds 15 --udp-send 127.0.0.1:5000\n";
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
        } else if (arg == "--record") {
            options.recordPath = requireValue("--record");
        } else if (arg == "--stream-encode") {
            options.streamEncode = true;
        } else if (arg == "--udp-send") {
            options.udpSendTarget = requireValue("--udp-send");
            options.streamEncode = true;
        } else if (arg == "--udp-recv") {
            options.udpReceivePort = screenshare::ParseUdpReceivePort(requireValue("--udp-recv"));
        } else if (arg == "--dump-h264") {
            options.h264DumpPath = requireValue("--dump-h264");
        } else if (arg == "--decode-h264") {
            options.decodeH264 = true;
        } else if (arg == "--bitrate-mbps") {
            options.bitrate = ParseBitrateMbps(requireValue("--bitrate-mbps"));
        } else {
            throw std::invalid_argument("Unknown argument: " + arg);
        }
    }

    if (options.fps <= 0 || options.fps > 240) {
        throw std::invalid_argument("--fps must be between 1 and 240");
    }
    if (options.seconds <= 0) {
        throw std::invalid_argument("--seconds must be positive");
    }
    if ((options.width == 0) != (options.height == 0)) {
        throw std::invalid_argument("--width and --height must be provided together");
    }
    if (options.width < 0 || options.height < 0) {
        throw std::invalid_argument("--width and --height must be positive");
    }
    if (options.width > 0 && options.streamEncode && ((options.width % 2) != 0 || (options.height % 2) != 0)) {
        throw std::invalid_argument("--stream-encode requires even --width and --height for NV12");
    }
    if (!options.udpSendTarget.empty()) {
        static_cast<void>(screenshare::ParseUdpSenderTarget(options.udpSendTarget));
    }
    if (options.udpReceivePort != 0 &&
        (options.listDisplays || !options.recordPath.empty() || options.streamEncode || !options.udpSendTarget.empty())) {
        throw std::invalid_argument("--udp-recv cannot be combined with --list, --record, --stream-encode, or --udp-send");
    }
    if (!options.h264DumpPath.empty() && options.udpReceivePort == 0) {
        throw std::invalid_argument("--dump-h264 requires --udp-recv");
    }
    if (options.decodeH264 && options.udpReceivePort == 0) {
        throw std::invalid_argument("--decode-h264 requires --udp-recv");
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

void RunCaptureStats(const Options& options)
{
    screenshare::CaptureConfig config;
    config.displayIndex = options.displayIndex;
    config.targetWidth = options.width;
    config.targetHeight = options.height;
    config.targetFps = options.fps;

    screenshare::DesktopCapturer capturer;
    capturer.Start(config);

    std::cout << "Capturing display " << options.displayIndex
              << " at target " << options.fps << " FPS";

    if (options.width > 0 && options.height > 0) {
        std::cout << ", requested output " << options.width << "x" << options.height;
    } else {
        std::cout << ", requested output native resolution";
    }
    if (!options.recordPath.empty()) {
        std::cout << ", recording H.264 to " << options.recordPath;
    }
    if (options.streamEncode) {
        std::cout << ", stream-encoding H.264 packets";
    }
    if (!options.udpSendTarget.empty()) {
        std::cout << ", UDP sending to " << options.udpSendTarget;
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
    bool hasFrame = false;
    screenshare::CapturedFrame lastFrame;
    std::unique_ptr<screenshare::H264FileEncoder> fileEncoder;
    std::unique_ptr<screenshare::H264StreamEncoder> streamEncoder;
    uint64_t fileEncodedFrames = 0;
    uint64_t streamEncodedFrames = 0;
    uint64_t streamPackets = 0;
    uint64_t streamBytes = 0;
    std::unique_ptr<screenshare::UdpSender> udpSender;

    const auto targetFrameTime = std::chrono::microseconds(1'000'000 / options.fps);
    auto nextFrameAt = Clock::now();

    while (Clock::now() - startedAt < std::chrono::seconds(options.seconds)) {
        std::this_thread::sleep_until(nextFrameAt);
        nextFrameAt += targetFrameTime;

        const auto frame = capturer.TryCaptureFrame(std::chrono::milliseconds(0));
        if (frame) {
            ++totalDesktopUpdates;
            ++intervalDesktopUpdates;
            lastSourceWidth = frame->sourceWidth;
            lastSourceHeight = frame->sourceHeight;
            lastOutputWidth = frame->width;
            lastOutputHeight = frame->height;
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
                    screenshare::H264StreamEncoderConfig encoderConfig;
                    encoderConfig.width = lastFrame.width;
                    encoderConfig.height = lastFrame.height;
                    encoderConfig.fps = options.fps;
                    encoderConfig.bitrate = SelectBitrate(options, lastFrame.width, lastFrame.height);

                    streamEncoder = std::make_unique<screenshare::H264StreamEncoder>();
                    streamEncoder->Start(encoderConfig);

                    if (!options.udpSendTarget.empty()) {
                        udpSender = std::make_unique<screenshare::UdpSender>();
                        udpSender->Open(screenshare::ParseUdpSenderTarget(options.udpSendTarget));
                    }

                    std::cout
                        << "Stream encoder output=" << encoderConfig.width << "x" << encoderConfig.height
                        << " bitrate_mbps=" << Mbps(encoderConfig.bitrate)
                        << "\n";
                }

                const auto packets = streamEncoder->EncodeFrame(lastFrame);
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
            std::cout
                << "source=" << lastSourceWidth << "x" << lastSourceHeight
                << " output=" << lastOutputWidth << "x" << lastOutputHeight
                << " output_fps=" << outputFps
                << " desktop_update_fps=" << desktopUpdateFps
                << " repeated_frames=" << intervalRepeatedFrames
                << " total_output_frames=" << totalOutputFrames
                << " total_desktop_updates=" << totalDesktopUpdates
                << " file_encoded_frames=" << fileEncodedFrames
                << " stream_encoded_frames=" << streamEncodedFrames
                << " stream_packets=" << streamPackets
                << " stream_bytes=" << streamBytes
                << " udp_datagrams=" << (udpSender ? udpSender->stats().datagramsSent : 0)
                << " udp_wire_bytes=" << (udpSender ? udpSender->stats().wireBytesSent : 0)
                << "\n";
            intervalOutputFrames = 0;
            intervalDesktopUpdates = 0;
            intervalRepeatedFrames = 0;
            lastReportAt = now;
        }
    }

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

    const screenshare::UdpSenderStats udpStats = udpSender ? udpSender->stats() : screenshare::UdpSenderStats{};
    udpSender.reset();
    streamEncoder.reset();
    fileEncoder.reset();
    capturer.Stop();

    const double totalElapsed = std::chrono::duration<double>(Clock::now() - startedAt).count();
    std::cout
        << "Done. Average output FPS: " << (static_cast<double>(totalOutputFrames) / totalElapsed)
        << ", average desktop update FPS: " << (static_cast<double>(totalDesktopUpdates) / totalElapsed)
        << ", repeated frames: " << totalRepeatedFrames
        << ", file encoded frames: " << fileEncodedFrames
        << ", stream encoded frames: " << streamEncodedFrames
        << ", stream packets: " << streamPackets
        << ", stream bytes: " << streamBytes
        << ", UDP datagrams: " << udpStats.datagramsSent
        << ", UDP wire bytes: " << udpStats.wireBytesSent
        << "\n";
}

void RunUdpReceiverStats(const Options& options)
{
    screenshare::UdpReceiver receiver;
    screenshare::UdpReceiverConfig config;
    config.port = options.udpReceivePort;
    receiver.Open(config);

    std::ofstream h264Dump;
    uint64_t h264DumpPackets = 0;
    uint64_t h264DumpBytes = 0;
    uint64_t nextH264DumpFrameId = 0;
    bool hasH264DumpStartFrame = false;
    std::map<uint64_t, screenshare::UdpCompletedFrame> h264DumpBacklog;
    if (!options.h264DumpPath.empty()) {
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

    auto flushH264DumpBacklog = [&]() {
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
    uint64_t h264DecodePackets = 0;
    uint64_t h264DecodedFrames = 0;
    uint64_t h264DecodedBytes = 0;
    int h264DecodedWidth = 0;
    int h264DecodedHeight = 0;
    uint64_t nextH264DecodeFrameId = 0;
    bool hasH264DecodeStartFrame = false;
    std::map<uint64_t, screenshare::UdpCompletedFrame> h264DecodeBacklog;

    if (options.decodeH264) {
        h264Decoder = std::make_unique<screenshare::H264StreamDecoder>();
        h264Decoder->Start();
    }

    auto countDecodedFrames = [&](const std::vector<screenshare::DecodedFrameInfo>& decodedFrames) {
        h264DecodedFrames += decodedFrames.size();
        for (const auto& decodedFrame : decodedFrames) {
            h264DecodedBytes += decodedFrame.bytes;
            h264DecodedWidth = decodedFrame.width;
            h264DecodedHeight = decodedFrame.height;
        }
    };

    auto decodeH264Frame = [&](const screenshare::UdpCompletedFrame& frame) {
        screenshare::EncodedPacket packet;
        packet.timestamp100ns = static_cast<int64_t>(frame.timestamp100ns);
        packet.bytes = frame.bytes;
        countDecodedFrames(h264Decoder->DecodePacket(packet));
        ++h264DecodePackets;
    };

    auto flushH264DecodeBacklog = [&]() {
        while (true) {
            const auto next = h264DecodeBacklog.find(nextH264DecodeFrameId);
            if (next == h264DecodeBacklog.end()) {
                break;
            }

            decodeH264Frame(next->second);
            h264DecodeBacklog.erase(next);
            ++nextH264DecodeFrameId;
        }
    };

    using Clock = std::chrono::steady_clock;
    const auto startedAt = Clock::now();
    auto lastReportAt = startedAt;
    uint64_t lastDatagramsReceived = 0;
    uint64_t lastFramesCompleted = 0;
    uint64_t latestFrameId = 0;
    uint64_t latestFrameBytes = 0;
    uint16_t latestFragmentCount = 0;
    bool hasCompletedFrame = false;

    while (Clock::now() - startedAt < std::chrono::seconds(options.seconds)) {
        if (auto frame = receiver.ReceiveFrame(std::chrono::milliseconds(100))) {
            latestFrameId = frame->frameId;
            latestFrameBytes = frame->bytes.size();
            latestFragmentCount = frame->fragmentCount;
            hasCompletedFrame = true;

            if (h264Decoder) {
                if (!hasH264DecodeStartFrame) {
                    nextH264DecodeFrameId = frame->frameId;
                    hasH264DecodeStartFrame = true;
                }

                h264DecodeBacklog.emplace(frame->frameId, *frame);
                flushH264DecodeBacklog();
            }

            if (h264Dump) {
                if (!hasH264DumpStartFrame) {
                    nextH264DumpFrameId = frame->frameId;
                    hasH264DumpStartFrame = true;
                }

                h264DumpBacklog.emplace(frame->frameId, std::move(*frame));
                flushH264DumpBacklog();
            }
        }

        const auto now = Clock::now();
        if (now - lastReportAt >= std::chrono::seconds(1)) {
            const double elapsed = std::chrono::duration<double>(now - lastReportAt).count();
            const auto& stats = receiver.stats();
            const double datagramsPerSecond = static_cast<double>(stats.datagramsReceived - lastDatagramsReceived) / elapsed;
            const double completedFps = static_cast<double>(stats.framesCompleted - lastFramesCompleted) / elapsed;

            std::cout
                << "udp_datagrams=" << stats.datagramsReceived
                << " udp_datagrams_per_second=" << datagramsPerSecond
                << " accepted_datagrams=" << stats.datagramsAccepted
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
                << " h264_decoded_output=" << h264DecodedWidth << "x" << h264DecodedHeight
                << " pending_h264_decode_packets=" << h264DecodeBacklog.size();

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

    const screenshare::UdpReceiverStats stats = receiver.stats();
    if (h264Decoder) {
        countDecodedFrames(h264Decoder->Drain());
        h264Decoder.reset();
    }
    receiver.Close();

    const double totalElapsed = std::chrono::duration<double>(Clock::now() - startedAt).count();
    std::cout
        << "Done. UDP datagrams: " << stats.datagramsReceived
        << ", accepted datagrams: " << stats.datagramsAccepted
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
        << ", H.264 decoded output: " << h264DecodedWidth << "x" << h264DecodedHeight
        << ", pending H.264 decode packets: " << h264DecodeBacklog.size()
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
