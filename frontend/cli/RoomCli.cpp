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
#include <QUrl>
#include <atomic>
#include <iostream>
#include <thread>

using namespace screenshare;
using namespace screenshare::v2;
using namespace screenshare::media;
using namespace std::chrono_literals;
namespace {
void Keys(const QJsonObject& value, std::initializer_list<const char*> allowed) {
    for (auto it = value.begin(); it != value.end(); ++it) {
        bool known = false;
        for (auto key : allowed) if (it.key() == QLatin1String(key)) known = true;
        if (!known) throw std::invalid_argument("Unknown configuration field");
    }
}
QString String(const QJsonObject& value, const char* key, QString fallback = {}) {
    if (!value.contains(key)) return fallback;
    if (!value[key].isString() || value[key].toString().size() > 4096) throw std::invalid_argument("Invalid text field");
    return value[key].toString();
}
int Integer(const QJsonObject& value, const char* key, int fallback, int minimum, int maximum) {
    if (!value.contains(key)) return fallback;
    const auto item = value[key]; const double number = item.toDouble(-1);
    if (!item.isDouble() || number < minimum || number > maximum || number != static_cast<int>(number))
        throw std::invalid_argument("Invalid integer field");
    return static_cast<int>(number);
}
bool Boolean(const QJsonObject& value, const char* key, bool fallback) {
    if (!value.contains(key)) return fallback;
    if (!value[key].isBool()) throw std::invalid_argument("Invalid boolean field");
    return value[key].toBool();
}
QJsonObject Object(const QJsonObject& value, const char* key) {
    if (!value.contains(key)) return {};
    if (!value[key].isObject()) throw std::invalid_argument("Invalid object field");
    return value[key].toObject();
}
StreamPreferences Preferences(const QJsonObject& object) {
    Keys(object, {"preset", "resolution", "width", "height", "fpsMode", "fps", "bitrateMode", "bitrateBps"});
    StreamPreferences result;
    const auto preset = String(object, "preset", "gaming");
    if (preset != "gaming" && preset != "quality") throw std::invalid_argument("Invalid preset");
    result.preset = preset == "gaming" ? StreamPreset::Gaming : StreamPreset::Quality;
    const auto resolution = String(object, "resolution", "auto");
    if (resolution != "auto" && resolution != "fixed" && resolution != "native") throw std::invalid_argument("Invalid resolution mode");
    result.resolution = resolution == "fixed" ? ResolutionMode::Fixed : resolution == "native" ? ResolutionMode::Native : ResolutionMode::Auto;
    result.width = Integer(object, "width", 1920, 2, 3840); result.height = Integer(object, "height", 1080, 2, 2160);
    const auto fpsMode = String(object, "fpsMode", "manual"), bitrateMode = String(object, "bitrateMode", "auto");
    if ((fpsMode != "auto" && fpsMode != "manual") || (bitrateMode != "auto" && bitrateMode != "manual"))
        throw std::invalid_argument("Invalid settings mode");
    result.fpsMode = fpsMode == "auto" ? SettingMode::Auto : SettingMode::Manual;
    result.bitrateMode = bitrateMode == "auto" ? SettingMode::Auto : SettingMode::Manual;
    result.fps = Integer(object, "fps", 60, 1, 240);
    if (object.contains("bitrateBps")) result.bitrateLimitBps = Integer(object, "bitrateBps", 0, 1000, 100000000);
    ValidateStreamPreferences(result); return result;
}
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
        {"observedRevision", qint64(peer.observedRevision)}, {"rejected", peer.rejected}, {"width", peer.width}, {"height", peer.height}});
    return {{"type", "status"}, {"phase", Phase(value.phase)}, {"error", int(value.error)},
        {"roomId", QString::fromStdString(value.roomId)}, {"activePeers", qint64(value.activePeers)},
        {"failedPeers", qint64(value.failedPeers)}, {"pendingPeers", qint64(value.pendingPeers)},
        {"requestedRevision", qint64(value.stream.requestedRevision)}, {"peers", peers}};
}
std::atomic<bool> interrupted{false};
BOOL WINAPI ConsoleSignal(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    interrupted = true; return TRUE;
}
}

