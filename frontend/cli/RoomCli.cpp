#include "cli/RoomCli.h"
#include "shared/LatestRoomVideoFrame.h"
#include "shared/RoomStreamDiagnostics.h"
#include "shared/PresentationDiagnostics.h"
#include "shared/RoomLaunch.h"
#include "render/ReceiverPreviewWindow.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/win32_socket_init.h"
#include "rtc_base/logging.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <atomic>
#include <iostream>
#include <thread>

using namespace screenshare;
using namespace screenshare::v2;
using namespace screenshare::media;
using namespace std::chrono_literals;
namespace {
const char* Phase(RoomPhase phase) {
    switch (phase) {
    case RoomPhase::Idle: return "idle"; case RoomPhase::Admitting: return "admitting";
    case RoomPhase::Connecting: return "connecting"; case RoomPhase::Active: return "active";
    case RoomPhase::Reconnecting: return "reconnecting"; case RoomPhase::Stopping: return "stopping";
    case RoomPhase::Stopped: return "stopped"; case RoomPhase::Failed: return "failed";
    }
    return "failed";
}
QJsonObject Status(const RoomStatus& value) {
    auto health = [](const AudioEndpointHealth& value) -> QJsonObject {
        return {{"state", AudioEndpointStateName(value.state)}, {"failures", qint64(value.failures)}};
    };
    QJsonArray peers;
    for (const auto& peer : value.stream.peers) peers.append(StreamPeerJson(peer, value.stream.requestedRevision));
    return {{"type", "status"}, {"phase", Phase(value.phase)}, {"error", int(value.error)},
        {"roomId", QString::fromStdString(value.roomId)}, {"activePeers", qint64(value.activePeers)},
        {"failedPeers", qint64(value.failedPeers)}, {"pendingPeers", qint64(value.pendingPeers)},
        {"requestedRevision", qint64(value.stream.requestedRevision)}, {"peers", peers},
        {"requestedPreferences", StreamPreferencesJson(value.stream.preferences)},
        {"settingsApplication", StreamApplicationJson(value.stream)}, {"pipeline", PipelineDiagnosticsJson(value.stream)},
        {"aggregateUploadBps", value.stream.preferences.aggregateUploadLimitBps.value_or(0)}, {"captureRevision", qint64(value.capture.revision)},
        {"audioRevision", qint64(value.audio.revision)}, {"audioSource", media::AudioKindName(value.audio.selected.kind)},
        {"audioHealth", health(value.audio.health)}, {"playbackHealth", health(value.playback.health)},
        {"microphoneProcessing", value.audio.microphoneProcessing},
        {"playbackRevision", qint64(value.playback.revision)},
        {"playbackVolume", int(value.playback.selected.volume)}, {"playbackMuted", value.playback.selected.muted}};
}
std::atomic<bool> interrupted{false};
BOOL WINAPI ConsoleSignal(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    interrupted = true; return TRUE;
}
}

