#include "cli/ScreenShareCLI.h"

#include "runtime/ScreenShareRuntimeInternal.h"

#include "audio/WasapiCapture.h"
#include "capture/DesktopCapturer.h"
#include "codec/H264EncoderProbe.h"
#include "transport/LanDiscovery.h"
#include "transport/NatTraversal.h"
#include "transport/SignalingClient.h"
#include "transport/StunClient.h"
#include "transport/UdpReceiver.h"
#include "transport/UdpSender.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
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

void PrintHelp()
{
    std::cout
        << "ScreenShare native C++ capture prototype\n\n"
        << "Usage:\n"
        << "  ScreenShare --list\n"
        << "  ScreenShare --self-test [--save-report PATH]\n"
        << "  ScreenShare --generate-access-code\n"
        << "  ScreenShare --list-h264-encoders [--width W --height H] [--fps FPS] [--bitrate-mbps Mbps]\n"
        << "  ScreenShare --list-audio-devices\n"
        << "  ScreenShare --share HOST:PORT|INVITE [--display N] [--seconds S]\n"
        << "              [--share-target HOST:PORT|WATCHER_INVITE]\n"
        << "              [--invite-endpoint auto|public|local]\n"
        << "              [--local-invite INVITE]\n"
        << "  ScreenShare --share-room PORT --signal-room ROOM [--signal-room-name NAME]\n"
        << "              [--signal-room-password PASSWORD] [--signal-server URL] [--seconds S]\n"
        << "  ScreenShare --watch PORT [--seconds S] [--peer-invite INVITE]\n"
        << "              [--signal-room ROOM] [--signal-room-password PASSWORD] [--signal-server URL]\n"
        << "  ScreenShare --lan-discover [--lan-discover-seconds S]\n"
        << "  ScreenShare --stun HOST[:PORT] [--stun-timeout-ms MS]\n"
        << "  ScreenShare --make-invite PORT --stun HOST[:PORT] [--invite-ttl-seconds S]\n"
        << "  ScreenShare --nat-probe PORT --peer-invite INVITE [--seconds S]\n"
        << "              [--nat-probe-interval-ms MS]\n"
        << "  ScreenShare --signal-health URL [--signal-timeout-ms MS]\n"
        << "  ScreenShare --signal-join URL --signal-room ROOM --signal-peer-id PEER\n"
        << "              --signal-candidate IP:PORT [--signal-name NAME] [--signal-room-name NAME]\n"
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
        << "              [--adapt-resolution|--no-adapt-resolution] [--adapt-resolution-min-scale N]\n"
        << "              [--adapt-resolution-cooldown S]\n"
        << "              [--control-file PATH]\n"
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
        << "             Optional --signal-room-password is verified by the Worker over HTTPS and mixed locally into the UDP encryption key.\n"
        << "  Presets: --share enables UDP video, system audio, and adaptation; --watch enables preview and audio playback.\n\n"
        << "Examples:\n"
        << "  ScreenShare --list\n"
        << "  ScreenShare --self-test --save-report self-test-report.zip\n"
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
        << "  ScreenShare --share-room 5001 --signal-room room1 --signal-room-name \"Movie night\"\n"
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

std::string ParseSignalingRoomName(const char* value)
{
    const std::string roomName = value != nullptr ? value : "";
    if (roomName.empty() || roomName.size() > 80) {
        throw std::invalid_argument("--signal-room-name must be between 1 and 80 bytes");
    }
    for (const char ch : roomName) {
        const unsigned char valueByte = static_cast<unsigned char>(ch);
        if (valueByte < 32U || valueByte == 127U) {
            throw std::invalid_argument("--signal-room-name may not contain control characters");
        }
    }
    return roomName;
}

std::string ParseSignalingRoomPassword(const char* value)
{
    const std::string password = value != nullptr ? value : "";
    if (password.empty() || password.size() > 128) {
        throw std::invalid_argument("--signal-room-password must be between 1 and 128 bytes");
    }
    for (const char ch : password) {
        const unsigned char valueByte = static_cast<unsigned char>(ch);
        if (valueByte < 32U || valueByte == 127U) {
            throw std::invalid_argument("--signal-room-password may not contain control characters");
        }
    }
    return password;
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

std::string FormatStunServerTarget(const screenshare::StunServerTarget& target)
{
    if (target.host.empty()) {
        return {};
    }
    return target.host + ":" + std::to_string(target.port);
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
    std::string accessCodeText;

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
        if (arg == "--self-test") {
            options.selfTest = true;
        } else if (arg == "--log") {
            options.logPath = requireValue("--log");
        } else if (arg == "--stop-file") {
            options.stopFilePath = requireValue("--stop-file");
        } else if (arg == "--control-file") {
            options.controlFilePath = requireValue("--control-file");
        } else if (arg == "--session" || arg == "--session-id") {
            options.sessionId = ParseSessionId(requireValue(arg.c_str()));
            options.sessionIdProvided = true;
        } else if (arg == "--generate-access-code") {
            options.generateAccessCode = true;
        } else if (arg == "--allow-plaintext") {
            options.allowPlaintext = true;
        } else if (arg == "--access-code" || arg == "--session-code") {
            const std::string accessCode = ParseAccessCode(requireValue(arg.c_str()));
            accessCodeText = accessCode;
            options.accessCodeFingerprint = screenshare::UdpAccessCodeFingerprint(accessCode);
            options.accessCodeKey = screenshare::DeriveUdpCryptoKey(accessCode);
            options.accessCodeProvided = true;
        } else if (arg.rfind(accessCodePrefix, 0) == 0 || arg.rfind(sessionCodePrefix, 0) == 0) {
            const size_t valueOffset =
                arg.rfind(accessCodePrefix, 0) == 0 ? accessCodePrefix.size() : sessionCodePrefix.size();
            const std::string accessCode = ParseAccessCode(arg.c_str() + valueOffset);
            accessCodeText = accessCode;
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
        } else if (arg == "--peer-token") {
            options.signalingPeerToken = requireValue("--peer-token");
        } else if (arg == "--signal-candidate") {
            options.signalingCandidate = requireValue("--signal-candidate");
        } else if (arg == "--signal-name") {
            options.signalingName = requireValue("--signal-name");
        } else if (arg == "--signal-platform") {
            options.signalingPlatform = requireValue("--signal-platform");
        } else if (arg == "--signal-room-name") {
            options.signalingRoomName = ParseSignalingRoomName(requireValue("--signal-room-name"));
        } else if (arg == "--signal-room-password") {
            options.signalingRoomPassword = ParseSignalingRoomPassword(requireValue("--signal-room-password"));
        } else if (arg == "--signal-stun") {
            options.signalingStunServer = screenshare::ParseStunServerTarget(requireValue("--signal-stun"));
            options.signalingStunServerProvided = true;
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
            options.adaptResolutionProvided = true;
            options.adaptBitrate = true;
        } else if (arg == "--no-adapt-resolution") {
            options.noAdaptResolution = true;
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
    if (options.noAdaptResolution) {
        if (options.adaptResolutionProvided ||
            options.adaptResolutionMinScaleProvided ||
            options.adaptResolutionCooldownProvided) {
            throw std::invalid_argument("--no-adapt-resolution cannot be combined with adaptive resolution options");
        }
        options.adaptResolution = false;
    }

    const auto fillCommonSessionConfig = [&](auto& config) {
        config.sessionId = options.sessionId;
        config.seconds = options.seconds;
        config.signalingServerUrl = options.signalingServerUrl;
        if (options.signalingStunServerProvided) {
            config.signalingStunServer = FormatStunServerTarget(options.signalingStunServer);
        }
        config.signalingTimeoutMs = options.signalingTimeoutMs;
        config.signalingSetupSeconds = options.signalingSetupSeconds;
        config.udpAccessCode = accessCodeText;
        config.allowPlaintext = options.allowPlaintext;
        config.reportPath = options.saveReportPath;
    };

    const auto fillShareStreamConfig = [&](screenshare::ShareSessionConfig& config) {
        config.displayIndex = options.displayIndex;
        config.udpLocalPort = options.udpLocalPortProvided ? options.udpLocalPort : 0;
        config.audioDeviceId = options.audioDeviceId;
        config.captureSystemAudio = options.audioCapture &&
            options.audioCaptureSource == screenshare::AudioCaptureSource::SystemOutput;
        config.stream.fps = options.fps;
        config.stream.bitrateBps = options.bitrate;
        config.stream.adaptBitrate = options.adaptBitrate;
        config.stream.adaptResolution = options.adaptResolution;
        if (options.width > 0 && options.height > 0) {
            config.stream.outputResolution = screenshare::SessionResolution{options.width, options.height};
        }
    };

    const bool shareHasUnsupportedTypedOption =
        options.streamEncoderPreferenceProvided ||
        options.udpPacingOptionProvided ||
        options.udpMaxQueueMsProvided ||
        options.adaptMinBitrateProvided ||
        options.adaptReduceCooldownProvided ||
        options.adaptResolutionMinScaleProvided ||
        options.adaptResolutionCooldownProvided ||
        options.keyframeIntervalProvided ||
        options.audioCodecProvided ||
        options.inviteEndpointPreferenceProvided ||
        !options.stopFilePath.empty() ||
        !options.controlFilePath.empty() ||
        options.captureBackend != screenshare::CaptureBackend::WindowsGraphicsCapture ||
        options.wgcBorderRequired ||
        !options.hdrToSdr ||
        options.hdrSdrWhiteNits != 203.0f ||
        options.hdrSdrBgraExposure != 0.88f ||
        (options.audioCapture && options.audioCaptureSource != screenshare::AudioCaptureSource::SystemOutput) ||
        std::any_of(extraShareTargets.begin(), extraShareTargets.end(), [](const ExtraShareTargetOption& target) {
            return !target.localInvite.empty();
        });

    if ((shareTarget || shareRoomPort) && !shareHasUnsupportedTypedOption) {
        screenshare::ShareSessionConfig config;
        fillCommonSessionConfig(config);
        fillShareStreamConfig(config);
        if (shareRoomPort) {
            config.connectionMode = screenshare::ShareConnectionMode::Room;
            config.roomPort = *shareRoomPort;
            config.roomId = options.signalingRoomId;
            config.roomName = options.signalingRoomName;
            config.roomPassword = options.signalingRoomPassword;
        } else if (shareTarget && (LooksLikeNatInvite(*shareTarget) || !options.localInvite.empty())) {
            config.connectionMode = screenshare::ShareConnectionMode::ManualInvite;
            config.localInvite = options.localInvite;
            config.watcherInvites.push_back(*shareTarget);
            for (const auto& target : extraShareTargets) {
                config.watcherInvites.push_back(target.target);
            }
        } else if (shareTarget) {
            config.connectionMode = screenshare::ShareConnectionMode::DirectTargets;
            config.targets.push_back(*shareTarget);
            for (const auto& target : extraShareTargets) {
                config.targets.push_back(target.target);
            }
        }
        options.cliShareSession = std::move(config);
    }

    const bool watchHasUnsupportedTypedOption =
        options.previewMaxLateProvided ||
        options.audioPlaybackLatencyProvided ||
        options.avSyncExplicit ||
        options.avSyncDisabled ||
        options.simulateLossProvided ||
        options.simulateJitterProvided ||
        options.natProbeIntervalProvided ||
        !options.stopFilePath.empty() ||
        !options.controlFilePath.empty() ||
        !options.h264DumpPath.empty() ||
        !options.decodedBmpPath.empty();

    if (watchPort && !watchHasUnsupportedTypedOption) {
        screenshare::WatchSessionConfig config;
        fillCommonSessionConfig(config);
        config.listenPort = *watchPort;
        config.playAudio = options.audioPlayback;
        config.muted = options.audioPlaybackMuted;
        config.previewLatencyMs =
            (!options.previewLatencyProvided && options.previewWindow && options.audioPlayback && !options.avSyncDisabled) ?
            DefaultAvSyncPreviewLatencyMs :
            options.previewLatencyMs;
        config.audioPlaybackVolumePercent = static_cast<int>(std::lround(options.audioPlaybackVolumePercent));
        if (!options.signalingRoomId.empty()) {
            config.connectionMode = screenshare::WatchConnectionMode::Room;
            config.roomId = options.signalingRoomId;
            config.roomPassword = options.signalingRoomPassword;
        } else if (!options.peerInvite.empty()) {
            config.connectionMode = screenshare::WatchConnectionMode::ManualInvite;
            config.peerInvite = options.peerInvite;
        } else {
            config.connectionMode = screenshare::WatchConnectionMode::DirectListen;
            config.lanAdvertise = options.lanAdvertise;
        }
        options.cliWatchSession = std::move(config);
    }

    if (options.selfTest) {
        if (options.generateAccessCode ||
            options.listDisplays ||
            options.listH264Encoders ||
            options.listAudioDevices ||
            options.lanDiscover ||
            options.lanAdvertise ||
            options.stunQuery ||
            options.makeInvite ||
            options.natProbe ||
            HasSignalingOptions(options) ||
            options.signalingLive ||
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
            options.noAdaptResolution ||
            options.adaptResolutionMinScaleProvided ||
            options.adaptResolutionCooldownProvided ||
            options.inviteEndpointPreferenceProvided ||
            !options.stopFilePath.empty() ||
            !options.controlFilePath.empty() ||
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
            throw std::invalid_argument("--self-test is a standalone diagnostic and can only be combined with --log, --save-report, or --session");
        }
        options.sessionFingerprint = SessionFingerprint(options.sessionId);
        return options;
    }

    if (options.generateAccessCode) {
        if (!options.logPath.empty() || !options.saveReportPath.empty()) {
            throw std::invalid_argument("--generate-access-code cannot be combined with --log or --save-report");
        }
        options.sessionFingerprint = SessionFingerprint(options.sessionId);
        return options;
    }

    if (HasSignalingOptions(options) && !HasSignalingCommand(options) && !options.signalingLive) {
        throw std::invalid_argument("--signal-room, --signal-peer-id, --signal-candidate, --signal-name, --signal-platform, --signal-room-name, --signal-room-password, --signal-stun, --signal-setup-seconds, and --signal-timeout-ms require --signal-server or a --signal-* command");
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
                !options.signalingPlatform.empty() || !options.signalingRoomName.empty() ||
                !options.signalingRoomPassword.empty() || options.signalingSetupSecondsProvided) {
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
                       !options.signalingPlatform.empty() || !options.signalingRoomName.empty() ||
                       (options.signalingCommand != SignalingCommand::Peers && !options.signalingRoomPassword.empty()) ||
                       options.signalingSetupSecondsProvided) {
                throw std::invalid_argument("--signal-candidate, --signal-name, --signal-platform, --signal-room-name, and --signal-setup-seconds are only used with --signal-join or --signal-server; --signal-room-password is also allowed with --signal-peers");
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
            options.noAdaptResolution ||
            options.adaptResolutionMinScaleProvided ||
            options.adaptResolutionCooldownProvided ||
            options.inviteEndpointPreferenceProvided ||
            !options.controlFilePath.empty() ||
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
        if (!options.signalingRoomPassword.empty() && (options.accessCodeProvided || options.allowPlaintext)) {
            throw std::invalid_argument("--signal-room-password cannot be combined with --access-code or --allow-plaintext");
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
    if (!options.controlFilePath.empty() && options.udpSendTarget.empty() && !options.shareRoom) {
        throw std::invalid_argument("--control-file requires --udp-send, --share, or --share-room");
    }
    if (options.udpMaxQueueMs < 0 || options.udpMaxQueueMs > 5000) {
        throw std::invalid_argument("--udp-max-queue-ms must be between 0 and 5000");
    }
    if ((options.adaptResolution || options.adaptResolutionMinScaleProvided || options.adaptResolutionCooldownProvided) &&
        options.udpSendTarget.empty() && !options.shareRoom) {
        throw std::invalid_argument("--adapt-resolution, --adapt-resolution-min-scale, and --adapt-resolution-cooldown require --udp-send or --share-room");
    }
    if (options.noAdaptResolution && options.udpSendTarget.empty() && !options.shareRoom) {
        throw std::invalid_argument("--no-adapt-resolution requires --udp-send, --share, or --share-room");
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
         options.adaptResolutionCooldownProvided || options.noAdaptResolution || !options.controlFilePath.empty() ||
         options.keyframeIntervalProvided)) {
        throw std::invalid_argument("--udp-recv cannot be combined with --list, --list-h264-encoders, --list-audio-devices, --audio-capture, --audio-device-id, --audio-send, --record, --dump-capture-bmp, --stream-encode, --stream-encoder, --udp-send, --udp-local-port, --no-udp-pacing, --udp-max-queue-ms, --adapt-bitrate, --adapt-min-bitrate-mbps, --adapt-reduce-cooldown, --adapt-resolution, --no-adapt-resolution, --adapt-resolution-min-scale, --adapt-resolution-cooldown, --control-file, or --keyframe-interval");
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
        << "signaling_room_name=\"" << screenshare::SignalingJsonEscape(response.roomName) << "\"\n"
        << "signaling_room_password=" << (response.passwordProtected ? "required" : "none") << "\n"
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
        peer.roomName = options.signalingRoomName;
        peer.roomPassword = options.signalingRoomPassword;
        peer.passwordProtected = !options.signalingRoomPassword.empty();
        const auto joinResponse = client.Join(
            options.signalingRoomId, peer, options.signalingPeerToken);
        // Print the issued peer token so it can be passed to --peer-token on
        // subsequent --signal-peers/--signal-heartbeat/--signal-leave calls.
        if (!joinResponse.peerToken.empty()) {
            std::cout << "signaling_peer_token=" << joinResponse.peerToken << "\n";
        }
        PrintSignalingPeers(joinResponse);
        break;
    }
    case SignalingCommand::Peers:
        PrintSignalingPeers(client.Peers(
            options.signalingRoomId,
            options.signalingPeerId,
            options.signalingPeerToken));
        break;
    case SignalingCommand::Heartbeat:
        client.Heartbeat(options.signalingRoomId, options.signalingPeerId, options.signalingPeerToken);
        std::cout << "signaling_heartbeat=ok\n";
        break;
    case SignalingCommand::Leave:
        client.Leave(options.signalingRoomId, options.signalingPeerId, options.signalingPeerToken);
        std::cout << "signaling_leave=ok\n";
        break;
    case SignalingCommand::None:
        throw std::logic_error("RunSignaling called without a signaling command");
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

void RunCliSelfTest(const Options& options)
{
    const std::string fingerprint = FormatSessionFingerprint(options.sessionFingerprint);
    std::cout
        << "cli_self_test=started"
        << " session=" << options.sessionId
        << " session_fingerprint=" << fingerprint
        << "\n";

    std::cout
        << "source=2560x1440"
        << " session=" << options.sessionId
        << " session_fingerprint=" << fingerprint
        << " source_format=DXGI_FORMAT_B8G8R8A8_UNORM"
        << " display_color_space=DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709"
        << " display_hdr=no"
        << " color_conversion=sdr"
        << " output=1920x1080"
        << " output_format=DXGI_FORMAT_B8G8R8A8_UNORM"
        << " resolution_scale=0.75"
        << " resolution_tier=1"
        << " resolution_tiers=3"
        << " resolution_adaptation=stable"
        << " nv12=gpu_texture"
        << " stream_input=d3d11"
        << " video_paused=no"
        << " stream_bitrate_mbps=16"
        << " stream_queue=1"
        << " stream_dropped=0"
        << " output_fps=60"
        << " desktop_update_fps=58"
        << " capture_avg_ms=1.2"
        << " stream_encode_avg_ms=3.4"
        << " repeated_frames=0"
        << " total_output_frames=60"
        << " total_desktop_updates=58"
        << " stream_encoded_frames=60"
        << " udp_targets=1"
        << " udp_active_targets=1"
        << " udp_failed_targets=0"
        << " udp_datagrams=900"
        << " udp_queued=900"
        << " udp_pending=12"
        << " udp_peak_pending=18"
        << " udp_queue_ms=14"
        << " udp_peak_queue_ms=24"
        << " udp_dropped_frames=0"
        << " audio_capture=running"
        << " audio_capture_source=system"
        << " audio_codec=opus"
        << " audio_payload_bitrate_mbps=0.128"
        << " audio_udp_packets=50"
        << " audio_udp_frames=48000"
        << " audio_udp_dropped_packets=0"
        << " udp_feedback_packets=4"
        << " udp_feedback_health=ok"
        << " udp_feedback_completed_frames=55"
        << " udp_feedback_resyncs=0"
        << " udp_feedback_skipped_packets=0"
        << "\n";

    std::cout
        << "viewer_target=0"
        << " viewer_group=1"
        << " viewer_endpoint=127.0.0.1:5000"
        << " viewer_name=self-test"
        << " viewer_state=feedback"
        << " viewer_feedback_packets=4"
        << " viewer_pending=8"
        << " viewer_queue_ms=10"
        << " viewer_feedback_health=ok"
        << " viewer_feedback_completed_frames=55"
        << " viewer_feedback_resyncs=0"
        << " viewer_feedback_session=" << fingerprint
        << " viewer_feedback_access=none"
        << "\n";

    std::cout
        << "udp_datagrams=920"
        << " session=" << options.sessionId
        << " session_fingerprint=" << fingerprint
        << " receiver_health=ok"
        << " nat_status=receiving"
        << " nat_hint=none"
        << " udp_datagrams_per_second=920"
        << " accepted_datagrams=920"
        << " feedback_sent=4"
        << " stream_restarts=0"
        << " audio_datagrams=50"
        << " audio_packets=50"
        << " audio_queued_packets=50"
        << " audio_queue_dropped=0"
        << " audio_frames=48000"
        << " audio_codec=opus"
        << " audio_playback=running"
        << " audio_playback_latency_ms=100"
        << " audio_playback_queue=6"
        << " audio_playback_queue_ms=96"
        << " audio_playback_packets=44"
        << " audio_playback_frames=42240"
        << " audio_playback_drops=0"
        << " audio_playback_latency_drops=0"
        << " audio_render_padding=384"
        << " av_sync=synced"
        << " av_audio_ahead_ms=2"
        << " invalid_datagrams=0"
        << " duplicate_fragments=0"
        << " completed_frames=55"
        << " completed_fps=59.4"
        << " pending_frames=1"
        << " incomplete_dropped=0"
        << " h264_decode_packets=55"
        << " h264_decode_avg_ms=1.1"
        << " h264_decoded_frames=55"
        << " h264_decoded_output=1920x1080"
        << " pending_h264_decode_packets=0"
        << " preview_frames_presented=54"
        << " preview_queue=2"
        << " video_playout_delay_avg_ms=104"
        << " video_playout_delay_max_ms=120"
        << " video_playout_delay_last_ms=101"
        << " preview_latency_ms=100"
        << " preview_max_late_ms=500"
        << " preview_late_drops=0"
        << " preview_overflow_drops=0"
        << " preview_startup_drops=0"
        << " preview_catchup_drops=2"
        << " latest_frame=55"
        << " latest_frame_bytes=60000"
        << " latest_fragments=48"
        << "\n";

    std::cout
        << "cli_self_test=passed"
        << " session=" << options.sessionId
        << " session_fingerprint=" << fingerprint
        << "\n";
}

} // namespace

namespace screenshare_runtime_internal {

int ExecuteScreenShareOptions(
    Options& options,
    SavedReportContext& reportContext,
    const ScreenShareRunContext& context)
{
    reportContext.sessionId = options.sessionId;
    reportContext.sessionFingerprint = options.sessionFingerprint;
    reportContext.accessCodeRequired = options.accessCodeProvided;
    reportContext.encryptionEnabled = options.accessCodeKey.has_value();

    if (options.selfTest) {
        RunCliSelfTest(options);
    } else if (options.generateAccessCode) {
        std::cout << screenshare::GenerateUdpAccessCode() << "\n";
    } else if (options.makeInvite) {
        RunMakeInvite(options);
    } else if (options.natProbe) {
        RunNatProbe(options);
    } else if (HasSignalingCommand(options)) {
        RunSignaling(options);
    } else if (options.stunQuery) {
        RunStunQuery(options);
    } else if (options.lanDiscover) {
        RunLanDiscovery(options);
    } else if (options.listDisplays) {
        PrintDisplays();
    } else if (options.listH264Encoders) {
        PrintH264Encoders(options);
    } else if (options.listAudioDevices) {
        PrintAudioDevices();
    } else {
        return ExecuteSessionRuntimeOptions(options, reportContext, context);
    }
    return 0;
}

} // namespace screenshare_runtime_internal


int RunScreenShareCli(int argc, char** argv)
{
    return RunScreenShareCli(argc, argv, ScreenShareRunContext{});
}

int RunScreenShareCli(const std::vector<std::string>& arguments)
{
    return RunScreenShareCli(arguments, ScreenShareRunContext{});
}

int RunScreenShareCli(const std::vector<std::string>& arguments, const ScreenShareRunContext& context)
{
    std::vector<std::string> normalizedArguments = arguments;
    std::vector<char*> argv = screenshare_runtime_internal::MutableArgv(normalizedArguments);

    return RunScreenShareCli(static_cast<int>(argv.size()), argv.data(), context);
}

int RunScreenShareCli(int argc, char** argv, const ScreenShareRunContext& context)
{
    std::unique_ptr<ScopedCallbackLogRedirect> callbackLogRedirect;
    if (context.outputHandler) {
        callbackLogRedirect = std::make_unique<ScopedCallbackLogRedirect>(context.outputHandler);
    }
    std::unique_ptr<ScopedLogRedirect> logRedirect;
    const auto saveReportPath = FindPathArgument(argc, argv, "--save-report");
    const auto explicitLogPath = FindPathArgument(argc, argv, "--log");
    std::optional<std::filesystem::path> capturedLogPath;
    bool capturedLogIsTemporary = false;
    screenshare_runtime_internal::SavedReportContext reportContext;
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
        if (options.cliShareSession) {
            exitCode = screenshare_runtime_internal::ExecuteShareSessionConfig(
                *options.cliShareSession,
                reportContext,
                context);
        } else if (options.cliWatchSession) {
            exitCode = screenshare_runtime_internal::ExecuteWatchSessionConfig(
                *options.cliWatchSession,
                reportContext,
                context);
        } else {
            exitCode = screenshare_runtime_internal::ExecuteScreenShareOptions(options, reportContext, context);
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
