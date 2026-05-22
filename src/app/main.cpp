#include "audio/OpusCodec.h"
#include "audio/WasapiCapture.h"
#include "audio/WasapiRenderer.h"
#include "capture/DesktopCapturer.h"
#include "codec/H264Bitstream.h"
#include "codec/H264EncoderProbe.h"
#include "codec/H264FileEncoder.h"
#include "codec/H264StreamDecoder.h"
#include "codec/H264StreamEncoder.h"
#include "render/ReceiverPreviewWindow.h"
#include "transport/LanDiscovery.h"
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
#include <cstring>
#include <cstdlib>
#include <ctime>
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
#include <random>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

enum class StreamEncoderPreference {
    Auto,
    Software,
    Hardware,
};

enum class InviteEndpointPreference {
    Auto,
    Public,
    Local,
};

enum class SignalingCommand {
    None,
    Health,
    Join,
    Peers,
    Heartbeat,
    Leave,
};

constexpr size_t OrderedReceiverStartThresholdFrames = 30;
constexpr size_t OrderedReceiverRecoveryThresholdFrames = 30;
constexpr size_t ReceiverHealthPendingFrameWarning = 8;
constexpr uint64_t SenderQueuePressureDatagrams = 2'048;
constexpr uint64_t SenderQueuePressureMs = 750;
constexpr uint64_t ResolutionSenderQueuePressureMs = 1'200;
constexpr uint32_t ResolutionBitrateReducedPercentBeforeQueueScale = 60;
constexpr uint32_t ResolutionStableReportsBeforeUpscale = 4;
constexpr uint32_t ResolutionPressureReportsBeforeReduce = 3;
constexpr int DefaultPreviewLatencyMs = 150;
constexpr int DefaultAvSyncPreviewLatencyMs = 100;
constexpr int DefaultPreviewMaxLateMs = 500;
constexpr int DefaultAudioPlaybackLatencyMs = 120;
constexpr int DefaultUdpMaxQueueMs = 0;
constexpr int DefaultShareUdpMaxQueueMs = 1500;
constexpr uint32_t DefaultUdpPacingHeadroomPercent = 125;
constexpr int MaxAvSyncCorrectionBiasMs = 250;
constexpr uint64_t AvSyncVideoOnlyFallbackFrames = 30;
constexpr uint64_t SenderDirectUdpBlockedDatagrams = 1024;
constexpr uint64_t ReceiverDirectUdpBlockedNatProbes = 120;
constexpr std::string_view DefaultSignalingServerUrl = "https://screenshare-signaling.bit-yeet.workers.dev";

struct UdpSendTargetSpec {
    std::string target;
    bool fromPeerInvite = false;
    std::string inviteEndpoint = "direct";
    uint16_t localPort = 0;
    bool localPortFromLocalInvite = false;
    bool collectNatProbeTargets = false;
    bool preferNatProbeTargets = false;
    uint64_t natProbeSessionFingerprint = 0;
};

struct ExtraShareTargetOption {
    std::string target;
    std::string localInvite;
};

struct Options {
    bool listDisplays = false;
    bool listH264Encoders = false;
    bool listAudioDevices = false;
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
    std::vector<std::string> udpSendTargets;
    std::vector<UdpSendTargetSpec> udpSendTargetSpecs;
    uint16_t udpLocalPort = 0;
    bool udpLocalPortProvided = false;
    bool udpPacing = true;
    bool udpPacingOptionProvided = false;
    bool udpSendTargetFromPeerInvite = false;
    std::string udpSendPeerInviteEndpoint = "direct";
    InviteEndpointPreference inviteEndpointPreference = InviteEndpointPreference::Auto;
    bool inviteEndpointPreferenceProvided = false;
    int udpMaxQueueMs = DefaultUdpMaxQueueMs;
    bool udpMaxQueueMsProvided = false;
    bool adaptBitrate = false;
    uint32_t adaptMinBitrate = 0;
    bool adaptMinBitrateProvided = false;
    int adaptReduceCooldownSeconds = 3;
    bool adaptReduceCooldownProvided = false;
    bool adaptResolution = false;
    float adaptResolutionMinScale = 0.5f;
    bool adaptResolutionMinScaleProvided = false;
    int adaptResolutionCooldownSeconds = 5;
    bool adaptResolutionCooldownProvided = false;
    bool wgcBorderRequired = false;
    bool hdrToSdr = true;
    float hdrSdrWhiteNits = 203.0f;
    float hdrSdrBgraExposure = 0.88f;
    uint16_t udpReceivePort = 0;
    std::string h264DumpPath;
    bool decodeH264 = false;
    std::string decodedBmpPath;
    bool previewWindow = false;
    int previewLatencyMs = DefaultPreviewLatencyMs;
    bool previewLatencyProvided = false;
    int previewMaxLateMs = DefaultPreviewMaxLateMs;
    bool previewMaxLateProvided = false;
    float simulateLossPercent = 0.0f;
    bool simulateLossProvided = false;
    int simulateJitterMs = 0;
    bool simulateJitterProvided = false;
    bool audioCapture = false;
    screenshare::AudioCaptureSource audioCaptureSource = screenshare::AudioCaptureSource::SystemOutput;
    std::string audioDeviceId;
    bool audioDeviceIdProvided = false;
    std::string audioSendTarget;
    screenshare::udp_protocol::AudioCodec audioCodec = screenshare::udp_protocol::AudioCodec::Opus;
    bool audioCodecProvided = false;
    bool audioPlayback = false;
    int audioPlaybackLatencyMs = DefaultAudioPlaybackLatencyMs;
    bool audioPlaybackLatencyProvided = false;
    bool audioPlaybackMuted = false;
    bool audioPlaybackMutedProvided = false;
    float audioPlaybackVolumePercent = 100.0f;
    bool audioPlaybackVolumeProvided = false;
    bool avSync = false;
    bool avSyncExplicit = false;
    bool avSyncDisabled = false;
    bool sharePreset = false;
    bool shareRoom = false;
    bool lanDiscover = false;
    int lanDiscoverSeconds = 2;
    bool lanAdvertise = false;
    uint16_t lanDiscoveryPort = screenshare::LanDiscoveryDefaultPort;
    std::string lanName;
    bool stunQuery = false;
    screenshare::StunServerTarget stunServer;
    int stunTimeoutMs = 3000;
    bool stunTimeoutProvided = false;
    bool makeInvite = false;
    uint16_t inviteLocalPort = 0;
    int inviteTtlSeconds = 300;
    bool inviteTtlProvided = false;
    bool natProbe = false;
    uint16_t natProbeLocalPort = 0;
    std::string peerInvite;
    std::string localInvite;
    int natProbeIntervalMs = 250;
    bool natProbeIntervalProvided = false;
    SignalingCommand signalingCommand = SignalingCommand::None;
    bool signalingLive = false;
    std::string signalingServerUrl;
    std::string signalingRoomId;
    std::string signalingPeerId;
    std::string signalingCandidate;
    std::string signalingName;
    std::string signalingPlatform;
    screenshare::StunServerTarget signalingStunServer{"stun.l.google.com", 19302};
    int signalingTimeoutMs = 5000;
    bool signalingTimeoutProvided = false;
    int signalingSetupSeconds = 5;
    bool signalingSetupSecondsProvided = false;
    std::vector<screenshare::UdpNatProbeTarget> signalingNatProbeTargets;
    bool signalingLocalCandidateAvailable = false;
    screenshare::SignalingCandidate signalingLocalCandidate;
    bool signalingHostCandidateAvailable = false;
    screenshare::SignalingCandidate signalingHostCandidate;
    std::string saveReportPath;
    std::string logPath;
    std::string sessionId;
    bool sessionIdProvided = false;
    bool sessionIdFromLocalInvite = false;
    std::string stopFilePath;
    bool generateAccessCode = false;
    bool allowPlaintext = false;
    bool accessCodeProvided = false;
    uint64_t sessionFingerprint = 0;
    uint64_t accessCodeFingerprint = 0;
    std::optional<screenshare::UdpCryptoKey> accessCodeKey;
    bool udpLocalPortFromLocalInvite = false;
};

struct SavedReportContext {
    std::string sessionId;
    uint64_t sessionFingerprint = 0;
    bool accessCodeRequired = false;
    bool encryptionEnabled = false;
    std::optional<screenshare::udp_protocol::FeedbackSnapshot> latestReceiverFeedback;
};

