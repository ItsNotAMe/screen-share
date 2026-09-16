#include "cli/RoomCli.h"
#include "shared/LatestRoomVideoFrame.h"
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
    QJsonArray peers;
    for (const auto& peer : value.stream.peers) peers.append(QJsonObject{
        {"peerId", QString::fromStdString(peer.peerId)}, {"appliedRevision", qint64(peer.appliedRevision)},
        {"observedRevision", qint64(peer.observedRevision)}, {"rejected", peer.rejected}, {"width", peer.width}, {"height", peer.height},
        {"allocatedVideoBps", peer.allocatedVideoBitrateBps}, {"appliedVideoBps", peer.appliedVideoBitrateBps},
        {"transportSendBps", peer.transportSendBps ? QJsonValue(qint64(*peer.transportSendBps)) : QJsonValue(QJsonValue::Null)}});
    return {{"type", "status"}, {"phase", Phase(value.phase)}, {"error", int(value.error)},
        {"roomId", QString::fromStdString(value.roomId)}, {"activePeers", qint64(value.activePeers)},
        {"failedPeers", qint64(value.failedPeers)}, {"pendingPeers", qint64(value.pendingPeers)},
        {"requestedRevision", qint64(value.stream.requestedRevision)}, {"peers", peers},
        {"aggregateUploadBps", value.stream.preferences.aggregateUploadLimitBps.value_or(0)}, {"captureRevision", qint64(value.capture.revision)},
        {"audioRevision", qint64(value.audio.revision)}, {"playbackRevision", qint64(value.playback.revision)},
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
        if (argc != 3) throw std::invalid_argument("Usage: ScreenShare --room-v2 CONFIG.json");
        QFile file(QString::fromLocal8Bit(argv[2]));
        if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) throw std::invalid_argument("Cannot read configuration (64 KiB maximum)");
        const auto bytes = file.read(65537);
        if (bytes.size() > 65536) throw std::invalid_argument("Configuration exceeds 64 KiB");
        QJsonParseError error; const auto document = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) throw std::invalid_argument("Invalid configuration JSON");
        auto config = ParseRoomSessionConfig(document.object());
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
        std::unique_ptr<ReceiverPreviewWindow> preview;
        if (!config.room.host && config.preview) { preview = std::make_unique<ReceiverPreviewWindow>(); preview->Show(); }
        RoomCliHooks hooks;
        hooks.report = [](const auto& status) { std::cout << QJsonDocument(status).toJson(QJsonDocument::Compact).constData() << std::endl; };
        hooks.pump = [&] {
            if (preview) {
                if (!preview->PumpMessages()) return false;
                if (auto frame = frames->Take()) preview->PresentFrame(*frame);
            }
            return !interrupted.load();
        };
        return RunRoomCliSession(config, WindowsRoomRuntimeFactory(config.media), std::move(hooks));
    } catch (const std::exception& error) {
        // Never dump the config, admission response, password or membership token.
        std::cerr << "Room session: " << error.what() << '\n'; return 1;
    }
}
