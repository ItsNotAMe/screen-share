#include "shared/RoomSessionConfig.h"
#include "shared/RoomLink.h"
#include "shared/StreamPreferencesJson.h"
#include <QJsonArray>
#include <QUrl>
#include <climits>

using namespace screenshare;
using namespace screenshare::media;
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
}
StreamPreferences ParseStreamPreferences(const QJsonObject& object) {
    Keys(object, {"preset", "resolution", "width", "height", "fpsMode", "fps", "bitrateMode", "bitrateBps", "aggregateUploadBps"});
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
    if (object.contains("aggregateUploadBps")) result.aggregateUploadLimitBps = Integer(object, "aggregateUploadBps", 0, 160000, 1000000000);
    ValidateStreamPreferences(result); return result;
}
QJsonObject StreamPreferencesJson(const StreamPreferences& value) {
    ValidateStreamPreferences(value);
    QJsonObject result{{"preset", value.preset == StreamPreset::Gaming ? "gaming" : "quality"},
        {"resolution", value.resolution == ResolutionMode::Auto ? "auto" : value.resolution == ResolutionMode::Fixed ? "fixed" : "native"},
        {"width", value.width}, {"height", value.height}, {"fps", value.fps},
        {"fpsMode", value.fpsMode == SettingMode::Auto ? "auto" : "manual"},
        {"bitrateMode", value.bitrateMode == SettingMode::Auto ? "auto" : "manual"}};
    if (value.bitrateLimitBps) result["bitrateBps"] = *value.bitrateLimitBps;
    if (value.aggregateUploadLimitBps) result["aggregateUploadBps"] = *value.aggregateUploadLimitBps;
    return result;
}
RoomSessionConfig ParseRoomSessionConfig(const QJsonObject& object, bool loopback) {
    Keys(object, {"origin", "host", "roomId", "nickname", "name", "password", "public", "viewerLimit", "seconds", "preview", "capture", "audio", "stream", "changes", "captureChanges", "audioChanges", "playbackChanges"});
    RoomSessionConfig result;
    const auto origin = String(object, "origin"); const QUrl url(origin);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() ||
        (!url.path().isEmpty() && url.path() != "/") || (url.scheme() != "https" && !(loopback && url.scheme() == "http" && url.host() == "127.0.0.1")))
        throw std::invalid_argument("A HTTPS service origin is required");
    result.room.origin = origin.toStdString(); result.room.host = Boolean(object, "host", false);
    result.room.roomId = String(object, "roomId").toStdString(); result.room.nickname = String(object, "nickname").toStdString();
    result.room.name = String(object, "name").toStdString(); result.room.password = String(object, "password").toStdString();
    result.room.publicRoom = Boolean(object, "public", true); result.room.viewerLimit = Integer(object, "viewerLimit", 4, 1, 63);
    if (!result.room.host) {
        const auto roomId = ParseRoomReference(QString::fromStdString(result.room.roomId));
        if (!roomId) throw std::invalid_argument("Viewer requires a room ID or v2 room link");
        result.room.roomId = roomId->toStdString();
    }
    result.duration = std::chrono::seconds(Integer(object, "seconds", 0, 0, 86400)); result.preview = Boolean(object, "preview", true);
    result.media.preferences = ParseStreamPreferences(Object(object, "stream"));
    if (object.contains("playbackChanges") && !object["playbackChanges"].isArray()) throw std::invalid_argument("Invalid playback changes");
    const auto playbackChanges = object["playbackChanges"].toArray();
    if (playbackChanges.size() > 64 || (result.room.host && !playbackChanges.isEmpty())) throw std::invalid_argument("Invalid playback change count or role");
    for (const auto& item : playbackChanges) {
        if (!item.isObject()) throw std::invalid_argument("Invalid playback change");
        const auto change = item.toObject(); Keys(change, {"atMs", "deviceId", "volume", "muted"});
        if (!change.contains("atMs")) throw std::invalid_argument("Playback change requires time");
        PlaybackSelection selection{String(change, "deviceId").toStdWString(), unsigned(Integer(change, "volume", 100, 0, 100)), Boolean(change, "muted", false)};
        ValidatePlaybackSelection(selection);
        const auto at = std::chrono::milliseconds(Integer(change, "atMs", 0, 0, 86400000));
        if (!result.playbackChanges.empty() && at <= result.playbackChanges.back().at) throw std::invalid_argument("Playback changes must be ordered");
        result.playbackChanges.push_back({at, std::move(selection)});
    }
    if (object.contains("audioChanges") && !object["audioChanges"].isArray()) throw std::invalid_argument("Invalid audio changes");
    const auto audioChanges = object["audioChanges"].toArray();
    if (audioChanges.size() > 64 || (!result.room.host && !audioChanges.isEmpty())) throw std::invalid_argument("Invalid audio change count or role");
    for (const auto& item : audioChanges) {
        if (!item.isObject()) throw std::invalid_argument("Invalid audio change");
        const auto change = item.toObject(); Keys(change, {"atMs", "source", "deviceId", "processId"});
        if (!change.contains("atMs") || !change.contains("source")) throw std::invalid_argument("Audio change requires time and source");
        const auto source = String(change, "source");
        if (source != "system" && source != "microphone" && source != "process") throw std::invalid_argument("Invalid audio source");
        AudioSelection selection{source == "microphone" ? AudioKind::Microphone : source == "process" ? AudioKind::Process : AudioKind::System,
            String(change, "deviceId").toStdWString(), uint32_t(Integer(change, "processId", 0, 0, INT_MAX))};
        ValidateAudioSelection(selection);
        const auto at = std::chrono::milliseconds(Integer(change, "atMs", 0, 0, 86400000));
        if (!result.audioChanges.empty() && at <= result.audioChanges.back().at) throw std::invalid_argument("Audio changes must be ordered");
        result.audioChanges.push_back({at, std::move(selection)});
    }
    if (object.contains("captureChanges") && !object["captureChanges"].isArray()) throw std::invalid_argument("Invalid capture changes");
    const auto captureChanges = object["captureChanges"].toArray();
    if (captureChanges.size() > 64 || (!result.room.host && !captureChanges.isEmpty())) throw std::invalid_argument("Invalid capture change count or role");
    for (const auto& item : captureChanges) {
        if (!item.isObject()) throw std::invalid_argument("Invalid capture change");
        const auto change = item.toObject(); Keys(change, {"atMs", "display", "window", "fps"});
        if (!change.contains("atMs") || change.contains("display") == change.contains("window")) throw std::invalid_argument("Capture change requires time and one source");
        CaptureSelection selection; selection.fps = Integer(change, "fps", 60, 1, 240);
        if (change.contains("window")) {
            bool ok = false; selection.kind = CaptureKind::Window; selection.window = String(change, "window").toULongLong(&ok, 0);
            if (!ok) throw std::invalid_argument("Invalid capture window");
        } else selection.display = Integer(change, "display", 0, 0, 63);
        ValidateCaptureSelection(selection);
        const auto at = std::chrono::milliseconds(Integer(change, "atMs", 0, 0, 86400000));
        if (!result.captureChanges.empty() && at < result.captureChanges.back().at) throw std::invalid_argument("Capture changes must be ordered");
        result.captureChanges.push_back({at, selection});
    }
    const auto capture = Object(object, "capture"); Keys(capture, {"display", "window", "fps"});
    result.media.capture.displayIndex = Integer(capture, "display", 0, 0, 63);
    result.media.capture.targetFps = Integer(capture, "fps", result.media.preferences.fps, 1, 240);
    if (capture.contains("window")) {
        if (capture.contains("display")) throw std::invalid_argument("Choose a display or window");
        bool ok = false; const auto handle = String(capture, "window").toULongLong(&ok, 0);
        if (!ok || !handle) throw std::invalid_argument("Invalid window handle string");
        result.media.capture.sourceType = CaptureSourceType::Window; result.media.capture.windowHandle = handle;
    }
    const auto audio = Object(object, "audio"); Keys(audio, {"source", "deviceId", "playbackDeviceId", "processId", "playbackVolume", "playbackMuted"});
    const auto source = String(audio, "source", "system");
    if (source != "system" && source != "microphone" && source != "process") throw std::invalid_argument("Invalid audio source");
    result.media.audio.source = source == "microphone" ? AudioCaptureSource::Microphone : source == "process" ? AudioCaptureSource::ProcessOutput : AudioCaptureSource::SystemOutput;
    result.media.audio.deviceId = String(audio, "deviceId").toStdWString();
    result.media.playbackDeviceId = String(audio, "playbackDeviceId").toStdWString();
    result.media.playbackVolume = unsigned(Integer(audio, "playbackVolume", 100, 0, 100));
    result.media.playbackMuted = Boolean(audio, "playbackMuted", false);
    ValidatePlaybackSelection({result.media.playbackDeviceId, result.media.playbackVolume, result.media.playbackMuted});
    result.media.audio.processId = Integer(audio, "processId", 0, 0, INT_MAX);
    if (source == "process" && !result.media.audio.processId) throw std::invalid_argument("Process audio requires processId");
    ValidateAudioSelection({source == "microphone" ? AudioKind::Microphone : source == "process" ? AudioKind::Process : AudioKind::System,
        result.media.audio.deviceId, result.media.audio.processId});
    if (object.contains("changes") && !object["changes"].isArray()) throw std::invalid_argument("Invalid settings changes");
    const auto changes = object["changes"].toArray();
    if (changes.size() > 64 || (!result.room.host && !changes.isEmpty())) throw std::invalid_argument("Invalid settings change count or role");
    int previous = -1;
    for (const auto& item : changes) {
        if (!item.isObject()) throw std::invalid_argument("Invalid settings change");
        const auto change = item.toObject(); Keys(change, {"atMs", "stream"});
        const int at = Integer(change, "atMs", -1, 0, 86400000);
        if (at <= previous || !change.contains("stream")) throw std::invalid_argument("Settings changes must be ordered and complete");
        previous = at; result.changes.push_back({std::chrono::milliseconds(at), ParseStreamPreferences(Object(change, "stream"))});
    }
    return result;
}
