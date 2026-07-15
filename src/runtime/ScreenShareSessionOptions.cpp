#include "runtime/ScreenShareSessionOptions.h"

#include "runtime/ScreenShareRuntimeSupport.h"
#include "transport/NatTraversal.h"
#include "transport/SignalingClient.h"
#include "transport/StunClient.h"
#include "transport/UdpCrypto.h"
#include "transport/UdpSender.h"

#include <algorithm>
#include <ctime>
#include <iostream>
#include <set>
#include <stdexcept>
#include <utility>

namespace screenshare_runtime_internal {

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
    // Access codes are meant to be app-generated (GenerateUdpAccessCode: 20
    // random chars, ~100 bits). Because the routing fingerprint is a fast hash
    // broadcast in the clear, a short/guessable code would be brute-forceable
    // offline, so enforce a minimum length that a generated code always meets
    // (23 chars incl. group dashes) while rejecting weak hand-picked codes.
    constexpr size_t MinAccessCodeBytes = 16;
    if (accessCode.size() < MinAccessCodeBytes || accessCode.size() > 64) {
        throw std::invalid_argument(
            "--access-code must be 16-64 bytes; prefer an app-generated code");
    }

    for (const char ch : accessCode) {
        const unsigned char valueByte = static_cast<unsigned char>(ch);
        if (valueByte < 32U || valueByte == 127U) {
            throw std::invalid_argument("--access-code may not contain control characters");
        }
    }