int RunRoomCliSession(const RoomSessionConfig& config, RoomRuntimeFactory factory, RoomCliHooks hooks, bool loopback) {
    RoomSession session(std::move(factory), loopback);
    auto starting = session.Start(config.room);
    std::future<StreamUpdateResult> updating;
    std::future<CaptureUpdateResult> captureUpdate;
    size_t nextCapture = 0;
    std::future<AudioUpdateResult> audioUpdate;
    size_t nextAudio = 0;
    std::future<AudioUpdateResult> playbackUpdate;
    size_t nextPlayback = 0;
    size_t next = 0; bool failed = false;
    const auto started = std::chrono::steady_clock::now();
    auto nextReport = started;
    QJsonObject lastReport;
    auto report = [&](QJsonObject value) { if (hooks.report) hooks.report(value); };
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (hooks.pump && !hooks.pump()) break;
        if (config.duration.count() && now - started >= config.duration) break;
        const auto status = session.Status();
        if (now >= nextReport) {
            auto value = Status(status); if (value != lastReport) { report(value); lastReport = value; }
            nextReport = now + 100ms;
        }
        if (starting.valid() && starting.wait_for(0ms) == std::future_status::ready) {
            const auto result = starting.get();
            if (result.error != RoomError::None) {
                report({{"type", "admission-error"}, {"error", int(result.error)}, {"outcomeUnconfirmed", result.outcomeUnconfirmed}});
                failed = true; break;
            }
        }
        if (status.phase == RoomPhase::Failed) { failed = true; break; }
        if (status.phase == RoomPhase::Stopped) break;
        if (updating.valid() && updating.wait_for(0ms) == std::future_status::ready) {
            const auto result = updating.get();
            if (result.error != StreamUpdateError::Busy && result.error != StreamUpdateError::Unavailable) {
                report({{"type", "settings"}, {"error", int(result.error)}, {"revision", qint64(result.revision)}});
                ++next;
                if (result.error != StreamUpdateError::None) { failed = true; break; }
            }
        }
        if (!updating.valid() && next < config.changes.size() && status.phase == RoomPhase::Active && now - started >= config.changes[next].at)
            updating = session.UpdateStreamPreferences(config.changes[next].preferences);
        if (captureUpdate.valid() && captureUpdate.wait_for(0ms) == std::future_status::ready) {
            const auto result = captureUpdate.get();
            report({{"type", "capture"}, {"error", int(result.error)}, {"revision", qint64(result.revision)}});
            if (result.error != CaptureUpdateError::None) { failed = true; break; }
        }
        if (!captureUpdate.valid() && nextCapture < config.captureChanges.size() && status.phase == RoomPhase::Active && now - started >= config.captureChanges[nextCapture].at)
            captureUpdate = session.SwitchCaptureSource(config.captureChanges[nextCapture++].selection);
        std::this_thread::sleep_for(5ms);
        if (playbackUpdate.valid() && playbackUpdate.wait_for(0ms) == std::future_status::ready) {
            const auto result = playbackUpdate.get();
            report({{"type", "playback"}, {"error", int(result.error)}, {"revision", qint64(result.revision)}});
            if (result.error != AudioUpdateError::None) { failed = true; break; }
        }
        if (!playbackUpdate.valid() && nextPlayback < config.playbackChanges.size() && status.phase == RoomPhase::Active && now - started >= config.playbackChanges[nextPlayback].at)
            playbackUpdate = session.UpdatePlayback(config.playbackChanges[nextPlayback++].selection);
        if (audioUpdate.valid() && audioUpdate.wait_for(0ms) == std::future_status::ready) {
            const auto result = audioUpdate.get();
            report({{"type", "audio"}, {"error", int(result.error)}, {"revision", qint64(result.revision)}});
            if (result.error != AudioUpdateError::None) { failed = true; break; }
        }
        if (!audioUpdate.valid() && nextAudio < config.audioChanges.size() && status.phase == RoomPhase::Active && now - started >= config.audioChanges[nextAudio].at)
            audioUpdate = session.SwitchAudioSource(config.audioChanges[nextAudio++].selection);
    }
    auto stopping = session.Stop();
    // Keep window messages responsive while native delivery and networking drain.
    while (stopping.wait_for(5ms) != std::future_status::ready) if (hooks.pump) hooks.pump();
    stopping.get();
    if (playbackUpdate.valid()) {
        const auto result = playbackUpdate.get();
        report({{"type", "playback-ended"}, {"error", int(result.error)}, {"revision", qint64(result.revision)}});
    }
    if (audioUpdate.valid()) {
        const auto result = audioUpdate.get();
        report({{"type", "audio-ended"}, {"error", int(result.error)}, {"revision", qint64(result.revision)}});
    }
    if (captureUpdate.valid()) {
        const auto result = captureUpdate.get();
        report({{"type", "capture-ended"}, {"error", int(result.error)}, {"revision", qint64(result.revision)}});
    }
    if (starting.valid()) {
        const auto result = starting.get();
        report({{"type", "admission-ended"}, {"error", int(result.error)}, {"outcomeUnconfirmed", result.outcomeUnconfirmed}});
    }
    const auto status = session.Status(); report(Status(status));
    return failed || status.error != RoomError::None ? 1 : 0;
}