void PrintHelp()
{
    std::cout
        << "ScreenShare native C++ capture prototype\n\n"
        << "Usage:\n"
        << "  ScreenShare --list\n"
        << "  ScreenShare --generate-access-code\n"
        << "  ScreenShare --list-h264-encoders [--width W --height H] [--fps FPS] [--bitrate-mbps Mbps]\n"
        << "  ScreenShare --list-audio-devices\n"
        << "  ScreenShare --share HOST:PORT|INVITE [--display N] [--seconds S]\n"
        << "              [--share-target HOST:PORT|WATCHER_INVITE]\n"
        << "              [--invite-endpoint auto|public|local]\n"
        << "              [--local-invite INVITE]\n"
        << "  ScreenShare --share-room PORT --signal-room ROOM [--signal-server URL] [--seconds S]\n"
        << "  ScreenShare --watch PORT [--seconds S] [--peer-invite INVITE]\n"
        << "              [--signal-room ROOM] [--signal-server URL]\n"
        << "  ScreenShare --lan-discover [--lan-discover-seconds S]\n"
        << "  ScreenShare --stun HOST[:PORT] [--stun-timeout-ms MS]\n"
        << "  ScreenShare --make-invite PORT --stun HOST[:PORT] [--invite-ttl-seconds S]\n"
        << "  ScreenShare --nat-probe PORT --peer-invite INVITE [--seconds S]\n"
        << "              [--nat-probe-interval-ms MS]\n"
        << "  ScreenShare --signal-health URL [--signal-timeout-ms MS]\n"
        << "  ScreenShare --signal-join URL --signal-room ROOM --signal-peer-id PEER\n"
        << "              --signal-candidate IP:PORT [--signal-name NAME]\n"
        << "  ScreenShare --signal-peers URL --signal-room ROOM --signal-peer-id PEER\n"
        << "  ScreenShare --signal-heartbeat URL --signal-room ROOM --signal-peer-id PEER\n"
        << "  ScreenShare --signal-leave URL --signal-room ROOM --signal-peer-id PEER\n"
        << "  ScreenShare --audio-capture system|microphone [--seconds S] [--audio-device-id ID]\n"
        << "              [--audio-send HOST:PORT] [--udp-local-port PORT] [--audio-codec raw|opus]\n"
        << "  ScreenShare --udp-recv PORT [--seconds S] [--dump-h264 PATH] [--decode-h264]\n"
        << "              [--dump-decoded-bmp PATH] [--preview]\n"
        << "              [--preview-latency-ms MS] [--preview-max-late-ms MS]\n"
        << "              [--audio-playback] [--audio-playback-latency-ms MS]\n"
        << "              [--audio-playback-muted] [--audio-playback-volume PERCENT]\n"
        << "              [--av-sync|--no-av-sync]\n"
        << "              [--simulate-loss-percent P] [--simulate-jitter-ms MS]\n"
        << "              [--peer-invite INVITE] [--nat-probe-interval-ms MS]\n"
        << "  ScreenShare [--display N] [--width W --height H] [--fps FPS] [--seconds S]\n"
        << "              [--record PATH] [--stream-encode] [--stream-encoder auto|software|hardware]\n"
        << "              [--udp-send HOST:PORT] [--udp-local-port PORT]\n"
        << "              [--no-udp-pacing] [--udp-max-queue-ms MS]\n"
        << "              [--adapt-bitrate]\n"
        << "              [--audio-capture system|microphone] [--audio-device-id ID]\n"
        << "              [--audio-codec raw|opus]\n"
        << "              [--adapt-min-bitrate-mbps Mbps] [--adapt-reduce-cooldown S]\n"
        << "              [--adapt-resolution] [--adapt-resolution-min-scale N]\n"
        << "              [--adapt-resolution-cooldown S]\n"
        << "              [--dump-capture-bmp PATH]\n"
        << "              [--capture-backend dxgi|wgc]\n"
        << "              [--bitrate-mbps Mbps] [--keyframe-interval S]\n"
        << "              [--wgc-border] [--no-hdr-to-sdr]\n"
        << "              [--hdr-sdr-white-nits N] [--hdr-sdr-exposure N]\n"
        << "  Global: add --log PATH to save console output to a file.\n"
        << "          add --save-report PATH to save a zipped run report.\n"
        << "          add --session ID to give both sides a matching diagnostic session.\n"
        << "          add --access-code CODE on both sides to encrypt and gate local UDP sessions.\n"
        << "          run --generate-access-code to print a random access code.\n"
        << "          add --allow-plaintext to acknowledge an unencrypted local UDP session.\n"
        << "  LAN: add --lan-advertise to --watch/--udp-recv, then use --lan-discover on the sender.\n"
        << "       Optional: --lan-name NAME, --lan-discovery-port PORT, --lan-discover-seconds S.\n"
        << "  NAT: use --stun HOST[:PORT] to print this machine's public UDP endpoint.\n"
        << "       use --make-invite PORT --stun HOST[:PORT] to print a compact invite blob.\n"
        << "       The invite output includes Watch/Share command templates for the next step.\n"
        << "       --nat-probe is an optional diagnostic for checking whether peer probes can pass.\n"
        << "       Watch can also use --peer-invite to send punch probes while waiting for Share.\n"
        << "       Share can use --share \"ss1e:...\" to send to the peer invite endpoint.\n"
        << "       Add --local-invite \"ss1e:...\" on Share to bind the local port from this side's invite.\n"
        << "       Passing the same room invite as --share and --local-invite lets reachable watchers join by probing that invite.\n"
        << "       Add --share-target WATCHER_INVITE for blocked watchers that send back response invites.\n"
        << "       Use --invite-endpoint local to test same-LAN/VPN invite endpoints.\n"
        << "  Signaling: --signal-* commands test the HTTP room server without starting media.\n"
        << "             The Worker only exchanges UDP candidates; screen/audio still use direct UDP.\n"
        << "             Live room setup uses --signal-room ROOM with --share-room or --watch; --signal-server overrides the built-in Worker.\n"
        << "  Presets: --share enables UDP video, system audio, and adaptation; --watch enables preview and audio playback.\n\n"
        << "Examples:\n"
        << "  ScreenShare --list\n"
        << "  ScreenShare --generate-access-code\n"
        << "  ScreenShare --list-h264-encoders --width 1920 --height 1080 --fps 60\n"
        << "  ScreenShare --list-audio-devices\n"
        << "  ScreenShare --watch 5000 --save-report receiver-report.zip\n"
        << "  ScreenShare --watch 5000 --session game-night --save-report receiver-report.zip\n"
        << "  ScreenShare --watch 5000 --access-code 123456\n"
        << "  ScreenShare --watch 5000 --lan-advertise\n"
        << "  ScreenShare --lan-discover\n"
        << "  ScreenShare --stun stun.l.google.com:19302\n"
        << "  ScreenShare --make-invite 5000 --stun stun.l.google.com:19302 --access-code 123456\n"
        << "  ScreenShare --nat-probe 5000 --peer-invite \"ss1e:...\" --access-code 123456\n"
        << "  ScreenShare --signal-health https://example.workers.dev\n"
        << "  ScreenShare --signal-join https://example.workers.dev --signal-room room1 --signal-peer-id alice --signal-candidate 203.0.113.10:5000\n"
        << "  ScreenShare --watch 5000\n"
        << "  ScreenShare --watch 5000 --signal-room room1\n"
        << "  ScreenShare --share-room 5001 --signal-room room1\n"
        << "  ScreenShare --share 127.0.0.1:5000 --session game-night --save-report sender-report.zip\n"
        << "  ScreenShare --share 127.0.0.1:5000 --access-code 123456\n"
        << "  ScreenShare --audio-capture system --seconds 5\n"
        << "  ScreenShare --audio-capture system --seconds 5 --audio-send 127.0.0.1:5000\n"
        << "  ScreenShare --display 0 --width 1920 --height 1080 --fps 60 --seconds 15\n"
        << "  ScreenShare --display 0 --fps 60 --seconds 15 --record native.mp4\n"
        << "  ScreenShare --udp-recv 5000 --seconds 15 --dump-decoded-bmp receiver.bmp\n"
        << "  ScreenShare --udp-recv 5000 --audio-playback\n"
        << "  ScreenShare --watch 5000 --audio-playback-muted\n"
        << "  ScreenShare --udp-recv 5000 --preview\n"
        << "  ScreenShare --udp-recv 5000 --preview --audio-playback\n"
        << "  ScreenShare --display 0 --width 1280 --height 720 --fps 60 --seconds 15 --udp-send 127.0.0.1:5000 --audio-capture system\n"
        << "  ScreenShare --share 203.0.113.10:5000 --udp-local-port 5001 --access-code 123456\n"
        << "  ScreenShare --share \"ss1e:...\" --local-invite \"ss1e:...\" --access-code 123456\n"
        << "  ScreenShare --share \"ss1e:...\" --invite-endpoint local --access-code 123456\n"
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

InviteEndpointPreference ParseInviteEndpointPreference(const char* value)
{
    const std::string preference = value;
    if (preference == "auto") {
        return InviteEndpointPreference::Auto;
    }
    if (preference == "public") {
        return InviteEndpointPreference::Public;
    }
    if (preference == "local") {
        return InviteEndpointPreference::Local;
    }

    throw std::invalid_argument(std::string("Invalid value for --invite-endpoint: ") + value);
}

const char* InviteEndpointPreferenceName(InviteEndpointPreference preference)
{
    switch (preference) {
    case InviteEndpointPreference::Auto:
        return "auto";
    case InviteEndpointPreference::Public:
        return "public";
    case InviteEndpointPreference::Local:
        return "local";
    default:
        return "unknown";
    }
}

screenshare::udp_protocol::AudioCodec ParseAudioCodec(const std::string& value)
{
    if (value == "raw") {
        return screenshare::udp_protocol::AudioCodec::Raw;
    }
    if (value == "opus") {
        return screenshare::udp_protocol::AudioCodec::Opus;
    }
    throw std::invalid_argument("Invalid value for --audio-codec: " + value + " (expected raw or opus)");
}

std::string FormatSessionFingerprint(uint64_t fingerprint)
{
    std::ostringstream text;
    text << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << fingerprint;
    return text.str();
}

uint64_t SessionFingerprint(std::string_view sessionId)
{
    uint64_t hash = 14695981039346656037ULL;
    for (const char ch : sessionId) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1 : hash;
}

std::string GenerateSessionId()
{
    uint64_t randomPart = 0;
    try {
        std::random_device random;
        randomPart = (static_cast<uint64_t>(random()) << 32) ^ static_cast<uint64_t>(random());
    } catch (...) {
        randomPart = 0;
    }
    const uint64_t timePart = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const uint64_t fingerprint = (randomPart ^ timePart) == 0 ? 1 : (randomPart ^ timePart);
    return FormatSessionFingerprint(fingerprint);
}

std::string ParseSessionId(const char* value)
{
    const std::string sessionId = value != nullptr ? value : "";
    if (sessionId.empty() || sessionId.size() > 64) {
        throw std::invalid_argument("--session must be between 1 and 64 characters");
    }

    for (const char ch : sessionId) {
        const bool allowed =
            (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' ||
            ch == '_' ||
            ch == '.';
        if (!allowed) {
            throw std::invalid_argument("--session may only contain letters, digits, dot, dash, or underscore");
        }
    }

    return sessionId;
}

std::string ParseAccessCode(const char* value)
{
    const std::string accessCode = value != nullptr ? value : "";
    if (accessCode.empty() || accessCode.size() > 64) {
        throw std::invalid_argument("--access-code must be between 1 and 64 bytes");
    }

    for (const char ch : accessCode) {
        const unsigned char valueByte = static_cast<unsigned char>(ch);
        if (valueByte < 32U || valueByte == 127U) {
            throw std::invalid_argument("--access-code may not contain control characters");
        }
    }

    return accessCode;
}

std::string DefaultLanName()
{
    if (const char* computerName = std::getenv("COMPUTERNAME")) {
        if (computerName[0] != '\0') {
            return computerName;
        }
    }
    if (const char* userName = std::getenv("USERNAME")) {
        if (userName[0] != '\0') {
            return userName;
        }
    }
    return "ScreenShare";
}

std::string ParseLanName(const char* value)
{
    std::string name = value != nullptr ? value : "";
    if (name.empty() || name.size() > 64) {
        throw std::invalid_argument("--lan-name must be between 1 and 64 bytes");
    }

    for (const char ch : name) {
        if (static_cast<unsigned char>(ch) < 32U || ch == 127) {
            throw std::invalid_argument("--lan-name must not contain control characters");
        }
    }
    return name;
}

screenshare::udp_protocol::AudioSampleFormat ToUdpAudioSampleFormat(const screenshare::AudioCaptureFormat& format);
screenshare::UdpAudioPacket BuildRawUdpAudioPacket(
    const screenshare::CapturedAudioPacket& packet,
    const screenshare::AudioCaptureFormat& format,
    screenshare::udp_protocol::AudioSampleFormat sampleFormat);
screenshare::UdpAudioPacket BuildOpusUdpAudioPacket(
    const screenshare::CapturedAudioPacket& packet,
    screenshare::OpusAudioEncoder& encoder);

class TeeStreambuf : public std::streambuf {
public:
    TeeStreambuf(std::streambuf& first, std::streambuf& second, std::mutex& mutex)
        : first_(first),
          second_(second),
          mutex_(mutex)
    {
    }

private:
    int overflow(int ch) override
    {
        if (traits_type::eq_int_type(ch, traits_type::eof())) {
            return traits_type::not_eof(ch);
        }

        std::lock_guard lock(mutex_);
        const auto c = traits_type::to_char_type(ch);
        const bool firstOk = !traits_type::eq_int_type(first_.sputc(c), traits_type::eof());
        const bool secondOk = !traits_type::eq_int_type(second_.sputc(c), traits_type::eof());
        return firstOk && secondOk ? ch : traits_type::eof();
    }

    std::streamsize xsputn(const char* text, std::streamsize count) override
    {
        std::lock_guard lock(mutex_);
        const std::streamsize firstWritten = first_.sputn(text, count);
        const std::streamsize secondWritten = second_.sputn(text, count);
        return std::min(firstWritten, secondWritten);
    }

    int sync() override
    {
        std::lock_guard lock(mutex_);
        const int firstResult = first_.pubsync();
        const int secondResult = second_.pubsync();
        return firstResult == 0 && secondResult == 0 ? 0 : -1;
    }

    std::streambuf& first_;
    std::streambuf& second_;
    std::mutex& mutex_;
};

class ScopedLogRedirect {
public:
    explicit ScopedLogRedirect(const std::filesystem::path& path, bool announce = true)
    {
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }

        log_.open(path, std::ios::out | std::ios::trunc);
        if (!log_) {
            throw std::runtime_error("Failed to open log file: " + path.string());
        }

        coutTee_ = std::make_unique<TeeStreambuf>(*std::cout.rdbuf(), *log_.rdbuf(), mutex_);
        cerrTee_ = std::make_unique<TeeStreambuf>(*std::cerr.rdbuf(), *log_.rdbuf(), mutex_);
        oldCout_ = std::cout.rdbuf(coutTee_.get());
        oldCerr_ = std::cerr.rdbuf(cerrTee_.get());

        if (announce) {
            std::cout << "Logging console output to " << path.string() << "\n";
        }
    }

    ~ScopedLogRedirect()
    {
        std::cout.flush();
        std::cerr.flush();
        if (oldCout_ != nullptr) {
            std::cout.rdbuf(oldCout_);
        }
        if (oldCerr_ != nullptr) {
            std::cerr.rdbuf(oldCerr_);
        }
        log_.flush();
    }

    ScopedLogRedirect(const ScopedLogRedirect&) = delete;
    ScopedLogRedirect& operator=(const ScopedLogRedirect&) = delete;

private:
    std::ofstream log_;
    std::mutex mutex_;
    std::unique_ptr<TeeStreambuf> coutTee_;
    std::unique_ptr<TeeStreambuf> cerrTee_;
    std::streambuf* oldCout_ = nullptr;
    std::streambuf* oldCerr_ = nullptr;
};

uint32_t Crc32(std::span<const uint8_t> bytes)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (const uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path)
{
    const auto size = std::filesystem::file_size(path);
    if (size > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Report input is too large for the zip bundle: " + path.string());
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open report input: " + path.string());
    }
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) {
            throw std::runtime_error("Failed to read report input: " + path.string());
        }
    }
    return bytes;
}

void RemoveFileIfExists(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
}