    return accessCode;
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
           !options.signalingRoomName.empty() ||
           !options.signalingRoomPassword.empty() ||
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

std::string FormatNatEndpoint(const screenshare::NatInviteEndpoint& endpoint)
{
    if (endpoint.host.empty() || endpoint.port == 0) {
        return "none";
    }
    return endpoint.host + ":" + std::to_string(endpoint.port);
}

uint64_t NatProbeSessionFingerprint(const Options& options)
{
    if (options.signalingLive) {
        return SessionFingerprint(options.signalingRoomId);
    }
    return (options.sessionIdProvided || options.sessionIdFromLocalInvite) ? options.sessionFingerprint : 0;
}

namespace {

bool HasNatEndpoint(const screenshare::NatInviteEndpoint& endpoint)
{
    return !endpoint.host.empty() && endpoint.port != 0;
}

} // namespace

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

namespace {

void RequireSessionConfig(bool condition, const char* message)
{
    if (!condition) {
        throw std::invalid_argument(message);
    }
}

void ApplyTypedSessionSecurity(
    Options& options,
    const std::string& accessCode,
    bool allowPlaintext,
    const std::string& reportPath)
{
    if (!accessCode.empty() && allowPlaintext) {
        throw std::invalid_argument("Access code cannot be combined with plaintext mode.");
    }
    if (!accessCode.empty()) {
        const std::string parsedAccessCode = ParseAccessCode(accessCode.c_str());
        options.accessCodeFingerprint = screenshare::UdpAccessCodeFingerprint(parsedAccessCode);
        options.accessCodeKey = screenshare::DeriveUdpCryptoKey(parsedAccessCode);
        options.accessCodeProvided = true;
    } else if (allowPlaintext) {
        options.allowPlaintext = true;
    }
    options.saveReportPath = reportPath;
}

void ApplyTypedSignalingConfig(
    Options& options,
    const std::string& roomId,
    const std::string& roomPassword,
    const std::string& signalingServerUrl,
    const std::string& signalingStunServer,
    int signalingTimeoutMs,
    int signalingSetupSeconds)
{
    RequireSessionConfig(!roomId.empty(), "Room ID is required.");
    screenshare::ValidateSignalingRoomId(roomId);
    if (!roomPassword.empty() && (options.accessCodeProvided || options.allowPlaintext)) {
        throw std::invalid_argument("Room password cannot be combined with access code or plaintext mode.");
    }

    options.signalingLive = true;
    options.signalingServerUrl =
        signalingServerUrl.empty() ? std::string(DefaultSignalingServerUrl) : signalingServerUrl;
    options.signalingRoomId = roomId;
    options.signalingPeerId = options.sessionId;
    options.signalingRoomPassword = roomPassword;
    if (!signalingStunServer.empty()) {
        options.signalingStunServer = screenshare::ParseStunServerTarget(signalingStunServer.c_str());
        options.signalingStunServerProvided = true;
    }
    options.signalingTimeoutMs = signalingTimeoutMs;
    options.signalingSetupSeconds = signalingSetupSeconds;
    if (options.signalingTimeoutMs < 100 || options.signalingTimeoutMs > 30000) {
        throw std::invalid_argument("Signaling timeout must be between 100 and 30000 ms.");
    }
    if (options.signalingSetupSeconds < 1 || options.signalingSetupSeconds > 60) {
        throw std::invalid_argument("Signaling setup seconds must be between 1 and 60.");
    }
}

void ApplyTypedStreamSettings(Options& options, const screenshare::StreamSettings& settings)
{
    options.fps = settings.fps;
    options.bitrate = settings.bitrateBps;
    options.adaptBitrate = settings.adaptBitrate || settings.adaptResolution;
    options.adaptResolution = settings.adaptResolution;
    if (settings.outputResolution &&
        settings.outputResolution->width > 0 &&
        settings.outputResolution->height > 0) {
        options.width = settings.outputResolution->width;
        options.height = settings.outputResolution->height;
    }

    if (options.fps <= 0 || options.fps > 240) {
        throw std::invalid_argument("FPS must be between 1 and 240.");
    }
    if ((options.width == 0) != (options.height == 0)) {
        throw std::invalid_argument("Output width and height must be provided together.");
    }
    if (options.width < 0 || options.height < 0) {
        throw std::invalid_argument("Output width and height must be positive.");
    }
    if (options.width > 0 && ((options.width % 2) != 0 || (options.height % 2) != 0)) {
        throw std::invalid_argument("Stream output width and height must be even.");
    }
}

void ApplyTypedLocalInviteSession(
    Options& options,
    const screenshare::NatInvite& invite,
    bool& sessionFromLocalInvite,
    const char* optionName)
{
    if (invite.sessionId.empty()) {
        return;
    }
    if (sessionFromLocalInvite) {
        return;
    }
    if (options.sessionIdProvided && options.sessionId != invite.sessionId) {
        throw std::invalid_argument(std::string(optionName) + " session does not match the current session");
    }
    options.sessionId = invite.sessionId;
    options.sessionIdFromLocalInvite = true;
    sessionFromLocalInvite = true;
}

UdpSendTargetSpec BuildTypedShareTargetSpec(
    Options& options,
    const ExtraShareTargetOption& targetOption,
    bool primary,
    bool& sessionFromLocalInvite)
{
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
                primary ? "local invite" : "watcher local invite");
            if (primary && options.udpLocalPortProvided && options.udpLocalPort != localInvite.localEndpoint.port) {
                throw std::invalid_argument("UDP local port does not match the local invite port.");
            }
            spec.localPort = localInvite.localEndpoint.port;
            spec.localPortFromLocalInvite = true;
            spec.collectNatProbeTargets = true;
            spec.preferNatProbeTargets = invite.sessionFingerprint == localInvite.sessionFingerprint;
            spec.natProbeSessionFingerprint = localInvite.sessionFingerprint;
            ApplyTypedLocalInviteSession(
                options,
                localInvite,
                sessionFromLocalInvite,
                primary ? "local invite" : "watcher local invite");
        }
    } else {
        if (!targetOption.localInvite.empty()) {
            throw std::invalid_argument("Watcher local invite can only be used with watcher invite targets.");
        }
        static_cast<void>(screenshare::ParseUdpSenderTarget(targetOption.target));
        spec.target = targetOption.target;
    }
    return spec;
}

void AddTypedShareTarget(
    Options& options,
    const ExtraShareTargetOption& target,
    bool primary,
    bool& sessionFromLocalInvite,
    std::set<uint16_t>& usedLocalPorts)
{
    auto spec = BuildTypedShareTargetSpec(options, target, primary, sessionFromLocalInvite);
    if (primary && spec.localPort == 0 && options.udpLocalPortProvided) {
        spec.localPort = options.udpLocalPort;
    }
    if (spec.localPort != 0 && !usedLocalPorts.insert(spec.localPort).second) {
        throw std::invalid_argument("Each NAT share target local invite must use a unique local port.");
    }

    if (primary) {
        options.udpSendTarget = spec.target;
        options.udpSendTargetFromPeerInvite = spec.fromPeerInvite;
        options.udpSendPeerInviteEndpoint = spec.inviteEndpoint;
        options.udpLocalPort = spec.localPort;
        if (options.udpLocalPort != 0) {
            options.udpLocalPortProvided = true;
        }
        options.udpLocalPortFromLocalInvite = spec.localPortFromLocalInvite;
    }

    options.udpSendTargetSpecs.push_back(spec);
    options.udpSendTargets.push_back(spec.target);
}

