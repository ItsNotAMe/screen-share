#include "cli/RoomCli.h"
#include "shared/LatestRoomVideoFrame.h"
#include "shared/RoomLaunch.h"
#include "shared/RoomInputCommands.h"
#include "RecordingGamepadSink.h"
#include "RecordingDesktopInput.h"
#include <QTemporaryDir>
#include <QFile>
#include "media/webrtc/NativeRoomRuntime.h"
#include "media/webrtc/MfVideoEncoderFactory.h"
#include "media/webrtc/MfVideoDecoderFactory.h"
#include "media/webrtc/PcmAudioDeviceModule.h"
#include "../tools/webrtc-proof/SyntheticAudio.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/win32_socket_init.h"
#include "rtc_base/logging.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <atomic>
#include <iostream>
#include <mutex>
#include <utility>
#include <source_location>
#include "api/make_ref_counted.h"
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
#include "../tools/webrtc-proof/CaptureTestWindow.h"
#include "core/WindowsMediaRuntime.h"
#include "render/ReceiverPreviewWindow.h"
#include <dxgi.h>
HWND captureWindow = nullptr;
#endif

using namespace screenshare::media;
using namespace screenshare::v2;
using namespace std::chrono_literals;
void Check(bool value, std::source_location where = std::source_location::current()) {
    if (!value) throw std::runtime_error("Room CLI integration failed at line " + std::to_string(where.line()));
}
template<class F> void Reject(F fn) {
    bool rejected = false;
    try { fn(); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected);
}
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
struct PreviewEvidence {
    bool failPresent = false, failUpdate = false;
    unsigned calls = 0, resets = 0;
    screenshare::Nv12D3D11Presenter::ScaleMode scale = screenshare::Nv12D3D11Presenter::ScaleMode::Fit;
};
class PreviewRenderer final : public FramePresentationBackend {
    std::unique_ptr<FramePresentationBackend> native_ = CreateNativeFramePresentation();
    std::shared_ptr<PreviewEvidence> evidence_;
public:
    explicit PreviewRenderer(std::shared_ptr<PreviewEvidence> evidence) : evidence_(std::move(evidence)) {}
    bool Present(HWND window, uint32_t width, uint32_t height, bool smooth, bool lowLatency,
        const screenshare::Nv12D3D11Presenter::FrameView& frame, screenshare::Nv12D3D11Presenter::ScaleMode scale) override {
        ++evidence_->calls; evidence_->scale = scale;
        const bool result = native_->Present(window, width, height, smooth, lowLatency, frame, scale);
        if (std::exchange(evidence_->failPresent, false))
            throw screenshare::PresentationError(DXGI_ERROR_DEVICE_REMOVED, "Injected preview device loss");
        return result;
    }
    void Update(HWND window, uint32_t width, uint32_t height, bool smooth, bool lowLatency,
        screenshare::Nv12D3D11Presenter::ScaleMode scale) override {
        ++evidence_->calls; evidence_->scale = scale;
        native_->Update(window, width, height, smooth, lowLatency, scale);
        if (std::exchange(evidence_->failUpdate, false))
            throw screenshare::PresentationError(DXGI_ERROR_DEVICE_RESET, "Injected preview resize loss");
    }
    void Reset() noexcept override { ++evidence_->resets; native_->Reset(); }
    uint32_t MaximumFrameLatency() const noexcept override { return native_->MaximumFrameLatency(); }
    screenshare::PresentationOutcome LastOutcome() const noexcept override { return native_->LastOutcome(); }
};
void PreviewLifecycle() {
    auto evidence = std::make_shared<PreviewEvidence>();
    screenshare::ReceiverPreviewWindow preview([evidence] { return std::make_unique<PreviewRenderer>(evidence); });
    preview.SetLowLatency(true); preview.Show();
    Check(IsWindowVisible(preview.windowHandle()));
    screenshare::Nv12VideoFrame frame; frame.width = 320; frame.height = 180; frame.nv12.resize(320 * 180 * 3 / 2, 128);
    auto resume = [&](std::source_location caller = std::source_location::current()) {
        const auto goal = preview.framesPresented() + 3;
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (preview.framesPresented() < goal) {
            if (!preview.PumpMessages() || std::chrono::steady_clock::now() >= deadline) {
                const auto stats = preview.presentationStats();
                RECT windowRect{}, clientRect{};
                GetWindowRect(preview.windowHandle(), &windowRect); GetClientRect(preview.windowHandle(), &clientRect);
                throw std::runtime_error("Preview presentation timeout from line " + std::to_string(caller.line()) +
                    "; frames=" + std::to_string(preview.framesPresented()) + "; errors=" + std::to_string(stats.errors) +
                    "; recoveries=" + std::to_string(stats.recoveries) + "; terminal=" + std::to_string(stats.terminal) +
                    "; outcome=" + screenshare::PresentationOutcomeName(stats.outcome) +
                    "; lastError=" + std::to_string(uint32_t(stats.lastError)) +
                    "; busy=" + std::to_string(stats.busyDrops) + "; occluded=" + std::to_string(stats.occludedDrops) +
                    "; visible=" + std::to_string(IsWindowVisible(preview.windowHandle())) +
                    "; minimized=" + std::to_string(IsIconic(preview.windowHandle())) +
                    "; rect=" + std::to_string(windowRect.left) + "," + std::to_string(windowRect.top) + "," +
                        std::to_string(windowRect.right) + "," + std::to_string(windowRect.bottom) +
                    "; client=" + std::to_string(clientRect.right) + "x" + std::to_string(clientRect.bottom) +
                    "; onMonitor=" + std::to_string(MonitorFromWindow(preview.windowHandle(), MONITOR_DEFAULTTONULL) != nullptr));
            }
            preview.PresentFrame(frame); std::this_thread::sleep_for(5ms);
        }
        Check(preview.maximumFrameLatency() == 1 && !preview.presentationStats().terminal);
    };
    resume();
    frame.inputMapping={77,320,180,0,0,320,180};resume();
    std::vector<screenshare::input::Event> remote;
    preview.SetRemoteInput(screenshare::input::Mouse|screenshare::input::Keyboard,[&](const auto& event){remote.push_back(event);});
    RECT inputRect{};GetClientRect(preview.windowHandle(),&inputRect);
    SendMessageW(preview.windowHandle(),WM_MOUSEMOVE,0,MAKELPARAM(inputRect.right/2,inputRect.bottom/2));
    SendMessageW(preview.windowHandle(),WM_KEYDOWN,'A',LPARAM(0x1e)<<16);
    SendMessageW(preview.windowHandle(),WM_KEYUP,'A',LPARAM(0x1e)<<16);
    Check(remote.size()==3 && remote[0].sourceGeneration==77 && remote[1].kind==screenshare::input::Kind::Key && !remote[2].down);
    SendMessageW(preview.windowHandle(),WM_LBUTTONUP,0,MAKELPARAM(0xffff,0xffff));
    Check(remote.back().kind==screenshare::input::Kind::Release);
    preview.SetRemoteInput(0,{});
    // Only direct messages to our own HWND; no global keyboard/mouse injection.
    const HWND window = preview.windowHandle();
    SendMessageW(window, WM_KEYDOWN, '1', 0);
    Check(evidence->scale == screenshare::Nv12D3D11Presenter::ScaleMode::OriginalSize);
    SendMessageW(window, WM_KEYDOWN, 'F', 0);
    Check(evidence->scale == screenshare::Nv12D3D11Presenter::ScaleMode::Fit);
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    SendMessageW(window, WM_KEYDOWN, VK_F11, 0);
    Check((GetWindowLongPtrW(window, GWL_STYLE) & WS_OVERLAPPEDWINDOW) == 0);
    SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
    // SetWindowPlacement/ShowWindow manage WS_VISIBLE independently of the
    // restored frame style (the generated test window may initially be hidden).
    Check(((GetWindowLongPtrW(window, GWL_STYLE) ^ style) & ~LONG_PTR(WS_VISIBLE)) == 0);
    int mute = 0, volume = 0;
    preview.SetControlCallbacks({[&] { ++mute; }, [&](int delta) { volume += delta; }});
    SendMessageW(window, WM_KEYDOWN, 'M', 0); SendMessageW(window, WM_KEYDOWN, VK_ADD, 0);
    Check(mute == 1 && volume == 5);
    ShowWindow(window, SW_MINIMIZE);
    const auto presented = preview.framesPresented(); preview.PresentFrame(frame);
    Check(preview.framesPresented() == presented);
    Check(preview.presentationStats().outcome == screenshare::PresentationOutcome::Minimized && preview.presentationStats().minimizedDrops == 1);
    ShowWindow(window, SW_RESTORE); resume();
    auto malformed = frame; malformed.width = 321;
    Reject([&] { preview.PresentFrame(malformed); });
    Check(!preview.presentationStats().terminal);
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
        if (attempt == 2) {
            evidence->failUpdate = true;
            SendMessageW(window, WM_SIZE, SIZE_RESTORED, MAKELPARAM(480, 270));
        } else {
            evidence->failPresent = true; preview.PresentFrame(frame);
        }
        Check(preview.presentationStats().recoveries == attempt);
        Check(preview.presentationStats().lastError == (attempt == 2 ? DXGI_ERROR_DEVICE_RESET : DXGI_ERROR_DEVICE_REMOVED));
        const auto calls = evidence->calls;
        for (int drop = 0; drop < 50; ++drop) preview.PresentFrame(frame);
        Check(evidence->calls == calls);
        resume();
    }
    evidence->failPresent = true; preview.PresentFrame(frame);
    Check(preview.presentationStats().terminal && preview.presentationStats().errors == 4);
    const auto calls = evidence->calls;
    for (int drop = 0; drop < 100; ++drop) preview.PresentFrame(frame);
    SendMessageW(window, WM_SIZE, SIZE_RESTORED, MAKELPARAM(640, 360));
    Check(evidence->calls == calls);
    preview.SetStatusText("Connected");
    wchar_t title[256]{}; GetWindowTextW(window, title, 256);
    Check(std::wstring(title).find(L"Leave and rejoin") != std::wstring::npos);
    preview.ClearFrame(); resume();
    Check(preview.presentationStats().recoveries == 0);
    screenshare::DecodedFrameInfo legacy; legacy.width = 320; legacy.height = 180;
    legacy.data.resize(frame.nv12.size(), std::byte{128}); preview.PresentFrame(legacy);
    // Closing a preview must not post WM_QUIT into another window's pump.
    screenshare::ReceiverPreviewWindow sibling; sibling.SetLowLatency(true); sibling.Show();
    SendMessageW(window, WM_CLOSE, 0, 0);
    Check(preview.closeRequested() && !IsWindow(window) && sibling.PumpMessages());
    sibling.PresentFrame(frame);
}
#endif
class CheckedNv12 : public webrtc::NV12BufferInterface {
    std::atomic<int>& destroyed_;
    const int stride_;
    std::vector<uint8_t> data_;
public:
    CheckedNv12(std::atomic<int>& destroyed, int stride) : destroyed_(destroyed), stride_(stride), data_(stride * 3, 0xee) {
        for (int y = 0; y < 2; ++y) std::fill_n(data_.data() + y * stride_, 4, uint8_t(42));
        for (int x = 0; x < 4; ++x) data_[stride_ * 2 + x] = x % 2 ? 190 : 90;
    }
    ~CheckedNv12() override { ++destroyed_; }
    int width() const override { return 4; }
    int height() const override { return 2; }
    int StrideY() const override { return stride_; }
    int StrideUV() const override { return stride_; }
    const uint8_t* DataY() const override { return data_.data(); }
    const uint8_t* DataUV() const override { return data_.data() + stride_ * 2; }
    webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override { throw std::runtime_error("NV12 unexpectedly converted to I420"); }
};
void PresentationOwnership() {
    LatestRoomVideoFrame sink;
    std::atomic<int> destroyed{0};
    auto buffer = webrtc::make_ref_counted<CheckedNv12>(destroyed, 4); const auto* pixels = buffer->DataY();
    sink.OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer).set_timestamp_us(9).build());
    auto frame = sink.Take(); buffer = nullptr;
    Check(frame && frame->retainedPixels && frame->nv12.empty() && frame->pixels().data() == pixels && frame->timestamp100ns == 90 && destroyed == 0);
    auto retained = *frame; frame.reset(); Check(retained.pixels()[0] == 42 && destroyed == 0);
    retained = {}; Check(destroyed == 1);
    buffer = webrtc::make_ref_counted<CheckedNv12>(destroyed, 8);
    sink.OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer).build()); frame = sink.Take(); buffer = nullptr;
    Check(frame && !frame->retainedPixels && destroyed == 2 && frame->pixels().size() == 12);
    for (size_t i = 0; i < 12; ++i) Check(frame->pixels()[i] == (i < 8 ? 42 : i % 2 ? 190 : 90));
    auto planar = webrtc::I420Buffer::Create(4, 2);
    std::fill_n(planar->MutableDataY(), 8, uint8_t(55)); std::fill_n(planar->MutableDataU(), 2, uint8_t(70)); std::fill_n(planar->MutableDataV(), 2, uint8_t(180));
    sink.OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(planar).build()); frame = sink.Take();
    Check(frame && frame->pixels()[0] == 55 && frame->pixels()[8] == 70 && frame->pixels()[9] == 180);
    Check(sink.statistics().retained == 1 && sink.statistics().repacked == 1 && sink.statistics().converted == 1);
    sink.OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(planar).set_rotation(webrtc::kVideoRotation_90).build());
    bool rejected = false; try { sink.Take(); } catch (const std::runtime_error&) { rejected = true; } Check(rejected);
    for (int i = 0; i < 100; ++i) {
        auto pending = webrtc::make_ref_counted<CheckedNv12>(destroyed, 4);
        sink.OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(pending).build());
    }
    Check(destroyed == 101 && sink.statistics().replaced == 99);
    sink.Stop(); Check(destroyed == 102 && !sink.Take());
    sink.OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(planar).build());
    Check(!sink.Take() && sink.statistics().rejectedAfterStop == 1);
}
RoomRuntimeFactory Factory(const RoomSessionConfig& config, std::shared_ptr<proof::AudioEvidence> audio,
                           std::shared_ptr<LatestRoomVideoFrame> frames = {}) {
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
    auto options = config.media;
    options.capture.sourceType = screenshare::CaptureSourceType::Window;
    options.capture.windowHandle = reinterpret_cast<uint64_t>(captureWindow);
    options.audioEndpoints = proof::SyntheticAudio(audio); options.frames = frames;
    options.audioForSelection = [audio](auto selection) { return proof::SyntheticAudioSelectionWithEvidence(selection, audio); };
    options.playbackForSelection = [audio](auto selection) { return proof::SyntheticPlayback(selection, audio); };
    return WindowsRoomRuntimeFactory(std::move(options));
#else
    return [preferences = config.media.preferences, initialAudio = config.media.audio, presentation = config.media.presentation, inputSink = config.media.inputSink, target = config.media.inputTarget, audio, frames](auto identity, auto send) {
        NativeRoomRuntimeOptions options;
        options.preferences = preferences; options.frames = frames; options.presentation = presentation;
        options.inputSink = inputSink;
        auto endpoints = proof::SyntheticAudio(audio);
        if (!identity.host) {
            options.playback = std::make_shared<PlaybackControl>(PlaybackSelection{}, endpoints.playout);
            endpoints.playout = [control = options.playback] { return std::make_unique<ControlledPcmPlayout>(control); };
            options.playbackForSelection = [audio](auto selection) { return proof::SyntheticPlayback(selection, audio); };
        }
        if (identity.host) {
            options.audioSwitch = std::make_shared<AudioSwitchControl>(AudioSelection{
                initialAudio.source == screenshare::AudioCaptureSource::None ? AudioKind::None : AudioKind::System}, endpoints.capture, ProcessMicrophone);
            endpoints.capture = [control = options.audioSwitch] { return std::make_unique<SwitchablePcmCapture>(control); };
            options.audioForSelection = [audio](auto selection) { return proof::SyntheticAudioSelectionWithEvidence(selection, audio); };
        }
        options.engine = [endpoints] {
            return std::make_unique<MediaEngine>(CreatePcmAudioDeviceModule(endpoints,
                std::make_shared<PcmAudioDiagnostics>()), std::make_unique<MfVideoEncoderFactory>(), std::make_unique<MfVideoDecoderFactory>());
        };
        options.capture = [] { return std::make_unique<SyntheticCaptureSource>(320, 180, 30); };
        if(target)options.capture=[target] {return std::make_unique<MappedSyntheticCapture>(320,180,30,target);};
        options.captureForSelection = [](CaptureSelection selection) -> CaptureSession::Factory {
            return [selection] { return std::make_unique<SyntheticCaptureSource>(640, 360, selection.fps); };
        };
        options.deliver = [](auto& source, const auto& sample) {
            source.Push(*std::static_pointer_cast<SyntheticCaptureResource>(sample.resource), sample.capturedAt,sample.resource->inputGeneration);
        };
        if(target)options.captureForSelection=[target](auto selection)->CaptureSession::Factory {return [target,selection] {return std::make_unique<MappedSyntheticCapture>(640,360,selection.fps,target);};};
        return CreateNativeRoomRuntime(identity, std::move(send), std::move(options));
    };
#endif
}
void CommandScenario() {
    const QStringList base{"--backend", "v2", "--signal-server", "https://example.test", "--create-room"};
    auto config = ParseRoomCommand(base + QStringList{"--nickname", "  Player  ", "--resolution", "1920x1080", "--fps", "144", "--bitrate", "9000000"});
    Check(config.room.host && config.room.nickname == "Player" && config.media.preferences.resolution == ResolutionMode::Fixed &&
        config.media.preferences.width == 1920 && config.media.preferences.fps == 144 &&
        config.media.preferences.bitrateMode == SettingMode::Manual && config.media.preferences.bitrateLimitBps == 9000000);
    for (const QStringList extra : {QStringList{"--backend", "v2"}, {"--join-room", "room"}, {"--display", "0", "--window", "1"},
        {"--resolution", "1921x1080"}, {"--fps", "300"}, {"--bitrate", "-1"}, {"--seconds", "1.5"}, {"--no-preview"},
        {"--watch", "5000"}, {"--signal-room", "chosen-id"}, {"--remote-control"}, {"--nickname", QString(33, 'a')}, {"--nickname"}})
        Reject([&] { ParseRoomCommand(base + extra); });
    Reject([&] { ParseRoomCommand({"--create-room", "--signal-server", "https://example.test"}); });
    Reject([&] { ParseRoomCommand({"--backend", "v2", "--create-room"}); });
    Reject([&] { ParseRoomHomeLaunch({"--backend", "v2", "--signal-server", "http://127.0.0.1"}); });
    Check(ParseRoomHomeLaunch({"--signal-server", "https://example.test", "--backend", "v2"}).host() == "example.test");
    for (const auto& origin : {"https://name:secret@example.test", "https://example.test/path", "https://example.test/?token=value"})
        Reject([&] { ParseRoomHomeLaunch({"--backend", "v2", "--signal-server", origin}); });
    QTemporaryDir files; Check(files.isValid());
    RoomProfile profile(files.filePath("defaults.ini")); Check(profile.saveNickname("Stored player"));
    auto prefs = config.media.preferences; Check(profile.saveStreamPreferences(prefs));
    auto inherited = ParseRoomCommand(base, &profile);
    Check(inherited.room.nickname == "Stored player" && inherited.media.preferences.fps == 144 && inherited.media.preferences.bitrateLimitBps == 9000000);
    auto automatic = ParseRoomCommand(base + QStringList{"--fps", "auto", "--bitrate", "auto", "--resolution", "auto"}, &profile);
    Check(automatic.media.preferences.fpsMode == SettingMode::Auto && !automatic.media.preferences.bitrateLimitBps &&
        automatic.media.preferences.resolution == ResolutionMode::Auto && profile.streamPreferences().fps == 144);
    const auto secretFile = files.filePath("password.txt");
    auto write = [&](const QByteArray& bytes) { QFile file(secretFile); Check(file.open(QIODevice::WriteOnly)); Check(file.write(bytes) == bytes.size()); };
    write(" preserved spaces \r\n");
    Check(ParseRoomCommand(base + QStringList{"--password-file", secretFile}).room.password == " preserved spaces ");
    for (const auto& invalid : {QByteArray(1025, 'x'), QByteArray(129, 'x'), QByteArray("a\nb"), QByteArray("a\0b", 3), QByteArray("\xff", 1)}) {
        write(invalid); Reject([&] { ParseRoomCommand(base + QStringList{"--password-file", secretFile}); });
    }
    const QStringList join{"--backend", "v2", "--signal-server", "https://example.test", "--join-room", "screenshare://room/v2/room_1"};
    Check(ParseRoomCommand(join + QStringList{"--no-preview", "--volume", "25", "--mute"}).media.playbackMuted);
    Check(profile.savePlayback({25, true}));
    Check(!ParseRoomCommand(join + QStringList{"--unmute"}, &profile).media.playbackMuted && profile.playback().muted);
    Reject([&] { ParseRoomCommand(join + QStringList{"--mute", "--unmute"}); });
    Reject([&] { ParseRoomCommand(join + QStringList{"--audio", "microphone"}); });
    Reject([&] { ParseRoomCommand(join + QStringList{"--volume", "101"}); });
}
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
    webrtc::InitializeLogging(std::move(logging));
    webrtc::WinsockInitializer winsock;
    if (winsock.error() || !webrtc::InitializeSSL()) return 1;
    int exitCode = 0;
    try {
        Check(argc == 2);
        CommandScenario();
        PresentationOwnership();
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
        screenshare::WindowsMediaRuntime mediaRuntime;
        Check(SUCCEEDED(mediaRuntime.result()));
        PreviewLifecycle();
        proof::TestWindow capture;
        captureWindow = capture.handle();
#endif
        QJsonObject stream{{"resolution", "fixed"}, {"width", 320}, {"height", 180}, {"fps", 30}, {"aggregateUploadBps", 2000000}};
        auto reduced = stream; reduced["width"] = 160; reduced["height"] = 90; reduced["fps"] = 20;
        QJsonObject captureChange{{"atMs", 2000}, {"display", 1}, {"fps", 30}};
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
        captureChange.remove("display"); captureChange["window"] = QString::number(reinterpret_cast<uint64_t>(captureWindow));
#endif
        QJsonObject object{{"origin", argv[1]}, {"host", true}, {"nickname", "CliHost"}, {"name", "CLI media"},
            {"seconds", 15}, {"stream", stream}, {"password", "test-only-password"},
            {"changes", QJsonArray{QJsonObject{{"atMs", 3000}, {"stream", reduced}}}}};
        object["captureChanges"] = QJsonArray{captureChange};
        object["audio"] = QJsonObject{{"source", "none"}};
        object["audioChanges"] = QJsonArray{QJsonObject{{"atMs", 2500}, {"source", "system"}},
            QJsonObject{{"atMs", 3500}, {"source", "microphone"}}, QJsonObject{{"atMs", 5000}, {"source", "system"}}};
        auto invalidAudio = object; invalidAudio["audioChanges"] = QJsonArray{QJsonObject{{"atMs", 100}, {"source", "process"}}};
        Reject([&] { ParseRoomSessionConfig(invalidAudio, true); });
        invalidAudio["audioChanges"] = QJsonArray{QJsonObject{{"atMs", 100}, {"source", "system"}, {"processId", 1}}};
        Reject([&] { ParseRoomSessionConfig(invalidAudio, true); });
        Reject([&] { ParseRoomSessionConfig(object); }); // Production cannot opt into plaintext.
        for (const auto& extra : {QJsonObject{{"deviceId", "default"}}, QJsonObject{{"processId", 1}}}) {
            auto forbidden = extra; forbidden["source"] = "none";
            auto invalid = object; invalid["audio"] = forbidden;
            Reject([&] { ParseRoomSessionConfig(invalid, true); });
            invalid = object; forbidden["atMs"] = 100; invalid["audioChanges"] = QJsonArray{forbidden};
            Reject([&] { ParseRoomSessionConfig(invalid, true); });
        }
        auto silentChange = object; silentChange["audioChanges"] = QJsonArray{QJsonObject{{"atMs", 100}, {"source", "none"}}};
        Check(ParseRoomSessionConfig(silentChange, true).audioChanges[0].selection.kind == AudioKind::None);
        auto malformed = object; malformed["unknown"] = true;
        Reject([&] { ParseRoomSessionConfig(malformed, true); });
        malformed = object; malformed["seconds"] = 1.5;
        Reject([&] { ParseRoomSessionConfig(malformed, true); });
        malformed = object; malformed["changes"] = QJsonArray{QJsonObject{{"stream", stream}}};
        Reject([&] { ParseRoomSessionConfig(malformed, true); });
        malformed = object; malformed["capture"] = QJsonObject{{"display", 0}, {"window", "0x1234"}};
        Reject([&] { ParseRoomSessionConfig(malformed, true); });
        malformed = object; malformed["origin"] = "https://user:password@example.com";
        Reject([&] { ParseRoomSessionConfig(malformed); });
        const auto scripted = ParseRoomSessionConfig(object, true);
        QTemporaryDir commandFiles; Check(commandFiles.isValid());
        const auto passwordPath = commandFiles.filePath("password.txt");
        { QFile file(passwordPath); Check(file.open(QIODevice::WriteOnly)); Check(file.write("test-only-password") == 18); }
        QStringList hostArguments{"--backend", "v2", "--signal-server", argv[1], "--create-room", "--name", "CLI media", "--nickname", "CliHost",
            "--password-file", passwordPath, "--seconds", "15", "--resolution", "320x180", "--fps", "30", "--upload-bps", "2000000", "--audio", "none", "--report", commandFiles.filePath("report.json")};
        auto host = ParseRoomCommand(hostArguments, nullptr, true);
        host.changes = scripted.changes; host.captureChanges = scripted.captureChanges; host.audioChanges = scripted.audioChanges;
        std::mutex mutex; std::string roomId;
        std::atomic<bool> stopHost{false}, applied{false}, stopped{false}, accepted{false}, budgetReported{false}, rateReported{false}, receiverReported{false}, senderReported{false}, sourceChanged{false}, audioChanged{false};
        auto hostAudio = std::make_shared<proof::AudioEvidence>();
        std::atomic<bool> extendedReported{false}, pipelineReported{false};
        std::atomic<bool> microphoneReported{false}, bypassRestored{false};
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
        std::atomic<bool> presentationReported{false};
#endif
        RoomCliHooks hostHooks;
        hostHooks.pump = [&] { return !stopHost; };
        hostHooks.report = [&](const QJsonObject& value) {
            const auto encoded = QJsonDocument(value).toJson();
            Check(!encoded.contains("test-only-password") && !encoded.contains("token"));
            if (value["type"] == "settings") { Check(value["error"].toInt() == 0); accepted = true; }
            if (value["type"] == "capture") { Check(value["error"].toInt() == 0 && value["revision"].toInt() == 2); sourceChanged = true; }
            if (value["type"] == "audio") { Check(value["error"].toInt() == 0 && value["revision"].toInt() >= 2 && value["revision"].toInt() <= 4); audioChanged = true; }
            if (value["microphoneProcessing"].toBool()) { Check(value["audioSource"] == "microphone"); microphoneReported = true; }
            if (microphoneReported && value["audioSource"] == "system" && !value["microphoneProcessing"].toBool()) bypassRestored = true;
            if (value["phase"] == "active") {
                std::lock_guard lock(mutex); roomId = value["roomId"].toString().toStdString();
            }
            if (value["pipeline"].toObject()["captureState"] == "running" &&
                value["settingsApplication"].toObject()["applied"].toInt() == 1) pipelineReported = true;
            for (const auto& peer : value["peers"].toArray()) {
                const auto row = peer.toObject();
                const auto source = row["source"].toObject();
                if (row["observedRevision"].toInteger() && row["width"].toInt() == 160) {
                    const auto image = source["activeImage"].toObject();
#ifndef SCREENSHARE_WINDOWS_CLI_PROOF
                    Check(image["width"].toInt() == 160 && image["height"].toInt() == 90);
                    Check(source["scalingPath"] == "cpu");
#else
                    // WGC captures the taller generated window; it must be
                    // pillarboxed, unlike the synthetic 16:9 fixture.
                    const int width = image["width"].toInt(), left = image["left"].toInt();
                    Check(image["top"].toInt() == 0 && image["height"].toInt() == 90 && width > 0 && width < 160);
                    Check(left > 0 && left % 2 == 0 && width % 2 == 0 && std::abs(160 - width - 2 * left) <= 2);
#endif
                }
                Check(row["sender"].isObject() && row["sender"].toObject().contains("limitingReason"));
                if (row["sender"].toObject()["videoPayloadBps"].toDouble() > 0 && row["sender"].toObject()["rttMs"].isDouble()) senderReported = true;
                Check(!row["sender"].toObject().contains("candidateId"));
                const auto receiver = row["receiver"].toObject();
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
                const auto expectedDecoder = "mf-h264-hardware";
#else
                const auto expectedDecoder = "mf-h264-software";
#endif
                if (receiver["decoder"] == expectedDecoder && row["settingsError"] == "none" &&
                    row["appliedPreferences"].isObject() && row["captureDelivery"].toObject()["delivered"].toInteger() > 0 &&
                    row["recovery"].toObject()["state"] == "connected") extendedReported = true;
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
                if (receiver["presentation"].toObject()["presented"].toInteger() > 0) presentationReported = true;
#else
                Check(receiver["presentation"].isNull()); // No renderer attached: decoded is not displayed.
#endif
                if (receiver["sampleState"] == "fresh" && receiver["width"].toInt() == 160 && receiver["framesDecoded"].toInteger() > 0) receiverReported = true;
                if (row["allocatedVideoBps"].toInt() == 1472000 && row["appliedVideoBps"].toInt() == 1472000) budgetReported = true;
                if (row["transportSendBps"].toDouble() > 0 && row["transportSampleState"] == "fresh" &&
                    row["requestedRevision"] == value["requestedRevision"] && value["requestedPreferences"].isObject()) rateReported = true;
                if (row["width"].toInt() == 160 && row["appliedRevision"] == row["observedRevision"] && !row["rejected"].toBool()) applied = true;
            }
            if (value["phase"] == "stopped") stopped = true;
        };
        host.inputCommandsFile = commandFiles.filePath("host.json");
        const auto viewerCommands = commandFiles.filePath("viewer.json");
        auto writeCommand = [](const QString& path, int sequence, const char* operation, const std::string& peer, const char* caps="gamepad") {
            QFile file(path); Check(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const auto data = QJsonDocument(QJsonObject{{"sequence", sequence}, {"operation", operation},
                {"peer", QString::fromStdString(peer)}, {"consent", true}, {"capabilities",caps}}).toJson(QJsonDocument::Compact);
            Check(file.write(data) == data.size()); file.close();
        };
        // These persisted grants/requests must be ignored by a new session.
        writeCommand(host.inputCommandsFile, 1, "grant", "old-peer");
        writeCommand(viewerCommands, 1, "request", "old-host");
        auto controllerSink = std::make_shared<RecordingGamepadSink>();
        auto desktopEvidence=std::make_shared<DesktopInputEvidence>();host.media.inputTarget=std::make_shared<screenshare::input::DesktopTargetState>();
        host.media.inputSink=std::make_shared<screenshare::input::DesktopSink>(host.media.inputTarget,controllerSink,
            [desktopEvidence](auto,uint8_t){return std::make_unique<RecordingDesktopDevice>(desktopEvidence);});
        bool grantWritten = false, revokeWritten = false, mouseGrantWritten=false;
        hostHooks.input = [&](auto port, const auto&) {
            if (!port) return;
            for (const auto& state : port->Read()) if (state.ready && (state.requested & screenshare::input::Gamepad) && !grantWritten) {
                writeCommand(host.inputCommandsFile, 2, "grant", state.peer); grantWritten = true;
            }
            if (controllerSink->applied && !revokeWritten) {
                writeCommand(host.inputCommandsFile, 3, "revoke", ""); revokeWritten = true;
            }
            for(const auto& state:port->Read())if(state.requested==screenshare::input::Mouse && !mouseGrantWritten) {
                writeCommand(host.inputCommandsFile,4,"grant",state.peer,"mouse");mouseGrantWritten=true;
            }
        };
        auto hosting = std::async(std::launch::async, [&] { return RunRoomCliSession(host, Factory(host, hostAudio), hostHooks, true); });
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        std::string joinedRoom;
        while (joinedRoom.empty()) {
            Check(std::chrono::steady_clock::now() < deadline);
            { std::lock_guard lock(mutex); joinedRoom = roomId; }
            std::this_thread::sleep_for(5ms);
        }
        object["host"] = false; object["roomId"] = "screenshare://room/v2/" + QString::fromStdString(joinedRoom); object["nickname"] = "CliViewer";
        object["seconds"] = 6; object.remove("changes"); object.remove("captureChanges"); object.remove("audioChanges");
        object["playbackChanges"] = QJsonArray{QJsonObject{{"atMs", 1000}}, QJsonObject{{"atMs", 2000}, {"muted", true}}, QJsonObject{{"atMs", 4000}, {"deviceId", "replacement"}, {"volume", 50}}};
        auto invalidPlayback = object; invalidPlayback["playbackChanges"] = QJsonArray{QJsonObject{{"atMs", 100}, {"volume", 101}}};
        Reject([&] { ParseRoomSessionConfig(invalidPlayback, true); });
        invalidPlayback = object; invalidPlayback["host"] = true;
        Reject([&] { ParseRoomSessionConfig(invalidPlayback, true); });
        invalidPlayback = object; invalidPlayback["playbackChanges"] = QJsonArray{QJsonObject{{"atMs", 100}}, QJsonObject{{"atMs", 100}}};
        Reject([&] { ParseRoomSessionConfig(invalidPlayback, true); });
        auto viewer = ParseRoomCommand({"--backend", "v2", "--signal-server", argv[1], "--join-room",
            "screenshare://room/v2/" + QString::fromStdString(joinedRoom), "--nickname", "CliViewer", "--password-file", passwordPath,
            "--seconds", "6", "--no-preview"}, nullptr, true);
        viewer.playbackChanges = ParseRoomSessionConfig(object, true).playbackChanges;
        viewer.media.presentation = std::make_shared<PresentationTelemetry>();
        auto frames = std::make_shared<LatestRoomVideoFrame>();
        auto audio = std::make_shared<proof::AudioEvidence>();
        audio->outputUnavailable = true;
        unsigned original = 0, changed = 0;
        bool silentVideo = false;
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
        screenshare::ReceiverPreviewWindow preview;
        preview.SetLowLatency(true);
        preview.Show();
#endif
        RoomCliHooks viewerHooks;
        viewer.inputCommandsFile = viewerCommands;
        bool requestWritten = false,mouseRequestWritten=false;
        screenshare::input::FrameMapping displayedMapping;
        viewerHooks.input = [&](auto port, const auto&) {
            if(!port)return;
            if(requestWritten && !mouseRequestWritten && controllerSink->released && displayedMapping.Valid())
                for(const auto& state:port->Read())if(state.ready && !state.granted && !state.revokePending) {
                    writeCommand(viewerCommands,3,"request",state.peer,"mouse");mouseRequestWritten=true;break;
                }
            if (requestWritten) return;
            for (const auto& state : port->Read()) if (state.ready && state.permission) {
                writeCommand(viewerCommands, 2, "request", state.peer); requestWritten = true; break;
            }
        };
        viewerHooks.gamepad = []() -> std::optional<screenshare::input::Event> {
            screenshare::input::Event event; event.kind = screenshare::input::Kind::Pad; event.buttons = 1; return event;
        };
        viewerHooks.inputCapture=[&](uint8_t caps,auto submit) {
            if((caps&screenshare::input::Mouse) && displayedMapping.Valid() && !desktopEvidence->applied) {
                screenshare::input::Event event;event.kind=screenshare::input::Kind::Pointer;event.x=event.y=.5f;event.sourceGeneration=displayedMapping.generation;submit(event);
            }
        };
        int playbackChanges = 0;
        bool playbackFailed = false, playbackRecovered = false;
        viewerHooks.report = [&](const QJsonObject& value) {
            const auto health = value["playbackHealth"].toObject();
            if (health["state"] == "failed") {
                Check(health["failures"].toInteger() == 1); playbackFailed = true; audio->outputUnavailable = false;
            }
            if (playbackFailed && health["state"] == "running") playbackRecovered = true;
            if (value["type"] == "playback") { Check(value["error"].toInt() == 0); ++playbackChanges; }
        };
        viewerHooks.pump = [&] {
            if (!audioChanged && original >= 10 && audio->quietStreak >= 10) {
                Check(audio->audibleBlocks == 0); silentVideo = true;
            }
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
            Check(preview.PumpMessages());
#endif
            if (auto frame = frames->Take()) {
                displayedMapping=frame->inputMapping;
                Check(frame->pixels().size() == frame->width * frame->height * 3 / 2 && frame->nv12.empty());
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
                Check(bool(frame->native));
#else
                Check(bool(frame->retainedPixels));
#endif
                Check(frame->pixels()[frame->width * frame->height / 2 + frame->width / 2] >= 35);
                if (frame->width == 320) ++original;
                else if (frame->width == 160) ++changed;
                else Check(false);
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
                preview.PresentFrame(*frame);
                const auto stats = preview.presentationStats();
                viewer.media.presentation->Publish({preview.framesPresented(), preview.framesDropped(), 0, uint8_t(stats.outcome)});
#endif
            }
            return true;
        };
        const int viewing = RunRoomCliSession(viewer, Factory(viewer, audio, frames), viewerHooks, true);
        stopHost = true;
        Check(playbackChanges == 3 && playbackFailed && playbackRecovered);
        const auto hostResult=hosting.get();
        { QFile saved(host.reportFile); Check(saved.open(QIODevice::ReadOnly)); const auto bytes = saved.readAll();
          Check(!bytes.contains("test-only-password") && !bytes.contains("CliHost"));
          Check(QJsonDocument::fromJson(bytes).object()["backend"] == "v2"); }
        if(!(hostResult == 0 && viewing == 0 && stopped && accepted && applied && budgetReported && rateReported && receiverReported && senderReported && sourceChanged && audioChanged))
            throw std::runtime_error("CLI completion host="+std::to_string(hostResult)+" viewer="+std::to_string(viewing)+" flags="+
                std::to_string(stopped)+std::to_string(accepted)+std::to_string(applied)+std::to_string(budgetReported)+std::to_string(rateReported)+std::to_string(receiverReported)+std::to_string(senderReported)+std::to_string(sourceChanged)+std::to_string(audioChanged));
        Check(extendedReported && pipelineReported);
        Check(microphoneReported && bypassRestored);
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
        Check(presentationReported);
#endif
        Check(silentVideo && original >= 10 && changed >= 10 && audio->audibleBlocks >= 20);
        Check(frames->statistics().retained >= original + changed && frames->statistics().converted == 0 && frames->statistics().repacked == 0);
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
        Check(preview.framesPresented() >= 20);
        Check(preview.maximumFrameLatency() == 1);
#endif
        // A stalled presentation consumer retains only the newest frame.
        auto pixels = webrtc::I420Buffer::Create(4, 2);
        for (int i = 0; i < 100; ++i) frames->OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(pixels).set_timestamp_us(i + 1).build());
        auto latest = frames->Take(); Check(latest && latest->timestamp100ns == 1000 && !frames->Take());
        Check(frames->statistics().replaced >= 99);
        bool cancelledAdmission = false, cancelledStopped = false;
        RoomCliHooks cancelHooks;
        cancelHooks.pump = [] { return false; };
        cancelHooks.report = [&](const QJsonObject& value) {
            if (value["type"] == "admission-ended") {
                cancelledAdmission = value["error"].toInt() == int(RoomError::Cancelled) && value.contains("outcomeUnconfirmed");
            }
            if (value["phase"] == "stopped") cancelledStopped = true;
        };
        Check(grantWritten && revokeWritten && requestWritten && controllerSink->applied > 0 && controllerSink->released > 0);
        Check(mouseGrantWritten && mouseRequestWritten && desktopEvidence->applied && desktopEvidence->released);
        Check(RunRoomCliSession(host, Factory(host, hostAudio), cancelHooks, true) == 0);
        Check(cancelledAdmission && cancelledStopped);
        auto missing = viewer; missing.room.roomId = "nonexistent-room";
        bool admissionError = false;
        RoomCliHooks missingHooks;
        missingHooks.report = [&](const QJsonObject& value) { if (value["type"] == "admission-error") admissionError = true; };
        Check(RunRoomCliSession(missing, Factory(missing, audio), missingHooks, true) == 1 && admissionError);
        std::cout << "{\"passed\":true,\"cli_session\":true,\"command_options\":true,\"live_settings\":true,\"bounded_presentation\":true,\"silent_audio\":true,\"original_frames\":" << original << ",\"changed_frames\":" << changed << "}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; exitCode = 1; }
    webrtc::CleanupSSL(); return exitCode;
}