int RunRoomCli(int argc, char** argv) {
    try {
        RoomSessionConfig config;
        if (argc > 1 && std::string_view(argv[1]) == "--room-v2") {
            if (argc != 3) throw std::invalid_argument("Usage: ScreenShare --room-v2 CONFIG.json");
            QFile file(QString::fromLocal8Bit(argv[2]));
            if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) throw std::invalid_argument("Cannot read configuration (64 KiB maximum)");
            const auto bytes = file.read(65537);
            if (bytes.size() > 65536) throw std::invalid_argument("Configuration exceeds 64 KiB");
            QJsonParseError error; const auto document = QJsonDocument::fromJson(bytes, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) throw std::invalid_argument("Invalid configuration JSON");
            config = ParseRoomSessionConfig(document.object());
        } else {
            QStringList arguments;
            for (int i = 1; i < argc; ++i) arguments.push_back(QString::fromLocal8Bit(argv[i]));
            config = ParseRoomCommand(arguments); // Validate before accessing stored defaults.
            RoomProfile profile;
            config = ParseRoomCommand(arguments, &profile);
        }
        QCoreApplication application(argc, argv);
        webrtc::WinsockInitializer winsock;
        if (winsock.error() || !webrtc::InitializeSSL()) throw std::runtime_error("Media networking initialization failed");
        struct SslLease { ~SslLease() { webrtc::CleanupSSL(); } } ssl;
        webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
        webrtc::InitializeLogging(std::move(logging));
        interrupted = false;
        if (!SetConsoleCtrlHandler(ConsoleSignal, TRUE)) throw std::runtime_error("Cannot install cancellation handler");
        struct ConsoleLease { ~ConsoleLease() { SetConsoleCtrlHandler(ConsoleSignal, FALSE); } } console;
        auto frames = std::make_shared<LatestRoomVideoFrame>(); config.media.frames = frames;
        config.media.presentation = std::make_shared<PresentationTelemetry>();
        std::unique_ptr<ReceiverPreviewWindow> preview;
        if (!config.room.host && config.preview) { preview = std::make_unique<ReceiverPreviewWindow>(); preview->SetLowLatency(true); preview->Show(); }
        RoomCliHooks hooks;
        hooks.report = [](const auto& status) { std::cout << QJsonDocument(status).toJson(QJsonDocument::Compact).constData() << std::endl; };
        uint64_t reportedPresentationErrors = 0;
        auto nextPresentationReport = std::chrono::steady_clock::now();
        hooks.pump = [&] {
            if (preview) {
                if (!preview->PumpMessages()) return false;
                if (auto frame = frames->Take()) preview->PresentFrame(*frame);
                const auto status = preview->presentationStats();
                const auto now = std::chrono::steady_clock::now();
                if (status.errors != reportedPresentationErrors || now >= nextPresentationReport) {
                    config.media.presentation->Publish({preview->framesPresented(), preview->framesDropped(), 0, uint8_t(status.outcome)});
                    reportedPresentationErrors = status.errors;
                    nextPresentationReport = now + 1s;
                    const QJsonObject update{{"type", "presentation-status"}, {"errors", qint64(status.errors)},
                        {"recoveries", qint64(status.recoveries)}, {"terminal", status.terminal},
                        {"presented", qint64(preview->framesPresented())}, {"dropped", qint64(preview->framesDropped())},
                        {"diagnostics", PresentationDiagnosticsJson(status)},
                        {"message", status.terminal ? "Video presentation failed. Leave and rejoin to retry; audio and room controls remain available." :
                            status.outcome == PresentationOutcome::Backoff || status.outcome == PresentationOutcome::Failed ?
                            "Video presentation is recovering." : "Local presentation counters; end-to-end latency is unknown."}};
                    std::cout << QJsonDocument(update).toJson(QJsonDocument::Compact).constData() << std::endl;
                }
            }
            return !interrupted.load();
        };
        const int result = RunRoomCliSession(config, WindowsRoomRuntimeFactory(config.media), std::move(hooks));
        frames->Stop(); const auto statistics = frames->statistics();
        const QJsonObject presentation{{"type", "presentation"}, {"received", qint64(statistics.received)}, {"replaced", qint64(statistics.replaced)},
            {"retained", qint64(statistics.retained)}, {"converted", qint64(statistics.converted)}, {"repacked", qint64(statistics.repacked)},
            {"gpuRetained", qint64(statistics.gpuRetained)}, {"gpuReadbacks", qint64(statistics.gpuReadbacks)},
            {"presented", qint64(preview ? preview->framesPresented() : 0)}, {"dropped", qint64(preview ? preview->framesDropped() : 0)},
            {"maximumFrameLatency", int(preview ? preview->maximumFrameLatency() : 0)},
            {"errors", qint64(preview ? preview->presentationStats().errors : 0)},
            {"recoveries", qint64(preview ? preview->presentationStats().recoveries : 0)},
            {"terminal", preview && preview->presentationStats().terminal}};
        auto finalPresentation = presentation;
        finalPresentation["diagnostics"] = PresentationDiagnosticsJson(preview ? preview->presentationStats() : FramePresentationSession::Statistics{});
        std::cout << QJsonDocument(finalPresentation).toJson(QJsonDocument::Compact).constData() << std::endl;
        return result;
    } catch (const std::exception& error) {
        // Never dump the config, admission response, password or membership token.
        std::cerr << "Room session: " << error.what() << '\n'; return 1;
    }
}