void ApplyTypedSharePreset(Options& options, const screenshare::ShareSessionConfig& config)
{
    if (!config.sessionId.empty()) {
        options.sessionId = ParseSessionId(config.sessionId.c_str());
        options.sessionIdProvided = true;
    }
    options.captureSourceType = config.captureSourceType == screenshare::SessionCaptureSourceType::Window
        ? screenshare::CaptureSourceType::Window
        : screenshare::CaptureSourceType::Display;
    options.displayIndex = config.displayIndex;
    options.windowHandle = config.windowHandle;
    options.windowProcessId = config.windowProcessId;
    if (options.captureSourceType == screenshare::CaptureSourceType::Window && options.windowHandle == 0) {
        throw std::invalid_argument("Window Share source requires a selected application window.");
    }
    options.streamEncode = true;
    options.sharePreset = true;
    options.seconds = config.seconds;
    // Low latency mode: push frames onto the wire immediately (no send pacing)
    // and do not let datagrams sit queued, trading smoothing for responsiveness.
    options.udpPacing = !config.stream.lowLatency;
    options.udpMaxQueueMs = config.stream.lowLatency ? 0 : DefaultShareUdpMaxQueueMs;
    if (config.udpLocalPort > 0) {
        options.udpLocalPort = config.udpLocalPort;
        options.udpLocalPortProvided = true;
    }
    options.streamEncoderPreference = StreamEncoderPreference::Software;
    options.videoPaused = config.hostVideoPaused;
    ApplyTypedStreamSettings(options, config.stream);

    if (options.seconds < 0) {
        throw std::invalid_argument("Session seconds must be non-negative.");
    }
    if (config.captureSystemAudio && !config.hostAudioMuted) {
        options.audioCapture = true;
        options.audioCaptureSource =
            options.captureSourceType == screenshare::CaptureSourceType::Window &&
                config.windowProcessId != 0 &&
                config.audioDeviceId.empty() ?
            screenshare::AudioCaptureSource::ProcessOutput :
            screenshare::AudioCaptureSource::SystemOutput;
        options.audioProcessId = config.windowProcessId;
        if (!config.audioDeviceId.empty()) {
            options.audioDeviceId = config.audioDeviceId;
            options.audioDeviceIdProvided = true;
        }
    } else if (!config.audioDeviceId.empty()) {
        throw std::invalid_argument("Audio device selection requires system audio capture.");
    }
}

} // namespace

Options BuildShareSessionOptions(
    const screenshare::ShareSessionConfig& config,
    std::string defaultSessionId)
{
    Options options;
    options.sessionId = std::move(defaultSessionId);
    ApplyTypedSessionSecurity(options, config.udpAccessCode, config.allowPlaintext, config.reportPath);
    ApplyTypedSharePreset(options, config);

    if (config.connectionMode != screenshare::ShareConnectionMode::Room &&
        (!config.signalingServerUrl.empty() || !config.signalingStunServer.empty())) {
        throw std::invalid_argument("Signaling settings require a room Share session.");
    }

    bool sessionFromLocalInvite = false;
    std::set<uint16_t> usedLocalPorts;
    switch (config.connectionMode) {
    case screenshare::ShareConnectionMode::Room:
        RequireSessionConfig(config.roomPort > 0, "Share room port is required.");
        options.shareRoom = true;
        options.udpLocalPort = config.roomPort;
        options.udpLocalPortProvided = true;
        options.signalingRoomName = config.roomName;
        ApplyTypedSignalingConfig(
            options,
            config.roomId,
            config.roomPassword,
            config.signalingServerUrl,
            config.signalingStunServer,
            config.signalingTimeoutMs,
            config.signalingSetupSeconds);
        break;
    case screenshare::ShareConnectionMode::DirectTargets:
        RequireSessionConfig(!config.targets.empty(), "At least one Share target is required.");
        for (size_t index = 0; index < config.targets.size(); ++index) {
            AddTypedShareTarget(
                options,
                ExtraShareTargetOption{config.targets[index], {}},
                index == 0,
                sessionFromLocalInvite,
                usedLocalPorts);
        }
        break;
    case screenshare::ShareConnectionMode::ManualInvite: {
        const std::string firstTarget =
            config.watcherInvites.empty() ? config.localInvite : config.watcherInvites.front();
        RequireSessionConfig(!firstTarget.empty(), "A room invite or watcher invite is required.");
        if (LooksLikeNatInvite(firstTarget)) {
            options.peerInvite = firstTarget;
        } else if (!config.localInvite.empty()) {
            throw std::invalid_argument("Local invite requires an invite share target.");
        }
        AddTypedShareTarget(
            options,
            ExtraShareTargetOption{firstTarget, config.localInvite},
            true,
            sessionFromLocalInvite,
            usedLocalPorts);
        for (size_t index = 1; index < config.watcherInvites.size(); ++index) {
            AddTypedShareTarget(
                options,
                ExtraShareTargetOption{config.watcherInvites[index], {}},
                false,
                sessionFromLocalInvite,
                usedLocalPorts);
        }
        break;
    }
    }

    if (options.accessCodeProvided && !HasUdpSession(options)) {
        throw std::invalid_argument("Access code requires a UDP Share session.");
    }
    if (options.allowPlaintext && !HasUdpSession(options)) {
        throw std::invalid_argument("Plaintext mode requires a UDP Share session.");
    }
    if (options.signalingLive && options.signalingRoomId.empty()) {
        throw std::invalid_argument("Live signaling requires a room ID.");
    }
    options.sessionFingerprint = SessionFingerprint(options.sessionId);
    return options;
}

