#include "core/SessionCommand.h"

#include <cstdint>
#include <string>
#include <vector>

namespace screenshare {
namespace {

void AddOption(std::vector<std::string>& args, const char* name, const std::string& value)
{
    args.emplace_back(name);
    args.push_back(value);
}

void AddOption(std::vector<std::string>& args, const char* name, int value)
{
    AddOption(args, name, std::to_string(value));
}

std::string BitrateMbps(uint32_t bitrateBps)
{
    if (bitrateBps == 0) {
        return {};
    }

    const uint64_t roundedThousandths = (static_cast<uint64_t>(bitrateBps) + 500) / 1000;
    const uint64_t whole = roundedThousandths / 1000;
    const uint64_t fractionValue = roundedThousandths % 1000;
    if (fractionValue == 0) {
        return std::to_string(whole);
    }

    std::string fraction = std::to_string(fractionValue);
    while (fraction.size() < 3) {
        fraction.insert(fraction.begin(), '0');
    }
    while (!fraction.empty() && fraction.back() == '0') {
        fraction.pop_back();
    }
    return std::to_string(whole) + "." + fraction;
}

void AddCommonRoomArguments(
    std::vector<std::string>& args,
    const std::string& signalingStunServer,
    const std::string& udpAccessCode,
    bool allowPlaintext,
    const std::string& reportPath)
{
    if (!signalingStunServer.empty()) {
        AddOption(args, "--signal-stun", signalingStunServer);
    }
    if (!udpAccessCode.empty()) {
        AddOption(args, "--access-code", udpAccessCode);
    } else if (allowPlaintext) {
        args.emplace_back("--allow-plaintext");
    }
    if (!reportPath.empty()) {
        AddOption(args, "--save-report", reportPath);
    }
}

} // namespace

std::vector<std::string> BuildShareRoomArguments(const ShareSessionConfig& config)
{
    std::vector<std::string> args;
    AddOption(args, "--share-room", static_cast<int>(config.roomPort));
    AddOption(args, "--signal-room", config.roomId);
    if (!config.roomName.empty()) {
        AddOption(args, "--signal-room-name", config.roomName);
    }
    if (!config.roomPassword.empty()) {
        AddOption(args, "--signal-room-password", config.roomPassword);
    }

    AddOption(args, "--display", config.displayIndex);
    AddOption(args, "--fps", config.stream.fps);

    if (!config.stream.adaptResolution) {
        args.emplace_back("--no-adapt-resolution");
    }
    if (config.stream.outputResolution &&
        config.stream.outputResolution->width > 0 &&
        config.stream.outputResolution->height > 0) {
        AddOption(args, "--width", config.stream.outputResolution->width);
        AddOption(args, "--height", config.stream.outputResolution->height);
    }
    const std::string bitrate = BitrateMbps(config.stream.bitrateBps);
    if (!bitrate.empty()) {
        AddOption(args, "--bitrate-mbps", bitrate);
    }
    if (!config.audioDeviceId.empty()) {
        AddOption(args, "--audio-device-id", config.audioDeviceId);
    }

    AddCommonRoomArguments(
        args,
        config.signalingStunServer,
        config.udpAccessCode,
        config.allowPlaintext,
        config.reportPath);
    return args;
}

std::vector<std::string> BuildWatchRoomArguments(const WatchSessionConfig& config)
{
    std::vector<std::string> args;
    if (config.playAudio) {
        AddOption(args, "--watch", static_cast<int>(config.listenPort));
    } else {
        AddOption(args, "--udp-recv", static_cast<int>(config.listenPort));
        args.emplace_back("--preview");
    }
    AddOption(args, "--signal-room", config.roomId);
    if (!config.roomPassword.empty()) {
        AddOption(args, "--signal-room-password", config.roomPassword);
    }
    AddOption(args, "--preview-latency-ms", config.previewLatencyMs);
    if (config.playAudio) {
        AddOption(args, "--audio-playback-volume", config.audioPlaybackVolumePercent);
        if (config.muted) {
            args.emplace_back("--audio-playback-muted");
        }
    }

    AddCommonRoomArguments(
        args,
        config.signalingStunServer,
        config.udpAccessCode,
        config.allowPlaintext,
        config.reportPath);
    return args;
}

} // namespace screenshare
