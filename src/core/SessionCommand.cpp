#include "core/SessionCommand.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
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

void AddCommonSessionArguments(
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

void Require(bool condition, const char* message)
{
    if (!condition) {
        throw std::invalid_argument(message);
    }
}

} // namespace

std::vector<std::string> BuildShareArguments(const ShareSessionConfig& config)
{
    std::vector<std::string> args;
    switch (config.connectionMode) {
    case ShareConnectionMode::Room:
        Require(config.roomPort > 0, "Share room port is required.");
        Require(!config.roomId.empty(), "Share room ID is required.");
        AddOption(args, "--share-room", static_cast<int>(config.roomPort));
        AddOption(args, "--signal-room", config.roomId);
        if (!config.roomName.empty()) {
            AddOption(args, "--signal-room-name", config.roomName);
        }
        if (!config.roomPassword.empty()) {
            AddOption(args, "--signal-room-password", config.roomPassword);
        }
        break;
    case ShareConnectionMode::DirectTargets:
        Require(!config.targets.empty(), "At least one Share target is required.");
        if (!config.targets.empty()) {
            AddOption(args, "--share", config.targets.front());
            for (size_t index = 1; index < config.targets.size(); ++index) {
                AddOption(args, "--share-target", config.targets[index]);
            }
        }
        break;
    case ShareConnectionMode::ManualInvite: {
        const std::string firstTarget =
            config.watcherInvites.empty() ? config.localInvite : config.watcherInvites.front();
        Require(!firstTarget.empty(), "A room invite or watcher invite is required.");
        if (!firstTarget.empty()) {
            AddOption(args, "--share", firstTarget);
        }
        if (!config.localInvite.empty()) {
            AddOption(args, "--local-invite", config.localInvite);
        }
        for (size_t index = 1; index < config.watcherInvites.size(); ++index) {
            AddOption(args, "--share-target", config.watcherInvites[index]);
        }
        break;
    }
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

    AddCommonSessionArguments(
        args,
        config.signalingStunServer,
        config.udpAccessCode,
        config.allowPlaintext,
        config.reportPath);
    return args;
}

std::vector<std::string> BuildWatchArguments(const WatchSessionConfig& config)
{
    std::vector<std::string> args;
    Require(config.listenPort > 0, "Watch listen port is required.");
    if (config.playAudio) {
        AddOption(args, "--watch", static_cast<int>(config.listenPort));
    } else {
        AddOption(args, "--udp-recv", static_cast<int>(config.listenPort));
        args.emplace_back("--preview");
    }

    switch (config.connectionMode) {
    case WatchConnectionMode::Room:
        Require(!config.roomId.empty(), "Watch room ID is required.");
        AddOption(args, "--signal-room", config.roomId);
        if (!config.roomPassword.empty()) {
            AddOption(args, "--signal-room-password", config.roomPassword);
        }
        break;
    case WatchConnectionMode::DirectListen:
        if (config.lanAdvertise) {
            args.emplace_back("--lan-advertise");
        }
        break;
    case WatchConnectionMode::ManualInvite:
        if (!config.peerInvite.empty()) {
            AddOption(args, "--peer-invite", config.peerInvite);
        }
        break;
    }

    AddOption(args, "--preview-latency-ms", config.previewLatencyMs);
    if (config.playAudio) {
        AddOption(args, "--audio-playback-volume", config.audioPlaybackVolumePercent);
        if (config.muted) {
            args.emplace_back("--audio-playback-muted");
        }
    }

    AddCommonSessionArguments(
        args,
        config.signalingStunServer,
        config.udpAccessCode,
        config.allowPlaintext,
        config.reportPath);
    return args;
}

} // namespace screenshare