Options BuildWatchSessionOptions(
    const screenshare::WatchSessionConfig& config,
    std::string defaultSessionId)
{
    Options options;
    options.sessionId = std::move(defaultSessionId);
    ApplyTypedSessionSecurity(options, config.udpAccessCode, config.allowPlaintext, config.reportPath);
    if (!config.sessionId.empty()) {
        options.sessionId = ParseSessionId(config.sessionId.c_str());
        options.sessionIdProvided = true;
    }

    if (config.connectionMode != screenshare::WatchConnectionMode::Room &&
        (!config.signalingServerUrl.empty() || !config.signalingStunServer.empty())) {
        throw std::invalid_argument("Signaling settings require a room Watch session.");
    }

    RequireSessionConfig(config.listenPort > 0, "Watch listen port is required.");
    options.udpReceivePort = config.listenPort;
    options.previewWindow = !config.emitVideoFrames;
    options.emitVideoFrames = config.emitVideoFrames;
    options.decodeH264 = true;
    options.audioPlayback = config.playAudio;
    options.seconds = config.seconds;
    options.previewLatencyMs = config.previewLatencyMs;
    options.previewLatencyProvided = true;
    if (config.playAudio) {
        options.audioPlaybackVolumePercent = static_cast<float>(config.audioPlaybackVolumePercent);
        options.audioPlaybackVolumeProvided = true;
        if (config.muted) {
            options.audioPlaybackMuted = true;
            options.audioPlaybackMutedProvided = true;
        }
    }

    switch (config.connectionMode) {
    case screenshare::WatchConnectionMode::Room:
        ApplyTypedSignalingConfig(
            options,
            config.roomId,
            config.roomPassword,
            config.signalingServerUrl,
            config.signalingStunServer,
            config.signalingTimeoutMs,
            config.signalingSetupSeconds);
        break;
    case screenshare::WatchConnectionMode::DirectListen:
        options.lanAdvertise = config.lanAdvertise;
        break;
    case screenshare::WatchConnectionMode::ManualInvite:
        options.peerInvite = config.peerInvite;
        break;
    }

    if (!options.avSyncExplicit && !options.avSyncDisabled && options.previewWindow && options.audioPlayback) {
        options.avSync = true;
    }
    if (options.avSync) {
        options.audioPlaybackLatencyMs = std::max(options.audioPlaybackLatencyMs, options.previewLatencyMs);
    }
    if (options.previewLatencyMs < 0 || options.previewLatencyMs > 2000) {
        throw std::invalid_argument("Preview latency must be between 0 and 2000 ms.");
    }
    if (options.audioPlaybackVolumePercent < 0.0f || options.audioPlaybackVolumePercent > 200.0f) {
        throw std::invalid_argument("Audio playback volume must be between 0 and 200 percent.");
    }
    if (options.lanAdvertise && options.signalingLive) {
        throw std::invalid_argument("LAN advertising cannot be combined with room watching.");
    }
    if (options.allowPlaintext && !HasUdpSession(options)) {
        throw std::invalid_argument("Plaintext mode requires a UDP Watch session.");
    }
    if (options.seconds < 0) {
        throw std::invalid_argument("Session seconds must be non-negative.");
    }
    options.sessionFingerprint = SessionFingerprint(options.sessionId);
    return options;
}

} // namespace screenshare_runtime_internal