RoomCliConfig ParseRoomCliConfig(const QJsonObject& object, bool loopback) {
    Keys(object, {"origin", "host", "roomId", "nickname", "name", "password", "public", "viewerLimit", "seconds", "preview", "capture", "audio", "stream", "changes"});
    RoomCliConfig result;
    const auto origin = String(object, "origin"); const QUrl url(origin);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() ||
        (!url.path().isEmpty() && url.path() != "/") || (url.scheme() != "https" && !(loopback && url.scheme() == "http" && url.host() == "127.0.0.1")))
        throw std::invalid_argument("A HTTPS service origin is required");
    result.room.origin = origin.toStdString(); result.room.host = Boolean(object, "host", false);
    result.room.roomId = String(object, "roomId").toStdString(); result.room.nickname = String(object, "nickname").toStdString();
    result.room.name = String(object, "name").toStdString(); result.room.password = String(object, "password").toStdString();
    result.room.publicRoom = Boolean(object, "public", true); result.room.viewerLimit = Integer(object, "viewerLimit", 4, 1, 63);
    if (!result.room.host && result.room.roomId.empty()) throw std::invalid_argument("Viewer requires roomId");
    result.duration = std::chrono::seconds(Integer(object, "seconds", 0, 0, 86400)); result.preview = Boolean(object, "preview", true);
    result.media.preferences = Preferences(Object(object, "stream"));
    const auto capture = Object(object, "capture"); Keys(capture, {"display", "window", "fps"});
    result.media.capture.displayIndex = Integer(capture, "display", 0, 0, 63);
    result.media.capture.targetFps = Integer(capture, "fps", result.media.preferences.fps, 1, 240);
    if (capture.contains("window")) {
        if (capture.contains("display")) throw std::invalid_argument("Choose a display or window");
        bool ok = false; const auto handle = String(capture, "window").toULongLong(&ok, 0);
        if (!ok || !handle) throw std::invalid_argument("Invalid window handle string");
        result.media.capture.sourceType = CaptureSourceType::Window; result.media.capture.windowHandle = handle;
    }
    const auto audio = Object(object, "audio"); Keys(audio, {"source", "deviceId", "playbackDeviceId", "processId"});
    const auto source = String(audio, "source", "system");
    if (source != "system" && source != "microphone" && source != "process") throw std::invalid_argument("Invalid audio source");
    result.media.audio.source = source == "microphone" ? AudioCaptureSource::Microphone : source == "process" ? AudioCaptureSource::ProcessOutput : AudioCaptureSource::SystemOutput;
    result.media.audio.deviceId = String(audio, "deviceId").toStdWString();
    result.media.playbackDeviceId = String(audio, "playbackDeviceId").toStdWString();
    result.media.audio.processId = Integer(audio, "processId", 0, 0, INT_MAX);
    if (source == "process" && !result.media.audio.processId) throw std::invalid_argument("Process audio requires processId");
    if (object.contains("changes") && !object["changes"].isArray()) throw std::invalid_argument("Invalid settings changes");
    const auto changes = object["changes"].toArray();
    if (changes.size() > 64 || (!result.room.host && !changes.isEmpty())) throw std::invalid_argument("Invalid settings change count or role");
    int previous = -1;
    for (const auto& item : changes) {
        if (!item.isObject()) throw std::invalid_argument("Invalid settings change");
        const auto change = item.toObject(); Keys(change, {"atMs", "stream"});
        const int at = Integer(change, "atMs", -1, 0, 86400000);
        if (at <= previous || !change.contains("stream")) throw std::invalid_argument("Settings changes must be ordered and complete");
        previous = at; result.changes.push_back({std::chrono::milliseconds(at), Preferences(Object(change, "stream"))});
    }
    return result;
}

int RunRoomCliSession(const RoomCliConfig& config, RoomRuntimeFactory factory, RoomCliHooks hooks, bool loopback) {
    RoomSession session(std::move(factory), loopback);
    auto starting = session.Start(config.room);
    std::future<StreamUpdateResult> updating;
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
        std::this_thread::sleep_for(5ms);
    }
    auto stopping = session.Stop();
    // Keep window messages responsive while native delivery and networking drain.
    while (stopping.wait_for(5ms) != std::future_status::ready) if (hooks.pump) hooks.pump();
    stopping.get();
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
        auto config = ParseRoomCliConfig(document.object());
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
