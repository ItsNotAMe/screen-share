#pragma once

#include "audio/WasapiCapture.h"
#include "capture/DesktopCapturer.h"
#include "core/ScreenShareSession.h"
#include "transport/LanDiscovery.h"
#include "transport/SignalingClient.h"
#include "transport/StunClient.h"
#include "transport/UdpCrypto.h"
#include "transport/UdpProtocol.h"
#include "transport/UdpReceiver.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace screenshare_runtime_internal {

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
    bool adaptResolutionProvided = false;
    float adaptResolutionMinScale = 0.5f;
    bool adaptResolutionMinScaleProvided = false;
    int adaptResolutionCooldownSeconds = 5;
    bool adaptResolutionCooldownProvided = false;
    bool noAdaptResolution = false;
    bool wgcBorderRequired = false;
    bool hdrToSdr = true;
    float hdrSdrWhiteNits = 203.0f;
    float hdrSdrBgraExposure = 0.88f;
    uint16_t udpReceivePort = 0;
    std::string h264DumpPath;
    bool decodeH264 = false;
    std::string decodedBmpPath;
    bool previewWindow = false;
    bool emitVideoFrames = false;
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
    std::string signalingRoomName;
    std::string signalingRoomPassword;
    screenshare::StunServerTarget signalingStunServer{"stun.l.google.com", 19302};
    bool signalingStunServerProvided = false;
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
    std::optional<screenshare::ShareSessionConfig> cliShareSession;
    std::optional<screenshare::WatchSessionConfig> cliWatchSession;
    std::string stopFilePath;
    std::string controlFilePath;
    bool generateAccessCode = false;
    bool allowPlaintext = false;
    bool accessCodeProvided = false;
    uint64_t sessionFingerprint = 0;
    uint64_t accessCodeFingerprint = 0;
    std::optional<screenshare::UdpCryptoKey> accessCodeKey;
    bool udpLocalPortFromLocalInvite = false;
};

} // namespace screenshare_runtime_internal
