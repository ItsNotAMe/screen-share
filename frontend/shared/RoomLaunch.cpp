#include "shared/RoomLaunch.h"
#include "shared/StreamPreferencesJson.h"
#include <QFile>
#include <QSet>
#include <QMap>
#include <QStringDecoder>
#include <stdexcept>
#include <utility>

namespace {
QMap<QString, QString> Options(const QStringList& args, const QSet<QString>& flags, const QSet<QString>& values) {
    QMap<QString, QString> result;
    for (qsizetype i = 0; i < args.size(); ++i) {
        const auto& key = args[i];
        if (result.contains(key)) throw std::invalid_argument("Duplicate room option");
        if (flags.contains(key)) result[key] = {};
        else if (values.contains(key)) {
            if (++i >= args.size() || args[i].startsWith("--")) throw std::invalid_argument("Missing room option value");
            result[key] = args[i];
        } else throw std::invalid_argument("Unsupported room option; legacy ports, invites and remote control cannot be translated to v2");
    }
    if (result.value("--backend") != "v2") throw std::invalid_argument("Room adoption requires --backend v2; omit it for the existing application");
    if (!result.contains("--signal-server")) throw std::invalid_argument("Specify --signal-server with the v2 HTTPS service origin");
    return result;
}
int Number(const QString& value) {
    bool ok = false; const int number = value.toInt(&ok);
    if (!ok) throw std::invalid_argument("Invalid numeric room option");
    return number;
}
QString Password(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024) throw std::invalid_argument("Cannot read room password file (maximum 1024 bytes)");
    auto bytes = file.read(1025);
    if (bytes.size() > 1024) throw std::invalid_argument("Room password file exceeds limit");
    // Permit the one line ending commonly added by text editors. Preserve spaces.
    if (bytes.endsWith('\n')) { bytes.chop(1); if (bytes.endsWith('\r')) bytes.chop(1); }
    QStringDecoder decoder(QStringDecoder::Utf8); const QString password = decoder.decode(bytes);
    if (decoder.hasError() || password.isEmpty() || password.contains('\n') || password.contains('\r'))
        throw std::invalid_argument("Password file must contain one nonempty UTF-8 line");
    if (password.toUtf8().size() > 128) throw std::invalid_argument("Room password exceeds 128 UTF-8 bytes");
    for (const auto c : password) if (c.unicode() < 0x20 || c.unicode() == 0x7f)
        throw std::invalid_argument("Room password contains a control character");
    return password;
}
}
QUrl ParseRoomHomeLaunch(const QStringList& arguments) {
    const auto options = Options(arguments, {}, {"--backend", "--signal-server"});
    const auto origin = options.value("--signal-server");
    ParseRoomSessionConfig({{"origin", origin}, {"host", true}});
    return QUrl(origin);
}
RoomSessionConfig ParseRoomCommand(const QStringList& arguments, const RoomProfile* defaults, bool loopback) {
    const auto options = Options(arguments, {"--create-room", "--private", "--no-preview", "--mute", "--unmute"},
        {"--backend", "--signal-server", "--join-room", "--nickname", "--name", "--password-file", "--viewer-limit",
         "--seconds", "--display", "--window", "--audio", "--audio-device", "--process-id", "--playback-device", "--volume",
         "--preset", "--resolution", "--fps", "--bitrate", "--upload-bps", "--control-file", "--gamepad", "--report"});
    const bool host = options.contains("--create-room");
    if (host == options.contains("--join-room")) throw std::invalid_argument("Choose exactly one of --create-room or --join-room");
    const QSet<QString> hostOnly{"--private", "--name", "--viewer-limit", "--display", "--window", "--audio", "--audio-device",
        "--process-id", "--preset", "--resolution", "--fps", "--bitrate", "--upload-bps"};
    const QSet<QString> viewerOnly{"--no-preview", "--mute", "--unmute", "--playback-device", "--volume", "--gamepad"};
    if (options.contains("--mute") && options.contains("--unmute")) throw std::invalid_argument("Choose either --mute or --unmute");
    for (auto it = options.begin(); it != options.end(); ++it)
        if ((!host && hostOnly.contains(it.key())) || (host && viewerOnly.contains(it.key())))
            throw std::invalid_argument("Room option is not applicable to the selected role");
    const auto nickname = RoomProfile::normalizeNickname(options.value("--nickname", defaults ? defaults->nickname() : QString("Guest")));
    if (!nickname) throw std::invalid_argument("Invalid room nickname");
    QJsonObject input{{"origin", options.value("--signal-server")}, {"host", host}, {"nickname", *nickname},
        {"name", options.value("--name", "My room")}, {"public", !options.contains("--private")},
        {"preview", !options.contains("--no-preview")}};
    if (!host) input["roomId"] = options.value("--join-room");
    if (options.contains("--password-file")) input["password"] = Password(options.value("--password-file"));
    for (const auto& pair : {std::pair{"--viewer-limit", "viewerLimit"}, {"--seconds", "seconds"}})
        if (options.contains(pair.first)) input[pair.second] = Number(options.value(pair.first));
    QJsonObject capture;
    if (options.contains("--display")) capture["display"] = Number(options.value("--display"));
    if (options.contains("--window")) capture["window"] = options.value("--window");
    input["capture"] = capture;
    QJsonObject audio;
    for (const auto& pair : {std::pair{"--audio", "source"}, {"--audio-device", "deviceId"}, {"--playback-device", "playbackDeviceId"}})
        if (options.contains(pair.first)) audio[pair.second] = options.value(pair.first);
    if (options.contains("--process-id")) audio["processId"] = Number(options.value("--process-id"));
    const auto playback = defaults ? defaults->playback() : RoomProfile::Playback{};
    audio["playbackVolume"] = options.contains("--volume") ? Number(options.value("--volume")) : playback.volume;
    audio["playbackMuted"] = options.contains("--mute") || (!options.contains("--unmute") && playback.muted);
    input["audio"] = audio;
    auto stream = StreamPreferencesJson(defaults ? defaults->streamPreferences() : screenshare::media::StreamPreferences{});
    if (options.contains("--preset")) stream["preset"] = options.value("--preset");
    if (options.contains("--resolution")) {
        const auto value = options.value("--resolution");
        if (value == "auto" || value == "native") stream["resolution"] = value;
        else {
            const auto parts = value.split('x');
            if (parts.size() != 2) throw std::invalid_argument("Resolution must be auto, native or WIDTHxHEIGHT");
            stream["resolution"] = "fixed"; stream["width"] = Number(parts[0]); stream["height"] = Number(parts[1]);
        }
    }
    for (const auto& pair : {std::pair{"--fps", "fps"}, {"--bitrate", "bitrateBps"}}) {
        if (!options.contains(pair.first)) continue;
        const bool automatic = options.value(pair.first) == "auto";
        stream[QString(pair.first) == "--fps" ? "fpsMode" : "bitrateMode"] = automatic ? "auto" : "manual";
        if (!automatic) stream[pair.second] = Number(options.value(pair.first));
        else if (QString(pair.first) == "--bitrate") stream.remove("bitrateBps");
    }
    if (options.contains("--upload-bps")) stream["aggregateUploadBps"] = Number(options.value("--upload-bps"));
    input["stream"] = stream;
    auto config = ParseRoomSessionConfig(input, loopback);
    config.inputCommandsFile = options.value("--control-file");
    config.gamepadDevice = options.value("--gamepad");
    config.reportFile = options.value("--report");
    if (options.contains("--report") && (config.reportFile.trimmed().isEmpty() || config.reportFile.size() > 4096 || config.reportFile.contains(QChar(0))))
        throw std::invalid_argument("Invalid diagnostic report path");
    if ((options.contains("--control-file") && config.inputCommandsFile.isEmpty()) ||
        config.inputCommandsFile.size() > 4096 || config.gamepadDevice.size() > 4096)
        throw std::invalid_argument("Invalid controller command file or device");
    if (!config.gamepadDevice.isEmpty() && config.inputCommandsFile.isEmpty())
        throw std::invalid_argument("Viewer controller control requires --control-file");
    if (!host && !config.inputCommandsFile.isEmpty() && !config.preview)
        throw std::invalid_argument("Interactive input control requires a focused preview");
    return config;
}