std::vector<uint8_t> StringBytes(const std::string& text)
{
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::string ToZipPath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

class ZipWriter {
public:
    explicit ZipWriter(const std::filesystem::path& path)
    {
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }

        output_.open(path, std::ios::binary | std::ios::trunc);
        if (!output_) {
            throw std::runtime_error("Failed to create report bundle: " + path.string());
        }
    }

    void AddFile(const std::string& archiveName, std::span<const uint8_t> bytes)
    {
        if (archiveName.empty()) {
            throw std::runtime_error("Report bundle entry name is empty");
        }
        if (bytes.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("Report bundle entry is too large: " + archiveName);
        }

        Entry entry;
        entry.name = ToZipPath(archiveName);
        if (entry.name.size() > std::numeric_limits<uint16_t>::max()) {
            throw std::runtime_error("Report bundle entry name is too long: " + entry.name);
        }
        entry.crc = Crc32(bytes);
        entry.size = static_cast<uint32_t>(bytes.size());
        entry.localHeaderOffset = Tell();

        WriteU32(0x04034B50U);
        WriteU16(20);
        WriteU16(0);
        WriteU16(0);
        WriteU16(0);
        WriteU16(0);
        WriteU32(entry.crc);
        WriteU32(entry.size);
        WriteU32(entry.size);
        WriteU16(static_cast<uint16_t>(entry.name.size()));
        WriteU16(0);
        output_.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
        if (!bytes.empty()) {
            output_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        if (!output_) {
            throw std::runtime_error("Failed to write report bundle entry: " + entry.name);
        }

        entries_.push_back(std::move(entry));
    }

    void Finish()
    {
        if (finished_) {
            return;
        }
        if (entries_.size() > std::numeric_limits<uint16_t>::max()) {
            throw std::runtime_error("Report bundle has too many entries");
        }

        const uint32_t centralDirectoryOffset = Tell();
        for (const auto& entry : entries_) {
            WriteU32(0x02014B50U);
            WriteU16(20);
            WriteU16(20);
            WriteU16(0);
            WriteU16(0);
            WriteU16(0);
            WriteU16(0);
            WriteU32(entry.crc);
            WriteU32(entry.size);
            WriteU32(entry.size);
            WriteU16(static_cast<uint16_t>(entry.name.size()));
            WriteU16(0);
            WriteU16(0);
            WriteU16(0);
            WriteU16(0);
            WriteU32(0);
            WriteU32(entry.localHeaderOffset);
            output_.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
        }

        const uint32_t centralDirectorySize = Tell() - centralDirectoryOffset;
        WriteU32(0x06054B50U);
        WriteU16(0);
        WriteU16(0);
        WriteU16(static_cast<uint16_t>(entries_.size()));
        WriteU16(static_cast<uint16_t>(entries_.size()));
        WriteU32(centralDirectorySize);
        WriteU32(centralDirectoryOffset);
        WriteU16(0);
        if (!output_) {
            throw std::runtime_error("Failed to finish report bundle");
        }
        finished_ = true;
    }

    [[nodiscard]] size_t entryCount() const noexcept { return entries_.size(); }

private:
    struct Entry {
        std::string name;
        uint32_t crc = 0;
        uint32_t size = 0;
        uint32_t localHeaderOffset = 0;
    };

    uint32_t Tell()
    {
        const auto position = output_.tellp();
        if (position < 0 || static_cast<uint64_t>(position) > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("Report bundle exceeded zip32 size limits");
        }
        return static_cast<uint32_t>(position);
    }

    void WriteU16(uint16_t value)
    {
        const char bytes[2] = {
            static_cast<char>(value & 0xFF),
            static_cast<char>((value >> 8) & 0xFF),
        };
        output_.write(bytes, sizeof(bytes));
    }

    void WriteU32(uint32_t value)
    {
        const char bytes[4] = {
            static_cast<char>(value & 0xFF),
            static_cast<char>((value >> 8) & 0xFF),
            static_cast<char>((value >> 16) & 0xFF),
            static_cast<char>((value >> 24) & 0xFF),
        };
        output_.write(bytes, sizeof(bytes));
    }

    std::ofstream output_;
    std::vector<Entry> entries_;
    bool finished_ = false;
};

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

        frames_.emplace(key, std::move(frame));
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

            auto frame = std::move(frames_.begin()->second);
            frames_.erase(frames_.begin());
            previewWindow.PresentFrame(frame);
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

    std::map<int64_t, screenshare::DecodedFrameInfo> frames_;
    Clock::time_point firstPresentationAt_{};
    int64_t firstTimestamp100ns_ = 0;
    int64_t lastPresentedTimestamp100ns_ = 0;
    bool clockStarted_ = false;
    bool hasPresentedTimestamp_ = false;
    uint64_t lateDrops_ = 0;
    uint64_t overflowDrops_ = 0;
    uint64_t syncDrops_ = 0;
    uint64_t syncWaits_ = 0;
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
        screenshare::udp_protocol::AudioCodec codec)
    {
        Stop();
        stopRequested_ = false;
        {
            std::lock_guard lock(mutex_);
            stats_ = {};
            stats_.codec = codec;
            error_.clear();
        }

        thread_ = std::thread([this, sendAudioPacket = std::move(sendAudioPacket), source, deviceId, codec] {
            Run(sendAudioPacket, source, deviceId, codec);
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
        screenshare::udp_protocol::AudioCodec codec)
    {
        try {
            screenshare::WasapiCapture capture;
            screenshare::AudioCaptureConfig config;
            config.source = source;
            if (!deviceId.empty()) {
                config.deviceId = screenshare::Widen(deviceId);
            }
            capture.Start(config);

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

bool HasNatShareTarget(const Options& options);

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

class UdpSenderFanout {
public:
    struct Target {
        std::unique_ptr<screenshare::UdpSender> sender;
        bool failed = false;
        std::string error;
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
            targets_.push_back(Target{std::move(sender)});
        }
    }

    void Close()
    {
        targets_.clear();
    }

    bool AddAdditionalTarget(const screenshare::UdpSenderEndpoint& endpoint)
    {
        for (auto& target : targets_) {
            if (target.failed || target.sender == nullptr || !target.sender->isOpen()) {
                continue;
            }
            return target.sender->AddAdditionalTarget(endpoint);
        }
        throw std::runtime_error("No active UDP sender target is available for live signaling");
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
        }
        return aggregate;
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

std::string SignalingCandidateEndpoint(const screenshare::SignalingCandidate& candidate)
{
    return candidate.ip + ":" + std::to_string(candidate.port);
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

screenshare::SignalingPeerState BuildLiveSignalingPeerState(const Options& options)
{
    if (!options.signalingLocalCandidateAvailable) {
        throw std::logic_error("Live signaling local candidate is not available");
    }

    screenshare::SignalingPeerState peer;
    peer.peerId = options.signalingPeerId;
    peer.metadata.name = options.signalingName.empty() ? options.lanName : options.signalingName;
    peer.metadata.platform = options.signalingPlatform.empty() ? "windows" : options.signalingPlatform;
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
            << " interval_ms=" << PollInterval.count()
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

private:
    static constexpr std::chrono::milliseconds PollInterval{2000};
    static constexpr std::chrono::milliseconds BackgroundRequestTimeout{2000};

    struct State {
        mutable std::mutex mutex;
        std::condition_variable wake;
        std::string roomId;
        screenshare::SignalingPeerState peer;
        std::set<std::string> seenCandidates;
        std::vector<LiveSignalingPeerCandidate> discoveredPeers;
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
        for (const auto& peer : response.peers) {
            if (peer.peerId == state->peer.peerId) {
                continue;
            }
            for (const auto& candidate : peer.candidates) {
                const std::string key = CandidateKey(peer.peerId, candidate);
                if (!state->seenCandidates.insert(key).second) {
                    continue;
                }
                state->discoveredPeers.push_back(LiveSignalingPeerCandidate{peer.peerId, candidate});
            }
        }
    }

    [[nodiscard]] static bool WaitForNextPoll(const std::shared_ptr<State>& state)
    {
        std::unique_lock lock(state->mutex);
        return state->wake.wait_for(lock, PollInterval, [&]() {
            return state->stopRequested;
        });
    }

    [[nodiscard]] static bool stopRequested(const std::shared_ptr<State>& state)
    {
        std::lock_guard lock(state->mutex);
        return state->stopRequested;
    }

    static void Run(std::shared_ptr<State> state, screenshare::SignalingClientConfig clientConfig)
    {
        screenshare::SignalingClient client(std::move(clientConfig));

        while (!stopRequested(state)) {
            try {
                RecordPeers(state, client.Join(state->roomId, state->peer));
            } catch (const std::exception& error) {
                std::cerr
                    << "signaling_live_refresh=error"
                    << " room=" << state->roomId
                    << " peer_id=" << state->peer.peerId
                    << " error=\"" << error.what() << "\""
                    << "\n";
            }

            if (WaitForNextPoll(state)) {
                break;
            }
        }

        try {
            client.Leave(state->roomId, state->peer.peerId);
            std::cout
                << "signaling_live_leave=ok"
                << " room=" << state->roomId
                << " peer_id=" << state->peer.peerId
                << "\n" << std::flush;
        } catch (const std::exception& error) {
            std::cerr
                << "signaling_live_leave=error"
                << " room=" << state->roomId
                << " peer_id=" << state->peer.peerId
                << " error=\"" << error.what() << "\""
                << "\n";
        }
    }

    std::thread worker_;
    std::shared_ptr<State> state_;
};

UdpSendTargetSpec SignalingSendTargetSpec(
    const screenshare::SignalingCandidate& candidate,
    uint16_t localPort,
    uint64_t probeSession)
{
    return UdpSendTargetSpec{
        SignalingCandidateEndpoint(candidate),
        true,
        "signaling",
        localPort,
        false,
        true,
        true,
        probeSession};
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

std::optional<std::filesystem::path> FindPathArgument(int argc, char** argv, const char* optionName)
{
    std::optional<std::filesystem::path> path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == optionName && i + 1 < argc) {
            path = argv[++i];
        }
    }
    return path;
}

bool StopRequested(const Options& options)
{
    if (options.stopFilePath.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::exists(options.stopFilePath, error);
}

bool HasUdpSession(const Options& options)
{
    return options.shareRoom ||
           options.udpReceivePort != 0 ||
           !options.udpSendTarget.empty() ||
           !options.audioSendTarget.empty();
}

bool HasSignalingCommand(const Options& options)
{
    return options.signalingCommand != SignalingCommand::None;
}

bool HasSignalingOptions(const Options& options)
{
    return HasSignalingCommand(options) ||
           options.signalingLive ||
           !options.signalingRoomId.empty() ||
           !options.signalingPeerId.empty() ||
           !options.signalingCandidate.empty() ||
           !options.signalingName.empty() ||
           !options.signalingPlatform.empty() ||
           options.signalingTimeoutProvided ||
           options.signalingSetupSecondsProvided;
}

bool HasNatShareTarget(const Options& options)
{
    if (options.signalingLive && options.shareRoom) {
        return true;
    }
    if (options.udpSendTargetFromPeerInvite) {
        return true;
    }
    return std::any_of(
        options.udpSendTargetSpecs.begin(),
        options.udpSendTargetSpecs.end(),
        [](const UdpSendTargetSpec& target) {
            return target.fromPeerInvite;
        });
}

bool LooksLikeNatInvite(std::string_view text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return false;
    }
    text.remove_prefix(first);
    return text.rfind("nat_invite=", 0) == 0 ||
           text.rfind("screenshare-invite-v1", 0) == 0 ||
           text.rfind("ss1p:", 0) == 0 ||
           text.rfind("ss1e:", 0) == 0;
}

std::string FormatNatEndpoint(const screenshare::NatInviteEndpoint& endpoint);
screenshare::NatInvite ParseValidatedPeerInvite(const Options& options);

uint64_t NatProbeSessionFingerprint(const Options& options)
{
    if (options.signalingLive) {
        return SessionFingerprint(options.signalingRoomId);
    }
    return (options.sessionIdProvided || options.sessionIdFromLocalInvite) ? options.sessionFingerprint : 0;
}

struct SelectedNatInviteEndpoint {
    screenshare::NatInviteEndpoint endpoint;
    const char* name = "public";
};

bool HasNatEndpoint(const screenshare::NatInviteEndpoint& endpoint)
{
    return !endpoint.host.empty() && endpoint.port != 0;
}

SelectedNatInviteEndpoint SelectNatInviteEndpoint(
    const screenshare::NatInvite& invite,
    InviteEndpointPreference preference)
{
    if (preference == InviteEndpointPreference::Local) {
        if (!HasNatEndpoint(invite.localEndpoint)) {
            throw std::invalid_argument("Peer invite does not include a local endpoint");
        }
        return {invite.localEndpoint, "local"};
    }

    if (!HasNatEndpoint(invite.publicEndpoint)) {
        throw std::invalid_argument("Peer invite does not include a public endpoint");
    }
    return {invite.publicEndpoint, preference == InviteEndpointPreference::Auto ? "auto-public" : "public"};
}

screenshare::NatInvite ParseValidatedLocalInviteForShare(
    const Options& options,
    const std::string& inviteText,
    const char* optionName)
{
    const auto invite = screenshare::ParseNatInvite(inviteText, options.accessCodeKey);
    const std::string label = optionName != nullptr ? optionName : "Local invite";
    if (!HasNatEndpoint(invite.localEndpoint)) {
        throw std::invalid_argument(label + " does not include a local endpoint");
    }
    if (invite.encrypted) {
        if (!options.accessCodeProvided) {
            throw std::invalid_argument(label + " requires --access-code CODE");
        }
        if (options.accessCodeFingerprint != invite.accessCodeFingerprint) {
            throw std::invalid_argument("Access code does not match the " + label + " fingerprint");
        }
    } else if (!options.allowPlaintext) {
        throw std::invalid_argument(label + " is plaintext; rerun with --allow-plaintext");
    }
    return invite;
}

void WarnIfPlaintextUdpSession(const Options& options)
{
    if (HasUdpSession(options) && !options.accessCodeProvided && !options.allowPlaintext) {
        std::cout
            << "Warning: UDP session is plaintext. Add --access-code CODE for encryption, "
            << "or --allow-plaintext to acknowledge plaintext mode.\n";
    }
}

Options ParseOptions(int argc, char** argv, std::string defaultSessionId)
{
    Options options;
    options.sessionId = std::move(defaultSessionId);
    bool secondsProvided = false;
    std::optional<std::string> shareTarget;
    std::optional<uint16_t> shareRoomPort;
    std::vector<ExtraShareTargetOption> extraShareTargets;
    bool sessionFromLocalInvite = false;
    std::optional<uint16_t> watchPort;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        auto requireValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                throw std::invalid_argument(std::string("Missing value for ") + name);
            }
            return argv[++i];
        };

        auto setSignalingCommand = [&](SignalingCommand command, const char* name) {
            if (options.signalingCommand != SignalingCommand::None) {
                throw std::invalid_argument("Only one --signal-* command can be used at a time");
            }
            options.signalingCommand = command;
            options.signalingServerUrl = requireValue(name);
        };

        constexpr std::string_view accessCodePrefix = "--access-code=";
        constexpr std::string_view sessionCodePrefix = "--session-code=";

        if (arg == "--help" || arg == "-h") {
            PrintHelp();
            std::exit(0);
        }
        if (arg == "--log") {
            options.logPath = requireValue("--log");
        } else if (arg == "--stop-file") {
            options.stopFilePath = requireValue("--stop-file");
        } else if (arg == "--session" || arg == "--session-id") {
            options.sessionId = ParseSessionId(requireValue(arg.c_str()));
            options.sessionIdProvided = true;
        } else if (arg == "--generate-access-code") {
            options.generateAccessCode = true;
        } else if (arg == "--allow-plaintext") {
            options.allowPlaintext = true;
        } else if (arg == "--access-code" || arg == "--session-code") {
            const std::string accessCode = ParseAccessCode(requireValue(arg.c_str()));
            options.accessCodeFingerprint = screenshare::UdpAccessCodeFingerprint(accessCode);
            options.accessCodeKey = screenshare::DeriveUdpCryptoKey(accessCode);
            options.accessCodeProvided = true;
        } else if (arg.rfind(accessCodePrefix, 0) == 0 || arg.rfind(sessionCodePrefix, 0) == 0) {
            const size_t valueOffset =
                arg.rfind(accessCodePrefix, 0) == 0 ? accessCodePrefix.size() : sessionCodePrefix.size();
            const std::string accessCode = ParseAccessCode(arg.c_str() + valueOffset);
            options.accessCodeFingerprint = screenshare::UdpAccessCodeFingerprint(accessCode);
            options.accessCodeKey = screenshare::DeriveUdpCryptoKey(accessCode);
            options.accessCodeProvided = true;
        } else if (arg == "--list") {
            options.listDisplays = true;
        } else if (arg == "--list-h264-encoders") {
            options.listH264Encoders = true;
        } else if (arg == "--list-audio-devices") {
            options.listAudioDevices = true;
        } else if (arg == "--save-report") {
            options.saveReportPath = requireValue("--save-report");
        } else if (arg == "--lan-discover") {
            options.lanDiscover = true;
        } else if (arg == "--lan-discover-seconds") {
            options.lanDiscoverSeconds = ParseInt(requireValue("--lan-discover-seconds"), "--lan-discover-seconds");
            options.lanDiscover = true;
        } else if (arg == "--lan-advertise") {
            options.lanAdvertise = true;
        } else if (arg == "--lan-discovery-port") {
            options.lanDiscoveryPort = screenshare::ParseUdpReceivePort(requireValue("--lan-discovery-port"));
        } else if (arg == "--lan-name") {
            options.lanName = ParseLanName(requireValue("--lan-name"));
        } else if (arg == "--stun") {
            options.stunServer = screenshare::ParseStunServerTarget(requireValue("--stun"));
            options.stunQuery = true;
        } else if (arg == "--stun-timeout-ms") {
            options.stunTimeoutMs = ParseInt(requireValue("--stun-timeout-ms"), "--stun-timeout-ms");
            options.stunTimeoutProvided = true;
        } else if (arg == "--make-invite") {
            options.inviteLocalPort = screenshare::ParseUdpReceivePort(requireValue("--make-invite"));
            options.makeInvite = true;
        } else if (arg == "--invite-ttl-seconds") {
            options.inviteTtlSeconds = ParseInt(requireValue("--invite-ttl-seconds"), "--invite-ttl-seconds");
            options.inviteTtlProvided = true;
        } else if (arg == "--nat-probe") {
            options.natProbeLocalPort = screenshare::ParseUdpReceivePort(requireValue("--nat-probe"));
            options.natProbe = true;
        } else if (arg == "--peer-invite") {
            options.peerInvite = requireValue("--peer-invite");
        } else if (arg == "--local-invite") {
            options.localInvite = requireValue("--local-invite");
        } else if (arg == "--nat-probe-interval-ms") {
            options.natProbeIntervalMs = ParseInt(requireValue("--nat-probe-interval-ms"), "--nat-probe-interval-ms");
            options.natProbeIntervalProvided = true;
        } else if (arg == "--signal-server") {
            options.signalingServerUrl = requireValue("--signal-server");
            options.signalingLive = true;
        } else if (arg == "--signal-health") {
            setSignalingCommand(SignalingCommand::Health, "--signal-health");
        } else if (arg == "--signal-join") {
            setSignalingCommand(SignalingCommand::Join, "--signal-join");
        } else if (arg == "--signal-peers") {
            setSignalingCommand(SignalingCommand::Peers, "--signal-peers");
        } else if (arg == "--signal-heartbeat") {
            setSignalingCommand(SignalingCommand::Heartbeat, "--signal-heartbeat");
        } else if (arg == "--signal-leave") {
            setSignalingCommand(SignalingCommand::Leave, "--signal-leave");
        } else if (arg == "--signal-room") {
            options.signalingRoomId = requireValue("--signal-room");
        } else if (arg == "--signal-peer-id") {
            options.signalingPeerId = requireValue("--signal-peer-id");
        } else if (arg == "--signal-candidate") {
            options.signalingCandidate = requireValue("--signal-candidate");
        } else if (arg == "--signal-name") {
            options.signalingName = requireValue("--signal-name");
        } else if (arg == "--signal-platform") {
            options.signalingPlatform = requireValue("--signal-platform");
        } else if (arg == "--signal-stun") {
            options.signalingStunServer = screenshare::ParseStunServerTarget(requireValue("--signal-stun"));
            options.signalingLive = true;
        } else if (arg == "--signal-timeout-ms") {
            options.signalingTimeoutMs = ParseInt(requireValue("--signal-timeout-ms"), "--signal-timeout-ms");
            options.signalingTimeoutProvided = true;
        } else if (arg == "--signal-setup-seconds") {
            options.signalingSetupSeconds = ParseInt(requireValue("--signal-setup-seconds"), "--signal-setup-seconds");
            options.signalingSetupSecondsProvided = true;
        } else if (arg == "--invite-endpoint") {
            options.inviteEndpointPreference = ParseInviteEndpointPreference(requireValue("--invite-endpoint"));
            options.inviteEndpointPreferenceProvided = true;
        } else if (arg == "--share") {
            shareTarget = requireValue("--share");
        } else if (arg == "--share-room") {
            shareRoomPort = screenshare::ParseUdpReceivePort(requireValue("--share-room"));
        } else if (arg == "--share-target" || arg == "--viewer") {
            extraShareTargets.push_back(ExtraShareTargetOption{requireValue(arg.c_str()), {}});
        } else if (arg == "--share-target-local-invite" || arg == "--viewer-local-invite") {
            if (extraShareTargets.empty()) {
                throw std::invalid_argument(std::string(arg) + " must follow --share-target INVITE");
            }
            if (!extraShareTargets.back().localInvite.empty()) {
                throw std::invalid_argument("Each --share-target accepts only one --share-target-local-invite");
            }
            extraShareTargets.back().localInvite = requireValue(arg.c_str());
        } else if (arg == "--watch") {
            watchPort = screenshare::ParseUdpReceivePort(requireValue("--watch"));
        } else if (arg == "--audio-capture") {
            options.audioCaptureSource = screenshare::ParseAudioCaptureSource(requireValue("--audio-capture"));
            options.audioCapture = true;
        } else if (arg == "--audio-device-id") {
            options.audioDeviceId = requireValue("--audio-device-id");
            options.audioDeviceIdProvided = true;
        } else if (arg == "--audio-send") {
            options.audioSendTarget = requireValue("--audio-send");
        } else if (arg == "--audio-codec") {
            options.audioCodec = ParseAudioCodec(requireValue("--audio-codec"));
            options.audioCodecProvided = true;
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
            const std::string target = requireValue("--udp-send");
            if (options.udpSendTarget.empty()) {
                options.udpSendTarget = target;
            }
            options.udpSendTargets.push_back(target);
            options.streamEncode = true;
        } else if (arg == "--udp-local-port") {
            options.udpLocalPort = screenshare::ParseUdpReceivePort(requireValue("--udp-local-port"));
            options.udpLocalPortProvided = true;
        } else if (arg == "--no-udp-pacing") {
            options.udpPacing = false;
            options.udpPacingOptionProvided = true;
        } else if (arg == "--udp-max-queue-ms") {
            options.udpMaxQueueMs = ParseInt(requireValue("--udp-max-queue-ms"), "--udp-max-queue-ms");
            options.udpMaxQueueMsProvided = true;
        } else if (arg == "--adapt-bitrate") {
            options.adaptBitrate = true;
        } else if (arg == "--adapt-min-bitrate-mbps") {
            options.adaptMinBitrate = ParseBitrateMbps(requireValue("--adapt-min-bitrate-mbps"));
            options.adaptMinBitrateProvided = true;
        } else if (arg == "--adapt-reduce-cooldown") {
            options.adaptReduceCooldownSeconds = ParseInt(requireValue("--adapt-reduce-cooldown"), "--adapt-reduce-cooldown");
            options.adaptReduceCooldownProvided = true;
        } else if (arg == "--adapt-resolution") {
            options.adaptResolution = true;
            options.adaptBitrate = true;
        } else if (arg == "--adapt-resolution-min-scale") {
            options.adaptResolutionMinScale = ParseFloat(requireValue("--adapt-resolution-min-scale"), "--adapt-resolution-min-scale");
            options.adaptResolutionMinScaleProvided = true;
        } else if (arg == "--adapt-resolution-cooldown") {
            options.adaptResolutionCooldownSeconds = ParseInt(requireValue("--adapt-resolution-cooldown"), "--adapt-resolution-cooldown");
            options.adaptResolutionCooldownProvided = true;
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
        } else if (arg == "--preview-latency-ms") {
            options.previewLatencyMs = ParseInt(requireValue("--preview-latency-ms"), "--preview-latency-ms");
            options.previewLatencyProvided = true;
        } else if (arg == "--preview-max-late-ms") {
            options.previewMaxLateMs = ParseInt(requireValue("--preview-max-late-ms"), "--preview-max-late-ms");
            options.previewMaxLateProvided = true;
        } else if (arg == "--audio-playback") {
            options.audioPlayback = true;
        } else if (arg == "--audio-playback-latency-ms") {
            options.audioPlaybackLatencyMs = ParseInt(requireValue("--audio-playback-latency-ms"), "--audio-playback-latency-ms");
            options.audioPlaybackLatencyProvided = true;
        } else if (arg == "--audio-playback-muted") {
            options.audioPlaybackMuted = true;
            options.audioPlaybackMutedProvided = true;
        } else if (arg == "--audio-playback-volume") {
            options.audioPlaybackVolumePercent = ParseFloat(requireValue("--audio-playback-volume"), "--audio-playback-volume");
            options.audioPlaybackVolumeProvided = true;
        } else if (arg == "--av-sync") {
            options.avSync = true;
            options.avSyncExplicit = true;
        } else if (arg == "--no-av-sync") {
            options.avSyncDisabled = true;
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

    const auto applyLocalInviteSession = [&](const screenshare::NatInvite& invite, const char* optionName) {
        if (invite.sessionId.empty()) {
            return;
        }
        if (options.sessionIdProvided) {
            if (options.sessionId != invite.sessionId) {
                throw std::invalid_argument(
                    std::string(optionName) + " session does not match the current --session");
            }
            return;
        }
        if (sessionFromLocalInvite) {
            return;
        }
        options.sessionId = invite.sessionId;
        options.sessionIdFromLocalInvite = true;
        sessionFromLocalInvite = true;
    };

    const auto makeShareTargetSpec = [&](const ExtraShareTargetOption& targetOption, bool primary) {
        UdpSendTargetSpec spec;
        if (LooksLikeNatInvite(targetOption.target)) {
            const auto invite = screenshare::ParseNatInvite(targetOption.target, options.accessCodeKey);
            const auto selectedEndpoint = SelectNatInviteEndpoint(invite, options.inviteEndpointPreference);
            spec.target = FormatNatEndpoint(selectedEndpoint.endpoint);
            spec.fromPeerInvite = true;
            spec.inviteEndpoint = selectedEndpoint.name;

            if (!targetOption.localInvite.empty()) {
                const auto localInvite = ParseValidatedLocalInviteForShare(
                    options,
                    targetOption.localInvite,
                    primary ? "--local-invite" : "--share-target-local-invite");
                if (primary && options.udpLocalPortProvided && options.udpLocalPort != localInvite.localEndpoint.port) {
                    throw std::invalid_argument("--udp-local-port does not match the local invite port");
                }
                spec.localPort = localInvite.localEndpoint.port;
                spec.localPortFromLocalInvite = true;
                spec.collectNatProbeTargets = true;
                spec.preferNatProbeTargets = invite.sessionFingerprint == localInvite.sessionFingerprint;
                spec.natProbeSessionFingerprint = localInvite.sessionFingerprint;
                applyLocalInviteSession(
                    localInvite,
                    primary ? "--local-invite" : "--share-target-local-invite");
            }
        } else {
            if (!targetOption.localInvite.empty()) {
                throw std::invalid_argument("--share-target-local-invite can only be used with --share-target INVITE");
            }
            static_cast<void>(screenshare::ParseUdpSenderTarget(targetOption.target));
            spec.target = targetOption.target;
        }
        return spec;
    };

    if (shareTarget && shareRoomPort) {
        throw std::invalid_argument("--share cannot be combined with --share-room");
    }
    if ((shareTarget || shareRoomPort) && watchPort) {
        throw std::invalid_argument("--share/--share-room cannot be combined with --watch");
    }
    if (!extraShareTargets.empty() && !shareTarget && !shareRoomPort) {
        throw std::invalid_argument("--share-target requires --share or --share-room");
    }
    if (shareTarget) {
        if (!options.udpSendTarget.empty()) {
            throw std::invalid_argument("--share cannot be combined with --udp-send");
        }
        if (options.udpReceivePort != 0) {
            throw std::invalid_argument("--share cannot be combined with --udp-recv");
        }
        if (!options.audioSendTarget.empty()) {
            throw std::invalid_argument("--share cannot be combined with --audio-send");
        }
        ExtraShareTargetOption primaryTarget{*shareTarget, options.localInvite};
        if (LooksLikeNatInvite(*shareTarget)) {
            if (!options.peerInvite.empty()) {
                throw std::invalid_argument("--share invite cannot be combined with --peer-invite");
            }
            options.peerInvite = *shareTarget;
        } else if (!options.localInvite.empty()) {
            throw std::invalid_argument("--local-invite requires --share INVITE");
        }

        auto primarySpec = makeShareTargetSpec(primaryTarget, true);
        if (primarySpec.localPort == 0 && options.udpLocalPortProvided) {
            primarySpec.localPort = options.udpLocalPort;
        }
        options.udpSendTarget = primarySpec.target;
        options.udpSendTargetFromPeerInvite = primarySpec.fromPeerInvite;
        options.udpSendPeerInviteEndpoint = primarySpec.inviteEndpoint;
        options.udpLocalPort = primarySpec.localPort;
        if (options.udpLocalPort != 0) {
            options.udpLocalPortProvided = true;
        }
        options.udpLocalPortFromLocalInvite = primarySpec.localPortFromLocalInvite;
        options.udpSendTargetSpecs.push_back(primarySpec);
        options.udpSendTargets.push_back(primarySpec.target);

        std::set<uint16_t> usedLocalPorts;
        if (primarySpec.localPort != 0) {
            usedLocalPorts.insert(primarySpec.localPort);
        }
        for (const auto& target : extraShareTargets) {
            auto extraSpec = makeShareTargetSpec(target, false);
            if (extraSpec.localPort != 0 && !usedLocalPorts.insert(extraSpec.localPort).second) {
                throw std::invalid_argument("Each NAT share target local invite must use a unique local port");
            }
            options.udpSendTargetSpecs.push_back(extraSpec);
            options.udpSendTargets.push_back(extraSpec.target);
        }
        options.streamEncode = true;
        options.adaptBitrate = true;
        options.adaptResolution = true;
        if (!options.audioCapture) {
            options.audioCapture = true;
            options.audioCaptureSource = screenshare::AudioCaptureSource::SystemOutput;
        }
        if (!options.streamEncoderPreferenceProvided) {
            options.streamEncoderPreference = StreamEncoderPreference::Software;
        }
        options.sharePreset = true;
        if (!secondsProvided) {
            options.seconds = 0;
        }
        if (!options.udpMaxQueueMsProvided) {
            options.udpMaxQueueMs = DefaultShareUdpMaxQueueMs;
        }
    }
    if (shareRoomPort) {
        if (!options.udpSendTarget.empty()) {
            throw std::invalid_argument("--share-room cannot be combined with --udp-send");
        }
        if (options.udpReceivePort != 0) {
            throw std::invalid_argument("--share-room cannot be combined with --udp-recv");
        }
        if (!options.audioSendTarget.empty()) {
            throw std::invalid_argument("--share-room cannot be combined with --audio-send");
        }
        if (!options.localInvite.empty() || !options.peerInvite.empty()) {
            throw std::invalid_argument("--share-room uses signaling and cannot be combined with --local-invite or --peer-invite");
        }
        if (!extraShareTargets.empty()) {
            throw std::invalid_argument("--share-target is not supported with --share-room; signaling supplies room peers");
        }
        options.signalingLive = true;
        options.shareRoom = true;
        options.udpLocalPort = *shareRoomPort;
        options.udpLocalPortProvided = true;
        options.streamEncode = true;
        options.adaptBitrate = true;
        options.adaptResolution = true;
        if (!options.audioCapture) {
            options.audioCapture = true;
            options.audioCaptureSource = screenshare::AudioCaptureSource::SystemOutput;
        }
        if (!options.streamEncoderPreferenceProvided) {
            options.streamEncoderPreference = StreamEncoderPreference::Software;
        }
        options.sharePreset = true;
        if (!secondsProvided) {
            options.seconds = 0;
        }
        if (!options.udpMaxQueueMsProvided) {
            options.udpMaxQueueMs = DefaultShareUdpMaxQueueMs;
        }
    }
    if (watchPort) {
        if (options.udpReceivePort != 0) {
            throw std::invalid_argument("--watch cannot be combined with --udp-recv");
        }
        if (!options.udpSendTarget.empty()) {
            throw std::invalid_argument("--watch cannot be combined with --udp-send or --share");
        }
        options.udpReceivePort = *watchPort;
        options.previewWindow = true;
        options.decodeH264 = true;
        options.audioPlayback = true;
    }
    if (watchPort && !options.signalingRoomId.empty()) {
        options.signalingLive = true;
    }

    if (options.adaptResolutionMinScaleProvided || options.adaptResolutionCooldownProvided) {
        options.adaptResolution = true;
        options.adaptBitrate = true;
    }

    if (options.generateAccessCode) {
        if (!options.logPath.empty() || !options.saveReportPath.empty()) {
            throw std::invalid_argument("--generate-access-code cannot be combined with --log or --save-report");
        }
        options.sessionFingerprint = SessionFingerprint(options.sessionId);
        return options;
    }

    if (HasSignalingOptions(options) && !HasSignalingCommand(options) && !options.signalingLive) {
        throw std::invalid_argument("--signal-room, --signal-peer-id, --signal-candidate, --signal-name, --signal-platform, --signal-stun, --signal-setup-seconds, and --signal-timeout-ms require --signal-server or a --signal-* command");
    }
    if (HasSignalingCommand(options)) {
        if (options.signalingLive) {
            throw std::invalid_argument("--signal-server cannot be combined with standalone --signal-* diagnostics");
        }
        if (options.signalingTimeoutMs < 100 || options.signalingTimeoutMs > 30000) {
            throw std::invalid_argument("--signal-timeout-ms must be between 100 and 30000");
        }
        if (options.signalingCommand == SignalingCommand::Health) {
            if (!options.signalingRoomId.empty() || !options.signalingPeerId.empty() ||
                !options.signalingCandidate.empty() || !options.signalingName.empty() ||
                !options.signalingPlatform.empty() || options.signalingSetupSecondsProvided) {
                throw std::invalid_argument("--signal-health only accepts URL and --signal-timeout-ms");
            }
        } else {
            if (options.signalingRoomId.empty()) {
                throw std::invalid_argument("--signal-room is required for this signaling command");
            }
            if (options.signalingPeerId.empty()) {
                throw std::invalid_argument("--signal-peer-id is required for this signaling command");
            }
            screenshare::ValidateSignalingRoomId(options.signalingRoomId);
            screenshare::ValidateSignalingPeerId(options.signalingPeerId);
            if (options.signalingCommand == SignalingCommand::Join) {
                if (options.signalingCandidate.empty()) {
                    throw std::invalid_argument("--signal-candidate IP:PORT is required for --signal-join");
                }
            } else if (!options.signalingCandidate.empty() || !options.signalingName.empty() ||
                       !options.signalingPlatform.empty() || options.signalingSetupSecondsProvided) {
                throw std::invalid_argument("--signal-candidate, --signal-name, --signal-platform, and --signal-setup-seconds are only used with --signal-join or --signal-server");
            }
        }
        if (options.generateAccessCode ||
            options.listDisplays ||
            options.listH264Encoders ||
            options.listAudioDevices ||
            options.lanDiscover ||
            options.lanAdvertise ||
            options.stunQuery ||
            options.makeInvite ||
            options.natProbe ||
            HasUdpSession(options) ||
            options.accessCodeProvided ||
            options.allowPlaintext ||
            options.audioCapture ||
            options.audioDeviceIdProvided ||
            options.audioCodecProvided ||
            options.audioPlayback ||
            options.audioPlaybackLatencyProvided ||
            options.audioPlaybackMutedProvided ||
            options.audioPlaybackVolumeProvided ||
            options.avSync ||
            options.avSyncDisabled ||
            options.streamEncode ||
            options.streamEncoderPreferenceProvided ||
            options.udpLocalPortProvided ||
            options.udpPacingOptionProvided ||
            options.udpMaxQueueMsProvided ||
            options.adaptBitrate ||
            options.adaptMinBitrateProvided ||
            options.adaptReduceCooldownProvided ||
            options.adaptResolution ||
            options.adaptResolutionMinScaleProvided ||
            options.adaptResolutionCooldownProvided ||
            options.inviteEndpointPreferenceProvided ||
            !options.localInvite.empty() ||
            !options.peerInvite.empty() ||
            secondsProvided ||
            options.width != 0 ||
            options.height != 0 ||
            options.bitrate != 0 ||
            options.keyframeIntervalProvided ||
            !options.recordPath.empty() ||
            !options.capturedBmpPath.empty() ||
            !options.h264DumpPath.empty() ||
            options.decodeH264 ||
            !options.decodedBmpPath.empty() ||
            options.previewWindow ||
            options.previewLatencyProvided ||
            options.previewMaxLateProvided ||
            options.simulateLossProvided ||
            options.simulateJitterProvided) {
            throw std::invalid_argument("--signal-* commands are standalone signaling diagnostics");
        }
        options.sessionFingerprint = SessionFingerprint(options.sessionId);
        return options;
    }
    if (options.signalingLive) {
        if (options.signalingServerUrl.empty()) {
            options.signalingServerUrl = std::string(DefaultSignalingServerUrl);
        }
        if (options.signalingRoomId.empty()) {
            throw std::invalid_argument("Live signaling requires --signal-room ROOM");
        }
        screenshare::ValidateSignalingRoomId(options.signalingRoomId);
        if (options.signalingPeerId.empty()) {
            options.signalingPeerId = options.sessionId;
        }
        screenshare::ValidateSignalingPeerId(options.signalingPeerId);
        if (!options.signalingCandidate.empty()) {
            throw std::invalid_argument("--signal-candidate is only used with --signal-join diagnostics");
        }
        if (options.signalingTimeoutMs < 100 || options.signalingTimeoutMs > 30000) {
            throw std::invalid_argument("--signal-timeout-ms must be between 100 and 30000");
        }
        if (options.signalingSetupSeconds < 1 || options.signalingSetupSeconds > 60) {
            throw std::invalid_argument("--signal-setup-seconds must be between 1 and 60");
        }
        if (!options.shareRoom && options.udpReceivePort == 0) {
            throw std::invalid_argument("--signal-server requires --share-room or --watch/--udp-recv");
        }
        if (options.stunQuery || options.makeInvite || options.natProbe || options.lanDiscover) {
            throw std::invalid_argument("--signal-server cannot be combined with standalone NAT/LAN diagnostics");
        }
    }

    if (options.audioDeviceIdProvided && !options.audioCapture) {
        throw std::invalid_argument("--audio-device-id requires --audio-capture");
    }
    if (options.audioCodecProvided &&
        (!options.audioCapture || (options.audioSendTarget.empty() && options.udpSendTarget.empty()))) {
        throw std::invalid_argument("--audio-codec requires --audio-capture with --audio-send or --udp-send");
    }
    if (!options.audioSendTarget.empty()) {
        static_cast<void>(screenshare::ParseUdpSenderTarget(options.audioSendTarget));
        if (!options.audioCapture) {
            throw std::invalid_argument("--audio-send requires --audio-capture");
        }
        if (!options.udpSendTarget.empty()) {
            throw std::invalid_argument("--audio-send cannot be combined with --udp-send; use --udp-send with --audio-capture for combined audio/video streaming");
        }
    }
    if (options.lanName.empty()) {
        options.lanName = DefaultLanName();
    }
    if (options.lanDiscoverSeconds < 1 || options.lanDiscoverSeconds > 30) {
        throw std::invalid_argument("--lan-discover-seconds must be between 1 and 30");
    }
    if (options.lanAdvertise && options.lanDiscover) {
        throw std::invalid_argument("--lan-advertise cannot be combined with --lan-discover");
    }
    if (options.lanAdvertise && options.udpReceivePort == 0) {
        throw std::invalid_argument("--lan-advertise requires --watch or --udp-recv");
    }
    if (options.stunTimeoutMs < 100 || options.stunTimeoutMs > 30000) {
        throw std::invalid_argument("--stun-timeout-ms must be between 100 and 30000");
    }
    if (options.stunTimeoutProvided && !options.stunQuery) {
        throw std::invalid_argument("--stun-timeout-ms requires --stun");
    }
    if (options.inviteTtlProvided && !options.makeInvite) {
        throw std::invalid_argument("--invite-ttl-seconds requires --make-invite");
    }
    if (options.makeInvite && !options.stunQuery) {
        throw std::invalid_argument("--make-invite requires --stun HOST[:PORT]");
    }
    if (options.inviteTtlSeconds < 30 || options.inviteTtlSeconds > 3600) {
        throw std::invalid_argument("--invite-ttl-seconds must be between 30 and 3600");
    }
    if (options.inviteEndpointPreferenceProvided && !HasNatShareTarget(options)) {
        throw std::invalid_argument("--invite-endpoint requires --share INVITE or --share-target INVITE");
    }
    if (!options.localInvite.empty()) {
        if (!options.udpSendTargetFromPeerInvite) {
            throw std::invalid_argument("--local-invite requires --share INVITE");
        }
    }
    if (!options.peerInvite.empty() &&
        !options.natProbe &&
        options.udpReceivePort == 0 &&
        options.udpSendTarget.empty()) {
        throw std::invalid_argument("--peer-invite requires --nat-probe, --watch, --udp-recv, or --share");
    }
    if (options.natProbeIntervalProvided && !options.natProbe && options.peerInvite.empty()) {
        throw std::invalid_argument("--nat-probe-interval-ms requires --nat-probe or --peer-invite");
    }
    if (options.natProbe && options.peerInvite.empty()) {
        throw std::invalid_argument("--nat-probe requires --peer-invite INVITE");
    }
    if (options.natProbe && options.stunQuery) {
        throw std::invalid_argument("--nat-probe cannot be combined with --stun; use an invite made with --make-invite");
    }
    if (options.natProbeIntervalMs < 50 || options.natProbeIntervalMs > 5000) {
        throw std::invalid_argument("--nat-probe-interval-ms must be between 50 and 5000");
    }
    if (options.stunQuery && !options.makeInvite &&
        (options.generateAccessCode ||
         options.listDisplays ||
         options.listH264Encoders ||
         options.listAudioDevices ||
         options.lanDiscover ||
         options.lanAdvertise ||
         HasUdpSession(options) ||
         options.audioCapture ||
         options.streamEncode ||
         !options.localInvite.empty() ||
         !options.peerInvite.empty() ||
         options.width != 0 ||
         options.height != 0 ||
         options.bitrate != 0 ||
         options.keyframeIntervalProvided ||
         !options.recordPath.empty() ||
         !options.capturedBmpPath.empty())) {
        throw std::invalid_argument("--stun is a standalone diagnostic command");
    }
    if (options.makeInvite &&
        (options.generateAccessCode ||
         options.listDisplays ||
         options.listH264Encoders ||
         options.listAudioDevices ||
         options.lanDiscover ||
         options.lanAdvertise ||
         HasUdpSession(options) ||
         options.audioCapture ||
         options.streamEncode ||
         !options.localInvite.empty() ||
         !options.peerInvite.empty() ||
         options.width != 0 ||
         options.height != 0 ||
         options.bitrate != 0 ||
         options.keyframeIntervalProvided ||
         !options.recordPath.empty() ||
         !options.capturedBmpPath.empty())) {
        throw std::invalid_argument("--make-invite is a standalone NAT setup command");
    }
    if (options.natProbe &&
        (options.generateAccessCode ||
         options.listDisplays ||
         options.listH264Encoders ||
         options.listAudioDevices ||
         options.lanDiscover ||
         options.lanAdvertise ||
         HasUdpSession(options) ||
         options.audioCapture ||
         options.streamEncode ||
         options.width != 0 ||
         options.height != 0 ||
         options.bitrate != 0 ||
         options.keyframeIntervalProvided ||
         !options.recordPath.empty() ||
         !options.capturedBmpPath.empty())) {
        throw std::invalid_argument("--nat-probe is a standalone NAT setup command");
    }
    if (options.accessCodeProvided &&
        options.udpReceivePort == 0 &&
        options.udpSendTarget.empty() &&
        options.audioSendTarget.empty() &&
        !options.shareRoom &&
        !options.makeInvite &&
        !options.natProbe) {
        throw std::invalid_argument("--access-code requires --share, --share-room, --watch, --udp-send, --udp-recv, --audio-send, --make-invite, or --nat-probe");
    }
    if (options.allowPlaintext && options.accessCodeProvided) {
        throw std::invalid_argument("--allow-plaintext cannot be combined with --access-code");
    }
    if (options.allowPlaintext && !HasUdpSession(options) && !options.makeInvite && !options.natProbe) {
        throw std::invalid_argument("--allow-plaintext requires --share, --watch, --udp-send, --udp-recv, --audio-send, --make-invite, --nat-probe, or --local-invite");
    }
    if (options.makeInvite && !options.accessCodeProvided && !options.allowPlaintext) {
        throw std::invalid_argument("--make-invite requires --access-code CODE or --allow-plaintext");
    }
    if (options.natProbe && !options.accessCodeProvided && !options.allowPlaintext) {
        throw std::invalid_argument("--nat-probe requires --access-code CODE or --allow-plaintext");
    }
    if (options.fps <= 0 || options.fps > 240) {
        throw std::invalid_argument("--fps must be between 1 and 240");
    }
    if (options.previewWindow && !secondsProvided) {
        options.seconds = 0;
    }
    if ((options.previewLatencyProvided || options.previewMaxLateProvided) && !options.previewWindow) {
        throw std::invalid_argument("--preview-latency-ms and --preview-max-late-ms require --preview");
    }
    if (options.audioPlaybackLatencyProvided && !options.audioPlayback) {
        throw std::invalid_argument("--audio-playback-latency-ms requires --audio-playback");
    }
    if ((options.audioPlaybackMutedProvided || options.audioPlaybackVolumeProvided) && !options.audioPlayback) {
        throw std::invalid_argument("--audio-playback-muted and --audio-playback-volume require --audio-playback");
    }
    if (options.avSync && options.avSyncDisabled) {
        throw std::invalid_argument("--av-sync cannot be combined with --no-av-sync");
    }
    if (options.avSyncDisabled && (!options.previewWindow || !options.audioPlayback)) {
        throw std::invalid_argument("--no-av-sync requires --preview and --audio-playback");
    }
    if (!options.avSyncExplicit && !options.avSyncDisabled && options.previewWindow && options.audioPlayback) {
        options.avSync = true;
    }
    if (options.avSync && (!options.previewWindow || !options.audioPlayback)) {
        throw std::invalid_argument("--av-sync requires --preview and --audio-playback");
    }
    if (options.avSync && !options.previewLatencyProvided) {
        options.previewLatencyMs = DefaultAvSyncPreviewLatencyMs;
    }
    if (options.avSync) {
        options.audioPlaybackLatencyMs = std::max(options.audioPlaybackLatencyMs, options.previewLatencyMs);
    }
    if (options.previewLatencyMs < 0 || options.previewLatencyMs > 2000) {
        throw std::invalid_argument("--preview-latency-ms must be between 0 and 2000");
    }
    if (options.previewMaxLateMs < 0 || options.previewMaxLateMs > 5000) {
        throw std::invalid_argument("--preview-max-late-ms must be between 0 and 5000");
    }
    if (options.audioPlaybackLatencyMs < 0 || options.audioPlaybackLatencyMs > 2000) {
        throw std::invalid_argument("--audio-playback-latency-ms must be between 0 and 2000");
    }
    if (options.audioPlaybackVolumePercent < 0.0f || options.audioPlaybackVolumePercent > 200.0f) {
        throw std::invalid_argument("--audio-playback-volume must be between 0 and 200 percent");
    }
    if (options.seconds < 0) {
        throw std::invalid_argument("--seconds must be non-negative");
    }
    if (options.seconds == 0 && !options.previewWindow && !options.sharePreset) {
        throw std::invalid_argument("--seconds 0 is only supported with --preview or --share");
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
    if (options.udpPacingOptionProvided && options.udpSendTarget.empty() && !options.shareRoom) {
        throw std::invalid_argument("--no-udp-pacing requires --udp-send or --share-room");
    }
    if (options.udpMaxQueueMsProvided && options.udpSendTarget.empty() && !options.shareRoom) {
        throw std::invalid_argument("--udp-max-queue-ms requires --udp-send or --share-room");
    }
    if (options.udpLocalPortProvided && options.udpSendTarget.empty() && options.audioSendTarget.empty() && !options.shareRoom) {
        throw std::invalid_argument("--udp-local-port requires --udp-send, --share, --share-room, or --audio-send");
    }
    if (options.udpMaxQueueMs < 0 || options.udpMaxQueueMs > 5000) {
        throw std::invalid_argument("--udp-max-queue-ms must be between 0 and 5000");
    }
    if ((options.adaptResolution || options.adaptResolutionMinScaleProvided || options.adaptResolutionCooldownProvided) &&
        options.udpSendTarget.empty() && !options.shareRoom) {
        throw std::invalid_argument("--adapt-resolution, --adapt-resolution-min-scale, and --adapt-resolution-cooldown require --udp-send or --share-room");
    }
    if (options.adaptBitrate && options.udpSendTarget.empty() && !options.shareRoom) {
        throw std::invalid_argument("--adapt-bitrate requires --udp-send or --share-room");
    }
    if ((options.adaptMinBitrateProvided || options.adaptReduceCooldownProvided) && options.udpSendTarget.empty() && !options.shareRoom) {
        throw std::invalid_argument("--adapt-min-bitrate-mbps and --adapt-reduce-cooldown require --udp-send or --share-room");
    }
    if (options.adaptResolution && !options.recordPath.empty()) {
        throw std::invalid_argument("--adapt-resolution cannot be combined with --record");
    }
    if (options.adaptReduceCooldownSeconds < 0 || options.adaptReduceCooldownSeconds > 30) {
        throw std::invalid_argument("--adapt-reduce-cooldown must be between 0 and 30 seconds");
    }
    if (options.adaptResolutionMinScale < 0.25f || options.adaptResolutionMinScale > 1.0f) {
        throw std::invalid_argument("--adapt-resolution-min-scale must be between 0.25 and 1.0");
    }
    if (options.adaptResolutionCooldownSeconds < 0 || options.adaptResolutionCooldownSeconds > 60) {
        throw std::invalid_argument("--adapt-resolution-cooldown must be between 0 and 60 seconds");
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
        (options.listDisplays || options.listH264Encoders || options.listAudioDevices || options.audioCapture ||
         options.audioDeviceIdProvided || options.audioCodecProvided || !options.audioSendTarget.empty() ||
         !options.recordPath.empty() || !options.capturedBmpPath.empty() ||
         options.streamEncode || options.streamEncoderPreferenceProvided || !options.udpSendTarget.empty() ||
         options.udpLocalPortProvided || options.udpPacingOptionProvided || options.udpMaxQueueMsProvided ||
         options.adaptBitrate || options.adaptMinBitrateProvided ||
         options.adaptReduceCooldownProvided || options.adaptResolution || options.adaptResolutionMinScaleProvided ||
         options.adaptResolutionCooldownProvided || options.keyframeIntervalProvided)) {
        throw std::invalid_argument("--udp-recv cannot be combined with --list, --list-h264-encoders, --list-audio-devices, --audio-capture, --audio-device-id, --audio-send, --record, --dump-capture-bmp, --stream-encode, --stream-encoder, --udp-send, --udp-local-port, --no-udp-pacing, --udp-max-queue-ms, --adapt-bitrate, --adapt-min-bitrate-mbps, --adapt-reduce-cooldown, --adapt-resolution, --adapt-resolution-min-scale, --adapt-resolution-cooldown, or --keyframe-interval");
    }
    if (options.listH264Encoders &&
        (options.listDisplays || options.listAudioDevices || options.audioCapture || options.audioDeviceIdProvided ||
         options.audioCodecProvided || !options.audioSendTarget.empty() || !options.recordPath.empty() || !options.capturedBmpPath.empty() ||
         options.streamEncode || options.streamEncoderPreferenceProvided || !options.udpSendTarget.empty() ||
         options.udpLocalPortProvided || options.udpPacingOptionProvided || options.adaptBitrate || options.adaptMinBitrateProvided ||
         options.adaptReduceCooldownProvided || options.adaptResolution || options.adaptResolutionMinScaleProvided ||
         options.adaptResolutionCooldownProvided || options.keyframeIntervalProvided ||
         options.decodeH264 || options.previewWindow || options.previewLatencyProvided || options.previewMaxLateProvided ||
         options.audioPlayback || options.audioPlaybackLatencyProvided || options.audioPlaybackMutedProvided ||
         options.audioPlaybackVolumeProvided || options.avSync || options.avSyncDisabled)) {
        throw std::invalid_argument("--list-h264-encoders can only be combined with --width, --height, --fps, and --bitrate-mbps");
    }
    if (options.listAudioDevices &&
        (options.listDisplays || options.listH264Encoders || options.audioCapture || options.audioDeviceIdProvided ||
         options.audioCodecProvided || !options.audioSendTarget.empty() ||
         options.width != 0 || options.height != 0 || !options.recordPath.empty() || !options.capturedBmpPath.empty() ||
         options.streamEncode || options.streamEncoderPreferenceProvided || !options.udpSendTarget.empty() ||
         options.udpLocalPortProvided || options.udpPacingOptionProvided || options.adaptBitrate || options.adaptMinBitrateProvided ||
         options.adaptReduceCooldownProvided || options.adaptResolution || options.adaptResolutionMinScaleProvided ||
         options.adaptResolutionCooldownProvided || options.keyframeIntervalProvided || options.udpReceivePort != 0 ||
         !options.h264DumpPath.empty() || options.decodeH264 || !options.decodedBmpPath.empty() ||
          options.previewWindow || options.previewLatencyProvided || options.previewMaxLateProvided ||
          options.audioPlayback || options.audioPlaybackLatencyProvided || options.audioPlaybackMutedProvided ||
          options.audioPlaybackVolumeProvided || options.avSync || options.avSyncDisabled ||
         options.simulateLossProvided || options.simulateJitterProvided)) {
        throw std::invalid_argument("--list-audio-devices cannot be combined with capture, stream, receiver, or video options");
    }
    const bool audioCaptureWithVideoSend = options.audioCapture && (!options.udpSendTarget.empty() || options.shareRoom);
    if (options.audioCapture && !audioCaptureWithVideoSend &&
        (options.listDisplays || options.listH264Encoders || options.listAudioDevices ||
         options.width != 0 || options.height != 0 || !options.recordPath.empty() || !options.capturedBmpPath.empty() ||
         options.streamEncode || options.streamEncoderPreferenceProvided ||
         (options.udpLocalPortProvided && options.audioSendTarget.empty()) ||
         options.udpPacingOptionProvided || options.adaptBitrate || options.adaptMinBitrateProvided ||
         options.adaptReduceCooldownProvided || options.adaptResolution || options.adaptResolutionMinScaleProvided ||
         options.adaptResolutionCooldownProvided || options.keyframeIntervalProvided || options.udpReceivePort != 0 ||
         !options.h264DumpPath.empty() || options.decodeH264 || !options.decodedBmpPath.empty() ||
          options.previewWindow || options.previewLatencyProvided || options.previewMaxLateProvided ||
          options.audioPlayback || options.audioPlaybackLatencyProvided || options.audioPlaybackMutedProvided ||
          options.audioPlaybackVolumeProvided || options.avSync || options.avSyncDisabled ||
         options.simulateLossProvided || options.simulateJitterProvided)) {
        throw std::invalid_argument("--audio-capture is currently a standalone diagnostic mode and can only be combined with --seconds, --audio-device-id, --audio-send, --udp-local-port, and --audio-codec");
    }
    if (audioCaptureWithVideoSend &&
        (options.listDisplays || options.listH264Encoders || options.listAudioDevices ||
         !options.recordPath.empty() || !options.capturedBmpPath.empty() ||
         options.udpReceivePort != 0 || !options.h264DumpPath.empty() || options.decodeH264 ||
         !options.decodedBmpPath.empty() || options.previewWindow || options.previewLatencyProvided ||
         options.previewMaxLateProvided || options.audioPlayback || options.audioPlaybackLatencyProvided ||
         options.audioPlaybackMutedProvided || options.audioPlaybackVolumeProvided || options.avSync || options.avSyncDisabled ||
         options.simulateLossProvided || options.simulateJitterProvided)) {
        throw std::invalid_argument("--audio-capture with --udp-send is only supported for live UDP sending, not receiver, recording, BMP dump, or preview options");
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
    if (options.audioPlayback && options.udpReceivePort == 0) {
        throw std::invalid_argument("--audio-playback requires --udp-recv");
    }

    options.sessionFingerprint = SessionFingerprint(options.sessionId);
    return options;
}

std::string UniqueArchiveName(const std::string& requestedName, std::vector<std::string>& usedNames)
{
    std::string baseName = ToZipPath(requestedName);
    if (baseName.empty()) {
        baseName = "file";
    }

    auto isUsed = [&](const std::string& name) {
        return std::find(usedNames.begin(), usedNames.end(), name) != usedNames.end();
    };

    if (!isUsed(baseName)) {
        usedNames.push_back(baseName);
        return baseName;
    }

    const auto slash = baseName.find_last_of('/');
    const auto dot = baseName.find_last_of('.');
    const bool hasExtension = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    const std::string prefix = hasExtension ? baseName.substr(0, dot) : baseName;
    const std::string extension = hasExtension ? baseName.substr(dot) : std::string{};
    for (int suffix = 2;; ++suffix) {
        const std::string candidate = prefix + "-" + std::to_string(suffix) + extension;
        if (!isUsed(candidate)) {
            usedNames.push_back(candidate);
            return candidate;
        }
    }
}

std::string CurrentLocalTimeText()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    if (const std::tm* local = std::localtime(&time)) {
        localTime = *local;
    }

    std::ostringstream text;
    text << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return text.str();
}

bool IsSensitiveOption(std::string_view option)
{
    return option == "--access-code" || option == "--session-code";
}

bool RedactInlineSensitiveOption(std::string& option)
{
    constexpr std::string_view accessCodePrefix = "--access-code=";
    constexpr std::string_view sessionCodePrefix = "--session-code=";
    if (option.rfind(accessCodePrefix, 0) == 0) {
        option = std::string(accessCodePrefix) + "<redacted>";
        return true;
    }
    if (option.rfind(sessionCodePrefix, 0) == 0) {
        option = std::string(sessionCodePrefix) + "<redacted>";
        return true;
    }
    return false;
}

std::string JoinCommandLine(int argc, char** argv)
{
    std::ostringstream command;
    bool redactNext = false;
    for (int index = 0; index < argc; ++index) {
        if (index > 0) {
            command << ' ';
        }
        std::string arg = argv[index] != nullptr ? argv[index] : "";
        if (redactNext) {
            arg = "<redacted>";
            redactNext = false;
        } else if (RedactInlineSensitiveOption(arg)) {
            redactNext = false;
        } else if (IsSensitiveOption(arg)) {
            redactNext = true;
        }
        const bool needsQuotes = arg.find_first_of(" \t\"") != std::string::npos;
        if (!needsQuotes) {
            command << arg;
            continue;
        }
        command << '"';
        for (const char ch : arg) {
            if (ch == '"') {
                command << '\\';
            }
            command << ch;
        }
        command << '"';
    }
    return command.str();
}

std::string PowerShellQuote(std::string_view value)
{
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('\'');
    for (const char ch : value) {
        if (ch == '\'') {
            quoted.append("''");
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

std::filesystem::path TemporaryReportLogPath(const std::filesystem::path& reportPath)
{
    std::filesystem::path tempPath = reportPath;
    tempPath += ".console.log.tmp";
    return tempPath;
}

void AppendReceiverFeedbackSummary(
    std::ostringstream& report,
    const std::optional<screenshare::udp_protocol::FeedbackSnapshot>& feedback)
{
    report << "Latest receiver feedback: ";
    if (!feedback) {
        report << "not observed\n";
        return;
    }

    const auto& snapshot = *feedback;
    report
        << "\n"
        << "  Session fingerprint: "
        << (snapshot.sessionFingerprint == 0 ? "unknown" : FormatSessionFingerprint(snapshot.sessionFingerprint))
        << "\n"
        << "  Access code required: " << (snapshot.accessCodeFingerprint == 0 ? "no" : "yes") << "\n"
        << "  Health: " << screenshare::udp_protocol::FeedbackHealthStateName(snapshot.healthState) << "\n"
        << "  Sequence: " << snapshot.sequence << "\n"
        << "  Completed frames: " << snapshot.completedFrames << "\n"
        << "  Dropped datagrams: " << snapshot.droppedDatagrams << "\n"
        << "  Invalid datagrams: " << snapshot.invalidDatagrams << "\n"
        << "  Incomplete frames dropped: " << snapshot.incompleteFramesDropped << "\n"
        << "  Decode resyncs: " << snapshot.decodeResyncs << "\n"
        << "  Decode skipped packets: " << snapshot.decodeSkippedPackets << "\n"
        << "  Preview late drops: " << snapshot.previewLateDrops << "\n"
        << "  Preview overflow drops: " << snapshot.previewOverflowDrops << "\n"
        << "  Pending frames: " << snapshot.pendingFrames << "\n"
        << "  Pending decode packets: " << snapshot.pendingDecodePackets << "\n"
        << "  Preview queued frames: " << snapshot.previewQueuedFrames << "\n";
}

void WriteSavedReport(
    const std::filesystem::path& outputPath,
    const std::optional<std::filesystem::path>& consoleLogPath,
    const char* argv0,
    int argc,
    char** argv,
    const SavedReportContext& reportContext,
    int exitCode)
{
    const std::filesystem::path executablePath = std::filesystem::absolute(argv0);
    const std::filesystem::path dependencyManifest =
        executablePath.has_parent_path() ?
        executablePath.parent_path() / "ScreenShare-runtime-dependencies.txt" :
        std::filesystem::path("ScreenShare-runtime-dependencies.txt");

    std::ostringstream report;
    report
        << "ScreenShare saved run report\n\n"
        << "Generated local time: " << CurrentLocalTimeText() << "\n"
        << "Session ID: " << reportContext.sessionId << "\n"
        << "Session fingerprint: " << FormatSessionFingerprint(reportContext.sessionFingerprint) << "\n"
        << "Access code required: " << (reportContext.accessCodeRequired ? "yes" : "no") << "\n"
        << "UDP encryption: " << (reportContext.encryptionEnabled ? "yes" : "no") << "\n"
        << "Exit code: " << exitCode << "\n"
        << "Working directory: " << std::filesystem::current_path().string() << "\n"
        << "Executable: " << executablePath.string() << "\n"
        << "Command line: " << JoinCommandLine(argc, argv) << "\n"
        << "Build: "
#ifdef NDEBUG
        << "release"
#else
        << "debug"
#endif
        << "\n"
        << "Pointer bits: " << (sizeof(void*) * 8) << "\n"
        << "Console log: "
        << (consoleLogPath ? std::filesystem::absolute(*consoleLogPath).string() : "not captured")
        << "\n";
    if (consoleLogPath && std::filesystem::exists(*consoleLogPath)) {
        report << "Console log bytes: " << std::filesystem::file_size(*consoleLogPath) << "\n";
    }
    AppendReceiverFeedbackSummary(report, reportContext.latestReceiverFeedback);
    report
        << "Runtime dependency manifest: "
        << (std::filesystem::exists(dependencyManifest) ? dependencyManifest.string() : "not found")
        << "\n";

    ZipWriter zip(outputPath);
    std::vector<std::string> archiveNames;
    zip.AddFile(UniqueArchiveName("ScreenShare-report.txt", archiveNames), StringBytes(report.str()));

    if (consoleLogPath && std::filesystem::exists(*consoleLogPath) && std::filesystem::is_regular_file(*consoleLogPath)) {
        zip.AddFile(
            UniqueArchiveName("logs/console.log", archiveNames),
            ReadBinaryFile(*consoleLogPath));
    }

    if (std::filesystem::exists(dependencyManifest) && std::filesystem::is_regular_file(dependencyManifest)) {
        zip.AddFile(
            UniqueArchiveName("runtime/ScreenShare-runtime-dependencies.txt", archiveNames),
            ReadBinaryFile(dependencyManifest));
    }

    zip.Finish();
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

void PrintAudioDevices()
{
    auto printDevices = [](screenshare::AudioCaptureSource source) {
        const auto devices = screenshare::WasapiCapture::EnumerateDevices(source);
        std::cout << screenshare::AudioCaptureSourceName(source) << " audio devices";
        if (source == screenshare::AudioCaptureSource::SystemOutput) {
            std::cout << " (captured with WASAPI loopback)";
        }
        std::cout << ":\n";

        if (devices.empty()) {
            std::cout << "  none\n";
            return;
        }

        for (size_t index = 0; index < devices.size(); ++index) {
            const auto& device = devices[index];
            std::cout
                << "  [" << index << "] "
                << (device.isDefault ? "default " : "")
                << "\"" << screenshare::Narrow(device.name) << "\""
                << "\n      id=" << screenshare::Narrow(device.id)
                << "\n";
        }
    };

    printDevices(screenshare::AudioCaptureSource::SystemOutput);
    printDevices(screenshare::AudioCaptureSource::Microphone);
}

void RunAudioCaptureStats(const Options& options, SavedReportContext& reportContext)
{
    screenshare::WasapiCapture capture;
    screenshare::AudioCaptureConfig config;
    config.source = options.audioCaptureSource;
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

    while (!StopRequested(options) && Clock::now() - startedAt < std::chrono::seconds(options.seconds)) {
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

void RunCaptureStats(const Options& options, SavedReportContext& reportContext)
{
    std::optional<screenshare::NatInvite> peerInvite;
    if (!options.peerInvite.empty()) {
        peerInvite = ParseValidatedPeerInvite(options);
    }

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

    WarnIfPlaintextUdpSession(options);

    std::cout << "Capturing display " << options.displayIndex
              << " at target " << options.fps << " FPS";

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
    uint64_t bitrateAdaptations = 0;
    uint64_t bitrateAdaptationFailures = 0;
    uint32_t lastBitrateAdaptationAttempt = 0;
    const char* bitrateAdaptationStatus = options.adaptBitrate ? "waiting" : "disabled";
    AdaptiveBitrateAdvisor bitrateAdvisor;
    std::vector<ResolutionTier> adaptiveResolutionTiers;
    size_t adaptiveResolutionTierIndex = 0;
    uint64_t resolutionAdaptations = 0;
    uint64_t resolutionAdaptationFailures = 0;
    int resolutionCooldownRemaining = 0;
    uint32_t resolutionStableFeedbackReports = 0;
    uint32_t resolutionReductionPressureReports = 0;
    const char* resolutionAdaptationStatus = options.adaptResolution ? "waiting" : "disabled";
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
    std::unique_ptr<UdpSenderFanout> udpSender;
    std::unique_ptr<AudioUdpCaptureWorker> audioCaptureWorker;
    std::unique_ptr<LiveSignalingRuntime> liveSignalingRuntime;
    std::vector<UdpSendTargetSpec> udpSendTargetSpecs = options.udpSendTargetSpecs;
    std::set<std::string> liveSignalingSendTargets;
    uint64_t audioPacingBitrate = 0;
    bool audioPacingApplied = false;

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
            (options.audioCapture ? audioPacingBitrate : 0ULL);
        return static_cast<uint32_t>(std::min<uint64_t>(
            combined,
            std::numeric_limits<uint32_t>::max()));
    };

    auto updateUdpPacingBitrate = [&]() {
        if (udpSender) {
            udpSender->SetPacingBitrate(combinedUdpPacingBitrate());
        }
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
        udpConfigs.reserve(udpTargets.size());
        for (size_t targetIndex = 0; targetIndex < udpTargets.size(); ++targetIndex) {
            const auto& target = udpTargets[targetIndex];
            auto udpConfig = screenshare::ParseUdpSenderTarget(target.target);
            if (targetIndex > 0 &&
                target.fromPeerInvite &&
                target.localPort == 0 &&
                !udpConfigs.empty() &&
                udpConfigs.front().collectNatProbeTargets) {
                udpConfigs.front().additionalTargets.push_back(
                    screenshare::UdpSenderEndpoint{udpConfig.host, udpConfig.port});
                continue;
            }
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
            if (options.audioCapture) {
                udpConfig.maxQueuedDatagrams = 16'384;
            }
            udpConfigs.push_back(std::move(udpConfig));
        }

        udpSender = std::make_unique<UdpSenderFanout>();
        udpSender->Open(udpConfigs);
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
            << " adaptive_bitrate=" << (options.adaptBitrate ? "enabled" : "advice-only")
            << " adapt_min_bitrate_mbps=" << Mbps(bitrateAdvisor.minBitrate())
            << " adapt_reduce_cooldown_s=" << options.adaptReduceCooldownSeconds
            << " max_queued_datagrams=" << udpConfigs.front().maxQueuedDatagrams
            << " access_code=" << (options.accessCodeProvided ? "required" : "none")
            << "\n";
        if (options.audioCapture) {
            audioCaptureWorker = std::make_unique<AudioUdpCaptureWorker>();
            audioCaptureWorker->Start(
                [udpSender = udpSender.get()](const screenshare::UdpAudioPacket& packet) {
                    udpSender->SendAudioPacket(packet);
                },
                options.audioCaptureSource,
                options.audioDeviceId,
                options.audioCodec);
            std::cout
                << "Audio capture worker started source="
                << screenshare::AudioCaptureSourceName(options.audioCaptureSource)
                << "\n";
        }
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

    auto drainLiveSignalingSendTargets = [&]() {
        if (!liveSignalingRuntime) {
            return;
        }

        const uint64_t probeSession = NatProbeSessionFingerprint(options);
        for (const auto& peer : liveSignalingRuntime->DrainDiscoveredPeers()) {
            const std::string endpoint = SignalingCandidateEndpoint(peer.candidate);
            if (!liveSignalingSendTargets.insert(endpoint).second) {
                continue;
            }

            UdpSendTargetSpec target = SignalingSendTargetSpec(
                peer.candidate,
                udpSendTargetSpecs.empty() ? options.udpLocalPort : static_cast<uint16_t>(0),
                probeSession);
            udpSendTargetSpecs.push_back(target);

            bool active = false;
            if (udpSender) {
                active = udpSender->AddAdditionalTarget(
                    screenshare::UdpSenderEndpoint{peer.candidate.ip, peer.candidate.port});
            }

            std::cout
                << "signaling_live_sender_peer=added"
                << " room=" << options.signalingRoomId
                << " peer_id=" << peer.peerId
                << " endpoint=" << endpoint
                << " active=" << (udpSender ? (active ? "yes" : "duplicate") : "pending")
                << "\n";
        }
    };

    auto applyAdaptiveBitrate = [&]() {
        if (!options.adaptBitrate || !udpSender || !streamEncoder || !bitrateAdvisor.configured()) {
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

    auto restartStreamAtResolution = [&](size_t tierIndex, const char* direction, const char* reason) {
        if (tierIndex >= adaptiveResolutionTiers.size()) {
            return;
        }

        const auto& tier = adaptiveResolutionTiers[tierIndex];
        try {
            if (streamEncoder) {
                sendStreamPackets(streamEncoder->Drain());
                streamEncoder.reset();
            }

            config.targetWidth = tier.width;
            config.targetHeight = tier.height;
            ConfigureCapturePayloads(config, options, streamEncoderPreference);
            capturer.Stop();
            capturer.Start(config);
            hasFrame = false;
            adaptiveResolutionTierIndex = tierIndex;
            resolutionCooldownRemaining = options.adaptResolutionCooldownSeconds;
            resolutionStableFeedbackReports = 0;
            resolutionReductionPressureReports = 0;
            ++resolutionAdaptations;
            resolutionAdaptationStatus = std::strcmp(direction, "increase") == 0 ? "applied_increase" : "applied_reduce";
            std::cout
                << "Adaptive resolution applied output=" << tier.width << "x" << tier.height
                << " scale=" << tier.scale
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

    auto applyAdaptiveResolution = [&](const screenshare::UdpSenderStats& udpStats) {
        if (!options.adaptResolution || adaptiveResolutionTiers.size() < 2 || !streamEncoder || !bitrateAdvisor.configured()) {
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

    const auto targetFrameTime = std::chrono::microseconds(1'000'000 / options.fps);
    auto nextFrameAt = Clock::now();
    auto keepRunning = [&]() {
        if (StopRequested(options)) {
            return false;
        }
        return options.seconds == 0 || Clock::now() - startedAt < std::chrono::seconds(options.seconds);
    };

    while (keepRunning()) {
        std::this_thread::sleep_until(nextFrameAt);
        nextFrameAt += targetFrameTime;
        drainLiveSignalingSendTargets();

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
            if (options.adaptResolution && adaptiveResolutionTiers.empty()) {
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
                        encoderConfig.fps = options.fps;
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

                }

                ensureUdpSenderForTargets();

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
                << " audio_capture=" << (options.audioCapture ? (audioCaptureStatsNow.started ? "running" : "starting") : "disabled")
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
        sendStreamPackets(drainedPackets);
    }

    if (audioCaptureWorker) {
        audioCaptureWorker->Stop();
        audioCaptureWorker->ThrowIfFailed();
    }

    if (udpSender) {
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
        << ", audio capture: " << (options.audioCapture ? (finalAudioCaptureStats.started ? "done" : "not-started") : "disabled")
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

screenshare::SignalingCandidate ParseSignalingCandidateEndpoint(const std::string& endpoint)
{
    std::string host;
    std::string portText;
    if (!endpoint.empty() && endpoint.front() == '[') {
        const size_t close = endpoint.find(']');
        if (close == std::string::npos || close + 1 >= endpoint.size() || endpoint[close + 1] != ':') {
            throw std::invalid_argument("--signal-candidate expects IP:PORT");
        }
        host = endpoint.substr(1, close - 1);
        portText = endpoint.substr(close + 2);
    } else {
        const size_t separator = endpoint.rfind(':');
        if (separator == std::string::npos || separator == 0 || separator + 1 >= endpoint.size()) {
            throw std::invalid_argument("--signal-candidate expects IP:PORT");
        }
        host = endpoint.substr(0, separator);
        portText = endpoint.substr(separator + 1);
    }

    char* end = nullptr;
    const long port = std::strtol(portText.c_str(), &end, 10);
    if (end == portText.c_str() || *end != '\0' || port <= 0 || port > 65535) {
        throw std::invalid_argument("Invalid UDP port in --signal-candidate: " + endpoint);
    }

    screenshare::SignalingCandidate candidate;
    candidate.ip = host;
    candidate.port = static_cast<uint16_t>(port);
    screenshare::ValidateSignalingCandidate(candidate);
    return candidate;
}

void PrintSignalingPeers(const screenshare::SignalingRoomResponse& response)
{
    std::cout
        << "signaling_result=" << (response.ok ? "ok" : "not_ok") << "\n"
        << "signaling_peers=" << response.peers.size() << "\n";
    for (const auto& peer : response.peers) {
        std::cout
            << "signaling_peer id=" << peer.peerId
            << " last_seen=" << peer.lastSeen;
        if (!peer.metadata.name.empty()) {
            std::cout << " name=\"" << screenshare::SignalingJsonEscape(peer.metadata.name) << "\"";
        }
        if (!peer.metadata.platform.empty()) {
            std::cout << " platform=\"" << screenshare::SignalingJsonEscape(peer.metadata.platform) << "\"";
        }
        std::cout << "\n";
        for (const auto& candidate : peer.candidates) {
            std::cout
                << "signaling_candidate peer=" << peer.peerId
                << " type=" << candidate.type
                << " endpoint=" << candidate.ip << ":" << candidate.port
                << " protocol=" << candidate.protocol << "\n";
        }
    }
}

void RunSignaling(const Options& options)
{
    screenshare::SignalingClientConfig config;
    config.serverUrl = options.signalingServerUrl;
    config.timeout = std::chrono::milliseconds(options.signalingTimeoutMs);
    screenshare::SignalingClient client(std::move(config));

    switch (options.signalingCommand) {
    case SignalingCommand::Health:
        client.Health();
        std::cout << "signaling_health=ok\n";
        break;
    case SignalingCommand::Join: {
        screenshare::SignalingPeerState peer;
        peer.peerId = options.signalingPeerId;
        peer.candidates.push_back(ParseSignalingCandidateEndpoint(options.signalingCandidate));
        peer.metadata.name = options.signalingName;
        peer.metadata.platform = options.signalingPlatform;
        PrintSignalingPeers(client.Join(options.signalingRoomId, peer));
        break;
    }
    case SignalingCommand::Peers:
        PrintSignalingPeers(client.Peers(options.signalingRoomId, options.signalingPeerId));
        break;
    case SignalingCommand::Heartbeat:
        client.Heartbeat(options.signalingRoomId, options.signalingPeerId);
        std::cout << "signaling_heartbeat=ok\n";
        break;
    case SignalingCommand::Leave:
        client.Leave(options.signalingRoomId, options.signalingPeerId);
        std::cout << "signaling_leave=ok\n";
        break;
    case SignalingCommand::None:
        throw std::logic_error("RunSignaling called without a signaling command");
    }
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
    const auto startedAt = std::chrono::steady_clock::now();
    const auto deadline = startedAt + std::chrono::seconds(options.signalingSetupSeconds);
    int polls = 0;
    do {
        MergeSignalingPeers(peersById, client.Join(options.signalingRoomId, peer));
        ++polls;
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } while (true);

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
                << " type=" << candidate.type
                << " protocol=" << candidate.protocol
                << "\n";

            if (options.shareRoom) {
                sendTargets.push_back(SignalingSendTargetSpec(
                    candidate,
                    sendTargets.empty() ? localPort : static_cast<uint16_t>(0),
                    probeSession));
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

void RunLanDiscovery(const Options& options)
{
    const auto timeout = std::chrono::seconds(options.lanDiscoverSeconds);
    std::cout
        << "Discovering LAN receivers for " << options.lanDiscoverSeconds
        << " seconds on UDP port " << options.lanDiscoveryPort << "...\n";

    const auto peers = screenshare::DiscoverLanPeers(timeout, options.lanDiscoveryPort);
    if (peers.empty()) {
        std::cout
            << "No LAN receivers found. Start the receiver with --watch PORT --lan-advertise "
            << "or enable LAN discoverable in the UI.\n";
        return;
    }

    for (const auto& peer : peers) {
        const std::string target = peer.address + ":" + std::to_string(peer.sharePort);
        std::cout
            << "receiver name=\"" << peer.name << "\""
            << " address=" << peer.address
            << " port=" << peer.sharePort
            << " session=" << (peer.sessionId.empty() ? "unknown" : peer.sessionId)
            << " fingerprint=" << (peer.sessionFingerprint == 0 ? "unknown" : FormatSessionFingerprint(peer.sessionFingerprint))
            << " security=" << (peer.accessCodeFingerprint == 0 ? "plaintext" : "encrypted")
            << " access_fingerprint=" << (peer.accessCodeFingerprint == 0 ? "none" : FormatSessionFingerprint(peer.accessCodeFingerprint))
            << "\n"
            << "share_target=" << target << "\n"
            << "command=ScreenShare --share " << target;
        if (!peer.sessionId.empty()) {
            std::cout << " --session " << peer.sessionId;
        }
        if (peer.accessCodeFingerprint == 0) {
            std::cout << " --allow-plaintext";
        } else {
            std::cout << " --access-code CODE";
        }
        std::cout << "\n";
    }
}

void RunStunQuery(const Options& options)
{
    screenshare::StunQueryConfig config;
    config.server = options.stunServer;
    config.timeout = std::chrono::milliseconds(options.stunTimeoutMs);

    std::cout
        << "Querying STUN server " << config.server.host << ":" << config.server.port
        << " with timeout " << options.stunTimeoutMs << " ms...\n";

    const auto result = screenshare::QueryPublicUdpEndpoint(config);
    std::cout
        << "stun_server=" << result.serverAddress << ":" << result.serverPort << "\n"
        << "local_udp_endpoint=" << result.localAddress << ":" << result.localPort << "\n"
        << "public_udp_endpoint=" << result.publicAddress << ":" << result.publicPort << "\n"
        << "manual_invite_endpoint=" << result.publicAddress << ":" << result.publicPort << "\n";
}

void RunMakeInvite(const Options& options)
{
    screenshare::StunQueryConfig config;
    config.server = options.stunServer;
    config.timeout = std::chrono::milliseconds(options.stunTimeoutMs);
    config.localPort = options.inviteLocalPort;

    std::cout
        << "Creating NAT invite from local UDP port " << options.inviteLocalPort
        << " via STUN server " << config.server.host << ":" << config.server.port
        << " with timeout " << options.stunTimeoutMs << " ms...\n";

    const auto result = screenshare::QueryPublicUdpEndpoint(config);
    const std::time_t expiresAt =
        std::time(nullptr) + static_cast<std::time_t>(options.inviteTtlSeconds);
    const std::string security = options.accessCodeProvided ? "encrypted" : "plaintext";

    screenshare::NatInvite invite;
    invite.publicEndpoint = screenshare::NatInviteEndpoint{result.publicAddress, result.publicPort};
    invite.localEndpoint = screenshare::NatInviteEndpoint{result.localAddress, result.localPort};
    invite.stunEndpoint = screenshare::NatInviteEndpoint{result.serverAddress, result.serverPort};
    invite.sessionId = options.sessionId;
    invite.sessionFingerprint = options.sessionFingerprint;
    invite.encrypted = options.accessCodeProvided;
    invite.accessCodeFingerprint = options.accessCodeFingerprint;
    invite.expiresUnix = static_cast<int64_t>(expiresAt);

    const std::string inviteLine = screenshare::FormatNatInvite(invite, options.accessCodeKey);
    const std::string securityOption = options.accessCodeProvided ? "--access-code CODE" : "--allow-plaintext";
    const std::string peerInvitePlaceholder = PowerShellQuote("<PEER_INVITE>");
    const std::string localInviteArgument = PowerShellQuote(inviteLine);

    std::cout
        << "stun_server=" << result.serverAddress << ":" << result.serverPort << "\n"
        << "local_udp_endpoint=" << result.localAddress << ":" << result.localPort << "\n"
        << "public_udp_endpoint=" << result.publicAddress << ":" << result.publicPort << "\n"
        << "invite_security=" << security << "\n"
        << "invite_format=" << (options.accessCodeProvided ? "compact-encrypted" : "compact-plaintext") << "\n"
        << "invite_expires_unix=" << static_cast<long long>(expiresAt) << "\n"
        << inviteLine << "\n"
        << "send_this_invite_to_peer=" << inviteLine << "\n"
        << "peer_invite_placeholder=<PEER_INVITE>\n"
        << "watch_command_template=.\\ScreenShare.exe --watch " << options.inviteLocalPort
        << " --peer-invite " << peerInvitePlaceholder << " " << securityOption << "\n"
        << "share_command_template=.\\ScreenShare.exe --share " << peerInvitePlaceholder
        << " --local-invite " << localInviteArgument << " " << securityOption << "\n"
        << "probe_command_template=.\\ScreenShare.exe --nat-probe " << options.inviteLocalPort
        << " --peer-invite " << peerInvitePlaceholder << " " << securityOption << "\n";
    if (options.accessCodeProvided) {
        std::cout << "template_note=replace CODE with the same access code used to create this invite\n";
    }
    std::cout << "template_note=replace <PEER_INVITE> with the invite copied from your friend\n";
}

std::string FormatNatEndpoint(const screenshare::NatInviteEndpoint& endpoint)
{
    if (endpoint.host.empty() || endpoint.port == 0) {
        return "none";
    }
    return endpoint.host + ":" + std::to_string(endpoint.port);
}

screenshare::NatInvite ParseValidatedPeerInvite(const Options& options)
{
    const auto invite = screenshare::ParseNatInvite(options.peerInvite, options.accessCodeKey);
    const std::time_t now = std::time(nullptr);
    if (invite.expiresUnix > 0 && invite.expiresUnix < static_cast<int64_t>(now)) {
        std::cerr
            << "Warning: peer invite expired at Unix time " << invite.expiresUnix
            << "; probing anyway because clocks can differ.\n";
    }

    if (invite.encrypted) {
        if (!options.accessCodeProvided) {
            throw std::invalid_argument("Peer invite requires --access-code CODE");
        }
        if (options.accessCodeFingerprint != invite.accessCodeFingerprint) {
            throw std::invalid_argument("Access code does not match the peer invite fingerprint");
        }
    } else if (!options.allowPlaintext) {
        throw std::invalid_argument("Peer invite is plaintext; rerun with --allow-plaintext");
    }

    if (options.sessionIdProvided && options.sessionFingerprint != invite.sessionFingerprint) {
        std::cerr
            << "Warning: --session fingerprint " << FormatSessionFingerprint(options.sessionFingerprint)
            << " does not match peer invite session fingerprint "
            << FormatSessionFingerprint(invite.sessionFingerprint) << ".\n";
    }

    return invite;
}

void RunNatProbe(const Options& options)
{
    const auto invite = ParseValidatedPeerInvite(options);

    screenshare::NatProbeConfig config;
    config.localPort = options.natProbeLocalPort;
    config.peerInvite = invite;
    config.duration = std::chrono::seconds(options.seconds);
    config.interval = std::chrono::milliseconds(options.natProbeIntervalMs);
    config.sessionFingerprint = options.sessionIdProvided ? options.sessionFingerprint : 0;
    config.accessCodeFingerprint = options.accessCodeFingerprint;

    std::cout
        << "Running NAT UDP probe from local port " << options.natProbeLocalPort
        << " for " << options.seconds << " seconds"
        << " every " << options.natProbeIntervalMs << " ms...\n"
        << "peer_public_endpoint=" << FormatNatEndpoint(invite.publicEndpoint) << "\n"
        << "peer_local_endpoint=" << FormatNatEndpoint(invite.localEndpoint) << "\n"
        << "peer_session=" << invite.sessionId << "\n"
        << "peer_session_fingerprint=" << FormatSessionFingerprint(invite.sessionFingerprint) << "\n"
        << "peer_security=" << (invite.encrypted ? "encrypted" : "plaintext") << "\n";

    const auto stats = screenshare::RunNatProbeExchange(config);
    const bool reachable = stats.receivedPackets > 0;
    std::cout
        << "nat_probe_result=" << (reachable ? "reachable" : "no_response") << "\n"
        << "nat_probe_sent_public=" << stats.sentPublicProbes << "\n"
        << "nat_probe_sent_local=" << stats.sentLocalProbes << "\n"
        << "nat_probe_received=" << stats.receivedPackets << "\n"
        << "nat_probe_received_probes=" << stats.receivedProbes << "\n"
        << "nat_probe_received_replies=" << stats.receivedReplies << "\n"
        << "nat_probe_replies_sent=" << stats.repliesSent << "\n"
        << "nat_probe_invalid_packets=" << stats.invalidPackets << "\n"
        << "nat_probe_session_mismatches=" << stats.sessionMismatches << "\n"
        << "nat_probe_access_mismatches=" << stats.accessCodeMismatches << "\n";
    for (const auto& endpoint : stats.seenEndpoints) {
        std::cout << "nat_probe_seen_endpoint=" << endpoint.address << ":" << endpoint.port << "\n";
    }
}

void RunUdpReceiverStats(const Options& options)
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

    auto drainLiveSignalingProbeTargets = [&]() {
        if (!liveSignalingRuntime) {
            return 0;
        }

        int added = 0;
        for (const auto& peer : liveSignalingRuntime->DrainDiscoveredPeers()) {
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
                << " endpoint=" << SignalingCandidateEndpoint(peer.candidate)
                << "\n";
        }
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
    if (options.previewWindow) {
        previewWindow = std::make_unique<screenshare::ReceiverPreviewWindow>();
        previewWindow->SetStatusText("waiting | res 0x0 | fps 0.0 | lat 0/0ms | q 0/0/0 | resync 0 | skip 0 | drops 0/0 | reset 0 | shown 0 | av wait");
        previewWindow->Show();
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

    if (previewWindow) {
        screenshare::ReceiverPreviewControlCallbacks callbacks;
        callbacks.toggleAudioMute = toggleAudioMute;
        callbacks.adjustAudioVolumePercent = adjustAudioVolume;
        previewWindow->SetControlCallbacks(std::move(callbacks));
        updatePreviewTitle();
    }

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
            latestDecodedFrame = decodedFrame;
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
        auto decodedFrames = h264Decoder->DecodePacket(packet);
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

    auto shouldContinue = [&]() {
        if (previewWindow && !previewWindow->PumpMessages()) {
            return false;
        }
        if (StopRequested(options)) {
            return false;
        }
        if (options.previewWindow && options.seconds == 0) {
            return true;
        }
        return Clock::now() - startedAt < std::chrono::seconds(options.seconds);
    };

    while (shouldContinue()) {
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
                << " h264_decoded_frames=" << h264DecodedFrames
                << " h264_decoded_bytes=" << h264DecodedBytes
                << " h264_decode_resyncs=" << h264DecodeResyncs
                << " h264_decode_restarts=" << h264DecodeDecoderRestarts
                << " h264_decode_skipped_packets=" << h264DecodeSkippedPackets
                << " h264_decoded_output=" << h264DecodedWidth << "x" << h264DecodedHeight
                << " pending_h264_decode_packets=" << h264DecodeBacklog.size()
                << " preview_frames_presented=" << (previewWindow ? previewWindow->framesPresented() : 0)
                << " preview_queue=" << (previewWindow ? previewPlayout.queuedFrameCount() : 0)
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
        }
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
        << ", H.264 decoded frames: " << h264DecodedFrames
        << ", H.264 decoded bytes: " << h264DecodedBytes
        << ", H.264 decode resyncs: " << h264DecodeResyncs
        << ", H.264 decoder restarts: " << h264DecodeDecoderRestarts
        << ", H.264 decode skipped packets: " << h264DecodeSkippedPackets
        << ", H.264 decoded output: " << h264DecodedWidth << "x" << h264DecodedHeight
        << ", pending H.264 decode packets: " << h264DecodeBacklog.size()
        << ", preview frames presented: " << (previewWindow ? previewWindow->framesPresented() : 0)
        << ", preview queued frames: " << (previewWindow ? previewPlayout.queuedFrameCount() : 0)
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

int main(int argc, char** argv)
{
    std::unique_ptr<ScopedLogRedirect> logRedirect;
    const auto saveReportPath = FindPathArgument(argc, argv, "--save-report");
    const auto explicitLogPath = FindPathArgument(argc, argv, "--log");
    std::optional<std::filesystem::path> capturedLogPath;
    bool capturedLogIsTemporary = false;
    SavedReportContext reportContext;
    reportContext.sessionId = GenerateSessionId();
    reportContext.sessionFingerprint = SessionFingerprint(reportContext.sessionId);
    int exitCode = 0;

    try {
        if (explicitLogPath) {
            capturedLogPath = *explicitLogPath;
            logRedirect = std::make_unique<ScopedLogRedirect>(*capturedLogPath);
        } else if (saveReportPath) {
            capturedLogPath = TemporaryReportLogPath(*saveReportPath);
            capturedLogIsTemporary = true;
            RemoveFileIfExists(*capturedLogPath);
            logRedirect = std::make_unique<ScopedLogRedirect>(*capturedLogPath, false);
            std::cout << "Saving run report to " << saveReportPath->string() << "\n";
        }

        Options options = ParseOptions(argc, argv, reportContext.sessionId);
        PrepareLiveSignaling(options);
        reportContext.sessionId = options.sessionId;
        reportContext.sessionFingerprint = options.sessionFingerprint;
        reportContext.accessCodeRequired = options.accessCodeProvided;
        reportContext.encryptionEnabled = options.accessCodeKey.has_value();

        if (options.generateAccessCode) {
            std::cout << screenshare::GenerateUdpAccessCode() << "\n";
            exitCode = 0;
        } else if (options.makeInvite) {
            RunMakeInvite(options);
            exitCode = 0;
        } else if (options.natProbe) {
            RunNatProbe(options);
            exitCode = 0;
        } else if (HasSignalingCommand(options)) {
            RunSignaling(options);
            exitCode = 0;
        } else if (options.stunQuery) {
            RunStunQuery(options);
            exitCode = 0;
        } else if (options.lanDiscover) {
            RunLanDiscovery(options);
            exitCode = 0;
        } else if (options.listDisplays) {
            PrintDisplays();
            exitCode = 0;
        } else if (options.listH264Encoders) {
            PrintH264Encoders(options);
            exitCode = 0;
        } else if (options.listAudioDevices) {
            PrintAudioDevices();
            exitCode = 0;
        } else if (options.audioCapture && options.udpSendTarget.empty() && !options.shareRoom) {
            RunAudioCaptureStats(options, reportContext);
            exitCode = 0;
        } else if (options.udpReceivePort != 0) {
            RunUdpReceiverStats(options);
            exitCode = 0;
        } else {
            RunCaptureStats(options, reportContext);
            exitCode = 0;
        }
    } catch (const std::invalid_argument& error) {
        std::cerr << "Error: " << error.what() << "\n\n";
        PrintHelp();
        exitCode = 1;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        exitCode = 1;
    }

    logRedirect.reset();

    if (saveReportPath) {
        try {
            WriteSavedReport(
                *saveReportPath,
                capturedLogPath,
                argv[0],
                argc,
                argv,
                reportContext,
                exitCode);
            std::cout << "Run report saved to " << saveReportPath->string() << "\n";
        } catch (const std::exception& error) {
            std::cerr << "Failed to save run report: " << error.what() << "\n";
            exitCode = 1;
        }
    }

    if (capturedLogIsTemporary && capturedLogPath) {
        RemoveFileIfExists(*capturedLogPath);
    }

    return exitCode;
}
