#include "ui/RoomSessionWindow.h"
#include "RecordingGamepadSink.h"
#include "RecordingDesktopInput.h"
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include "shared/RoomLink.h"
#include "shared/StreamPreferencesJson.h"
#include "shared/RoomStreamDiagnostics.h"
#include <QClipboard>
#include "ui/RoomBrowserWindow.h"
#include "ui/RoomApplication.h"
#include "shared/RoomLaunch.h"
#include <QNetworkAccessManager>
#include <QStackedWidget>
#include "ui/VideoFrameWidget.h"
#include "media/webrtc/NativeRoomRuntime.h"
#include "media/webrtc/MfVideoEncoderFactory.h"
#include "media/webrtc/MfVideoDecoderFactory.h"
#include "media/webrtc/PcmAudioDeviceModule.h"
#include "../tools/webrtc-proof/SyntheticAudio.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/win32_socket_init.h"
#include "rtc_base/logging.h"
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QLineEdit>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <iostream>
#include <deque>
#include <source_location>
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
#include "../tools/webrtc-proof/CaptureTestWindow.h"
#include "core/WindowsMediaRuntime.h"
#include "render/Nv12D3D11Presenter.h"
#include "shared/LatestRoomVideoFrame.h"
#include <dxgi.h>
HWND captureWindow = nullptr;
#endif
using namespace screenshare::media;
using namespace screenshare::v2;
using namespace std::chrono_literals;
void Check(bool value, std::source_location where = std::source_location::current()) {
    if (!value) throw std::runtime_error("Room UI proof failed at " + std::to_string(where.line()));
}
template<class F> void Wait(F condition, std::source_location where = std::source_location::current()) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!condition()) {
        Check(std::chrono::steady_clock::now() < deadline, where);
        QCoreApplication::processEvents(); std::this_thread::sleep_for(1ms);
    }
}
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
// Inject typed loss after actual GPU work, then let the production worker tear
// down/recreate real resources. This is not a physical driver-removal test.
class FaultingNativeRenderer final : public FramePresentationBackend {
    std::unique_ptr<FramePresentationBackend> native_ = CreateNativeFramePresentation();
    std::shared_ptr<std::atomic_bool> fail_;
public:
    explicit FaultingNativeRenderer(std::shared_ptr<std::atomic_bool> fail) : fail_(std::move(fail)) {}
    bool Present(HWND window, uint32_t width, uint32_t height, bool smooth, bool lowLatency,
        const screenshare::Nv12D3D11Presenter::FrameView& frame, screenshare::Nv12D3D11Presenter::ScaleMode scale) override {
        const bool result = native_->Present(window, width, height, smooth, lowLatency, frame, scale);
        if (fail_->exchange(false)) throw screenshare::PresentationError(DXGI_ERROR_DEVICE_REMOVED, "Injected native loss");
        return result;
    }
    void Update(HWND window, uint32_t width, uint32_t height, bool smooth, bool lowLatency, screenshare::Nv12D3D11Presenter::ScaleMode scale) override {
        native_->Update(window, width, height, smooth, lowLatency, scale);
    }
    void Reset() noexcept override { native_->Reset(); }
    uint32_t MaximumFrameLatency() const noexcept override { return native_->MaximumFrameLatency(); }
    screenshare::PresentationOutcome LastOutcome() const noexcept override { return native_->LastOutcome(); }
};
void NativePresentationRecovery() {
    auto fail = std::make_shared<std::atomic_bool>(false);
    VideoFrameWidget widget(nullptr, [fail] { return std::make_unique<FaultingNativeRenderer>(fail); });
    widget.resize(320, 180); widget.setLowLatency(true); widget.show();
    // The fixture hides helper consoles via STARTUPINFO. Windows can apply that
    // state to the first GUI ShowWindow as well, despite Qt reporting visible.
    // Explicitly show only this test-owned window, without taking activation.
    ShowWindow(reinterpret_cast<HWND>(widget.winId()), SW_SHOWNOACTIVATE);
    Check(IsWindowVisible(reinterpret_cast<HWND>(widget.winId())));
    auto gpu = std::make_shared<D3dVideoDevice>();
    const std::vector<uint8_t> pixels(320 * 180 * 3 / 2, 128);
    auto texture = gpu->UploadNv12(320, 180, pixels);
    auto native = std::make_shared<RetainedRoomGpuFrame>(texture);
    bool useGpu = true;
    auto send = [&] {
        screenshare::Nv12VideoFrame frame; frame.width = 320; frame.height = 180;
        if (useGpu) frame.native = native;
        else frame.nv12 = pixels;
        Check(widget.setVideoFrame(std::move(frame)));
    };
    try { Wait([&] { send(); return widget.presentedFrameCount() >= 3; }); }
    catch (...) {
        const auto stats = widget.presentationStats();
        const auto hwnd = reinterpret_cast<HWND>(widget.winId());
        std::cerr << "native-presentation-start: qtVisible=" << widget.isVisible()
            << " winVisible=" << IsWindowVisible(hwnd) << " minimized=" << IsIconic(hwnd)
            << " presented=" << stats.presentedFrames << " enqueued=" << stats.enqueuedFrames
            << " outcome=" << int(stats.renderer.outcome) << " occluded=" << stats.renderer.occludedDrops
            << " errors=" << stats.presentErrors << " code=" << stats.renderer.lastError << '\n';
        throw;
    }
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
        *fail = true;
        Wait([&] { send(); return widget.presentationStats().recoveries == attempt; });
        const auto presented = widget.presentedFrameCount();
        Wait([&] { send(); return widget.presentedFrameCount() >= presented + 3; });
        Check(!widget.presentationStats().terminal && widget.presentationStats().maximumFrameLatency == 1);
    }
    *fail = true;
    Wait([&] { send(); return widget.presentationStats().terminal; });
    Check(widget.presentationStats().recoveries == 3);
    widget.clearFrame(); Wait([&] { return !widget.presentationStats().terminal; });
    const auto presented = widget.presentedFrameCount();
    Wait([&] { send(); return widget.presentedFrameCount() >= presented + 3; });
    Check(widget.presentationStats().recoveries == 0);
    // The presenter sees a native child surface, while only its parent shell is
    // minimized. Drops must avoid GPU work/recovery and restore on a fresh frame.
    const auto errors = widget.presentationStats().presentErrors;
    widget.showMinimized();
    Wait([&] { send(); return widget.presentationStats().renderer.minimizedDrops >= 3; });
    const auto minimizedFrames = widget.presentedFrameCount();
    for (int i = 0; i < 10; ++i) { send(); QCoreApplication::processEvents(); }
    Check(widget.presentedFrameCount() == minimizedFrames);
    widget.showNormal();
    Wait([&] { send(); return widget.presentedFrameCount() >= minimizedFrames + 3; });
    widget.hide();
    Wait([&] { send(); return widget.presentationStats().renderer.unavailableDrops >= 3; });
    widget.show();
    const auto hiddenFrames = widget.presentedFrameCount();
    Wait([&] { send(); return widget.presentedFrameCount() >= hiddenFrames + 3; });
    Check(widget.presentationStats().presentErrors == errors && widget.presentationStats().recoveries == 0);
    // Switch between GPU output and software fallback on the same widget/device.
    for (bool mode : {false, true, false, true}) {
        useGpu = mode;
        const auto previous = widget.presentedFrameCount();
        Wait([&] { send(); return widget.presentedFrameCount() >= previous + 3; });
    }
    Check(gpu->readbackCount() == 0 && widget.presentationStats().maximumFrameLatency == 1);
}
#endif
QtRoomSession::Factory Factory(std::shared_ptr<proof::AudioEvidence> audio) {
    return [audio](WindowsRoomRuntimeOptions windows) -> RoomRuntimeFactory {
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
        windows.capture.sourceType = screenshare::CaptureSourceType::Window;
        windows.capture.windowHandle = reinterpret_cast<uint64_t>(captureWindow);
        windows.audioEndpoints = proof::SyntheticAudio(audio);
        windows.audioForSelection = [audio](auto selection) { return proof::SyntheticAudioSelectionWithEvidence(selection, audio); };
        windows.playbackForSelection = [audio](auto selection) { return proof::SyntheticPlayback(selection, audio); };
        return WindowsRoomRuntimeFactory(std::move(windows));
#else
        return [windows, audio](auto identity, auto send) {
            NativeRoomRuntimeOptions options; options.preferences = windows.preferences; options.frames = windows.frames;
            options.presentation = windows.presentation;
            options.inputSink = windows.inputSink;
            auto endpoints = proof::SyntheticAudio(audio);
            if (!identity.host) {
                options.playback = std::make_shared<PlaybackControl>(PlaybackSelection{windows.playbackDeviceId, windows.playbackVolume, windows.playbackMuted}, endpoints.playout);
                endpoints.playout = [control = options.playback] { return std::make_unique<ControlledPcmPlayout>(control); };
                options.playbackForSelection = [audio](auto selection) { return proof::SyntheticPlayback(selection, audio); };
            }
            if (identity.host) {
                options.audioSwitch = std::make_shared<AudioSwitchControl>(AudioSelection{}, endpoints.capture, ProcessMicrophone);
                endpoints.capture = [control = options.audioSwitch] { return std::make_unique<SwitchablePcmCapture>(control); };
                options.audioForSelection = [audio](auto selection) { return proof::SyntheticAudioSelectionWithEvidence(selection, audio); };
            }
            options.engine = [endpoints] {
                return std::make_unique<MediaEngine>(CreatePcmAudioDeviceModule(endpoints,
                    std::make_shared<PcmAudioDiagnostics>()), std::make_unique<MfVideoEncoderFactory>(), std::make_unique<MfVideoDecoderFactory>());
            };
            options.capture = [] { return std::make_unique<SyntheticCaptureSource>(320, 180, 30); };
            if(windows.inputTarget)options.capture=[target=windows.inputTarget] {return std::make_unique<MappedSyntheticCapture>(320,180,30,target);};
            options.captureForSelection = [](CaptureSelection selection) -> CaptureSession::Factory {
                return [selection]() -> std::unique_ptr<ICaptureSource> {
                    if (selection.display == 63 || selection.window == 1) throw std::runtime_error("Injected capture startup failure");
                    return std::make_unique<SyntheticCaptureSource>(640, 360, selection.fps);
                };
            };
            if(windows.inputTarget)options.captureForSelection=[target=windows.inputTarget](auto selection)->CaptureSession::Factory {
                return [target,selection] {return std::make_unique<MappedSyntheticCapture>(640,360,selection.fps,target);};
            };
            options.deliver = [](auto& source, const auto& sample) { source.Push(*std::static_pointer_cast<SyntheticCaptureResource>(sample.resource), sample.capturedAt,sample.resource->inputGeneration); };
            return CreateNativeRoomRuntime(identity, std::move(send), std::move(options));
        };
#endif
    };
}
class HeldRuntime final : public RoomRuntime {
    std::shared_future<void> barrier_;
public:
    explicit HeldRuntime(std::shared_future<void> barrier) : barrier_(barrier) {}
    void Advance() override {}
    bool Ready(const std::string&) override { return true; }
    bool Add(const std::string&) override { return true; }
    void Remove(const std::string&) noexcept override {}
    bool Receive(const std::string&, RoomPeerSignal) override { return true; }
    std::shared_future<void> BeginStop() override { return barrier_; }
};
struct MutationBarrier { std::atomic<bool> pause{false}, entered{false}; std::promise<void> release; std::shared_future<void> ready = release.get_future().share(); };
class PausedRuntime final : public RoomRuntime {
    std::shared_ptr<MutationBarrier> barrier_;
public:
    explicit PausedRuntime(std::shared_ptr<MutationBarrier> barrier) : barrier_(std::move(barrier)) {}
    void Advance() override { if (barrier_->pause) { barrier_->entered = true; barrier_->ready.wait(); } }
    bool Ready(const std::string&) override { return true; }
    bool Add(const std::string&) override { return true; }
    void Remove(const std::string&) noexcept override {}
    bool Receive(const std::string&, RoomPeerSignal) override { return true; }
    std::shared_future<void> BeginStop() override { std::promise<void> done; done.set_value(); return done.get_future().share(); }
};
void MutationLifecycle(const std::string& origin) {
    auto barrier = std::make_shared<MutationBarrier>();
    RoomSession session([barrier](auto, auto) { return std::make_unique<PausedRuntime>(barrier); }, true);
    RoomOptions options; options.origin = origin; options.host = true; options.nickname = "Lifecycle"; options.name = "Mutation lifecycle";
    auto start = session.Start(options); Wait([&] { return start.wait_for(0ms) == std::future_status::ready; }); Check(start.get().error == RoomError::None);
    const auto revision = session.Status().revision;
    Check(session.UpdateNickname("Invalid revision", UINT64_MAX).get().error == RoomUpdateError::Invalid);
    Check(session.UpdateNickname(std::string(1, char(0xff)), revision).get().error == RoomUpdateError::Invalid);
    barrier->pause = true;
    try {
        Wait([&] { return barrier->entered.load(); });
        auto pending = session.UpdateNickname("Pending", revision);
        auto pendingSource = session.SwitchCaptureSource({});
        auto pendingAudio = session.SwitchAudioSource({});
        auto pendingPlayback = session.UpdatePlayback({});
        Check(session.UpdatePlayback({}).get().error == AudioUpdateError::Busy);
        Check(session.SwitchAudioSource({}).get().error == AudioUpdateError::Busy);
        Check(session.SwitchCaptureSource({}).get().error == CaptureUpdateError::Busy);
        Check(session.UpdateNickname("Overflow", revision).get().error == RoomUpdateError::Busy);
        auto stopping = session.Stop();
        Check(session.UpdateNickname("Stopped", revision).get().error == RoomUpdateError::Unavailable);
        barrier->release.set_value();
        Wait([&] { return stopping.wait_for(0ms) == std::future_status::ready; }); stopping.get();
        Check(pending.get().error == RoomUpdateError::Unconfirmed);
        Check(pendingSource.get().error == CaptureUpdateError::Cancelled);
        Check(pendingAudio.get().error == AudioUpdateError::Cancelled);
        Check(pendingPlayback.get().error == AudioUpdateError::Cancelled);
        Check(session.UpdatePlayback({}).get().error == AudioUpdateError::Unavailable);
        Check(session.SwitchAudioSource({}).get().error == AudioUpdateError::Unavailable);
        Check(session.SwitchCaptureSource({}).get().error == CaptureUpdateError::Unavailable);
    } catch (...) { if (barrier->ready.wait_for(0ms) != std::future_status::ready) barrier->release.set_value(); throw; }
}
void ProfileSettingsScenario() {
    PeerStreamStatus diagnostic;
    Check(StreamPeerState(diagnostic, 2) == "pending");
    Check(StreamPeerJson(diagnostic, 2)["transportSendBps"].isNull());
    Check(StreamPeerJson(diagnostic, 2)["source"].toObject()["activeImage"].isNull());
    diagnostic.appliedRevision = 2;
    Check(StreamPeerState(diagnostic, 2) == "upload-paused");
    diagnostic.appliedVideoBitrateBps = 1000000;
    Check(StreamPeerState(diagnostic, 2) == "waiting-for-source");
    diagnostic.observedRevision = 2;
    Check(StreamPeerState(diagnostic, 2) == "source-observed");
    diagnostic.transportSendBps = 0;
    Check(StreamSampleState(diagnostic) == "fresh" && StreamPeerJson(diagnostic, 2)["transportSendBps"].isDouble());
    diagnostic.transportSampleStale = true;
    Check(StreamSampleState(diagnostic) == "stale" && StreamPeerJson(diagnostic, 2)["transportSendBps"].isNull());
    diagnostic.rejected = true;
    Check(StreamPeerState(diagnostic, 3) == "rejected");
    diagnostic.receiver.observation = ReceiverVideoObservation{320, 180, 0, 0};
    diagnostic.receiver.observation->presentation = ReceiverPresentationObservation{12, 3, 1, 3};
    diagnostic.receiver.observation->decoderDrops = 4;
    diagnostic.receiver.observation->jitterBufferMeanMs = 5;
    diagnostic.receiver.observation->jitterBufferRecentMs = 0;
    Check(StreamPeerJson(diagnostic, 3)["receiver"].toObject()["jitterBufferRecentMs"].toInt(-1) == 0);
    diagnostic.receiver.observation->decoder = CodecImplementation::MfH264Software;
    Check(StreamPeerJson(diagnostic, 3)["receiver"].toObject()["decodeFps"].toDouble(-1) == 0);
    Check(StreamPeerJson(diagnostic, 3)["receiver"].toObject()["presentation"].toObject()["dropped"].toInteger() == 3);
    diagnostic.receiver.stale = true;
    const auto expiredReceiver = StreamPeerJson(diagnostic, 3)["receiver"].toObject();
    Check(expiredReceiver["sampleState"] == "stale" && expiredReceiver["width"].isNull() && expiredReceiver["framesDecoded"].isNull());
    Check(expiredReceiver["presentation"].isNull() && expiredReceiver["decoderDrops"].isNull() &&
        expiredReceiver["jitterBufferMeanMs"].isNull() && expiredReceiver["jitterBufferRecentMs"].isNull() && expiredReceiver["decoder"] == "unknown");
    StreamStatus partial; partial.requestedRevision = 2;
    diagnostic.settingsError = SettingsApplyError::SenderRejected;
    diagnostic.appliedPreferences = StreamPreferences{};
    partial.peers.push_back(diagnostic); // Rejection takes precedence even with matching prior revisions.
    auto success = diagnostic; success.rejected = false; success.settingsError = SettingsApplyError::None;
    partial.peers.push_back(success); partial.peers.push_back(PeerStreamStatus{});
    const auto application = StreamApplicationJson(partial);
    Check(application["state"] == "partial" && application["applied"] == 1 && application["rejected"] == 1 && application["pending"] == 1);
    Check(StreamPeerJson(diagnostic, 2)["settingsError"] == "sender-rejected" &&
        StreamPeerJson(diagnostic, 2)["appliedPreferences"].toObject()["width"] == 1920);
    partial.capture = {HostMediaState::Recovering, CaptureFailure::Source, 9};
    partial.codec = {true, true, false, 12, 1};
    const auto pipeline = PipelineDiagnosticsJson(partial);
    Check(pipeline["captureState"] == "recovering" && pipeline["captureFailure"] == "source" &&
        pipeline["hardwarePipeline"].toObject()["fallbackState"] == "hardware-quarantined");
    Check(PipelineDiagnosticsJson(StreamStatus{})["hardwarePipeline"].isNull());
    partial.capture.state = HostMediaState::Minimized;
    partial.capture.source = {CaptureImplementation::DesktopDuplication, true};
    Check(PipelineDiagnosticsJson(partial)["captureState"] == "minimized" &&
        PipelineDiagnosticsJson(partial)["captureBackend"] == "dxgi" && PipelineDiagnosticsJson(partial)["captureFallback"] == true);
    partial.capture.state = HostMediaState::SourceClosed;
    Check(PipelineDiagnosticsJson(partial)["captureState"] == "source-closed");
    QTemporaryDir files; Check(files.isValid()); const auto path = files.filePath("profile.ini");
    RoomProfile profile(path);
    const auto guest = profile.nickname(); Check(guest.startsWith("Guest-") && guest.size() == 14);
    Check(RoomProfile(path).nickname() == guest);
    for (int preset = 0; preset < 2; ++preset) for (int resolution = 0; resolution < 3; ++resolution)
    for (int fps = 0; fps < 2; ++fps) for (int bitrate = 0; bitrate < 2; ++bitrate) {
        StreamPreferences settings; settings.preset = StreamPreset(preset); settings.resolution = ResolutionMode(resolution);
        settings.width = 1280; settings.height = 720; settings.fps = 144; settings.fpsMode = SettingMode(fps);
        settings.bitrateMode = SettingMode(bitrate); settings.bitrateLimitBps = 9000000; settings.aggregateUploadLimitBps = 30000000;
        Check(profile.saveStreamPreferences(settings));
        const auto saved = RoomProfile(path).streamPreferences();
        Check(StreamPreferencesJson(saved) == StreamPreferencesJson(settings));
        const auto config = ParseRoomSessionConfig(QJsonObject{{"origin", "https://example.invalid"}, {"host", true}, {"stream", StreamPreferencesJson(saved)}});
        Check(StreamPreferencesJson(config.media.preferences) == StreamPreferencesJson(saved));
    }
    StreamPreferences automatic; Check(profile.saveStreamPreferences(automatic));
    Check(!RoomProfile(path).streamPreferences().bitrateLimitBps);
    auto invalid = automatic; invalid.width = 3; Check(!profile.saveStreamPreferences(invalid));
    Check(profile.streamPreferences().width == 1920);
    Check(profile.savePlayback({23, true})); Check(!profile.savePlayback({101, false}));
    Check(RoomProfile(path).playback().volume == 23 && RoomProfile(path).playback().muted);
    for (const auto& bytes : {QByteArray("[]"), QByteArray("{\"width\":3}"), QByteArray("{\"bitrateMode\":\"manual\"}"),
            QByteArray("{\"width\":\"1280\"}"), QByteArray("{\"password\":\"never-accepted\"}"), QByteArray(5000, 'x')}) {
        { QSettings raw(path, QSettings::IniFormat); raw.setValue("stream/v1", bytes); raw.sync(); }
        Check(RoomProfile(path).streamPreferences().width == 1920);
        Check(RoomProfile(path).playback().volume == 23); // Corrupt stream does not reset playback or nickname.
        Check(RoomProfile(path).nickname() == guest);
    }
    { QSettings raw(path, QSettings::IniFormat); raw.setValue("playback/v1", QByteArray("{\"volume\":50.5,\"muted\":true}")); raw.sync(); }
    Check(RoomProfile(path).playback().volume == 100 && !RoomProfile(path).playback().muted);
    Check(profile.saveDecoder("software") && RoomProfile(path).decoder() == "software");
    { QSettings raw(path, QSettings::IniFormat); raw.setValue("decoder/v1", "unsupported"); raw.sync(); }
    Check(RoomProfile(path).decoder() == "auto");
    // A directory cannot be a settings file. Failed writes must not change the
    // in-memory defaults later consumed by browser sessions.
    RoomProfile unwritable(files.path());
    Check(!unwritable.savePlayback({12, true})); Check(unwritable.playback().volume == 100);
    Check(!unwritable.saveDecoder("software") && unwritable.decoder() == "auto");
    auto valid = automatic; valid.width = 1280;
    Check(!unwritable.saveStreamPreferences(valid)); Check(unwritable.streamPreferences().width == 1920);
}
void NormalHomeScenario(const QUrl& origin) {
    using Directory = screenshare::room::qt::RoomDirectory;
    QTemporaryDir profiles; Check(profiles.isValid());
    auto audio = std::make_shared<proof::AudioEvidence>();
    const auto hostFile = profiles.filePath("normal-host.ini");
    RoomProfile profile(hostFile);
    StreamPreferences prefs; prefs.resolution = ResolutionMode::Fixed; prefs.width = 320; prefs.height = 180; prefs.fps = 30;
    Check(profile.saveStreamPreferences(prefs));
    StreamPreferences createdPreferences;
    RoomApplication host(origin, [factory = Factory(audio), &createdPreferences](WindowsRoomRuntimeOptions options) {
        createdPreferences = options.preferences; return factory(std::move(options));
    }, true, hostFile, false, true);
    RoomApplication viewer(origin, Factory(audio), true, profiles.filePath("normal-viewer.ini"), false, true);
    host.show(); viewer.show();
    auto* stack = host.window().findChild<QStackedWidget*>("AppPageStack");
    Check(host.home() && viewer.home() && stack->currentWidget() == host.home());
    Check(!host.home()->findChild<QNetworkAccessManager*>()); // No legacy /rooms client.
    Wait([&] { return host.browser()->directory().status().phase == Directory::Phase::Ready &&
        viewer.browser()->directory().status().phase == Directory::Phase::Ready; });
    const auto attempts = host.browser()->directory().connectionAttempts();
    const auto previews = qEnvironmentVariable("SCREENSHARE_UI_PREVIEWS");
    auto snapshot = [&](const QString& name) {
        if (previews.isEmpty()) return;
        QCoreApplication::processEvents();
        Check(QDir().mkpath(previews));
        const auto ratio = host.window().devicePixelRatioF();
        QPixmap rendered(host.window().size() * ratio); rendered.setDevicePixelRatio(ratio); rendered.fill(Qt::transparent);
        host.window().render(&rendered);
        Check(rendered.save(QDir(previews).filePath(name + ".png")));
    };
    host.window().findChild<QPushButton*>("TitleProfile")->click();
    auto* preferences = host.window().findChild<QDialog*>("ProfilePreferences"); Check(preferences);
    const auto originalName = host.browser()->profileName();
    preferences->findChild<QLineEdit*>("profileNickname")->setText(" ");
    preferences->findChild<QPushButton*>("saveProfile")->click();
    Check(preferences->isVisible() && !preferences->findChild<QLabel*>("profileError")->text().isEmpty());
    Check(host.browser()->profileName() == originalName);
    preferences->findChild<QLineEdit*>("profileNickname")->setText("Not saved");
    if (!previews.isEmpty()) { Check(QDir().mkpath(previews)); Check(preferences->grab().save(QDir(previews).filePath("profile.png"))); }
    preferences->reject(); Check(host.browser()->profileName() == originalName);
    for (const auto size : {QSize(800,600), QSize(1200,850)}) {
        host.window().resize(size); QCoreApplication::processEvents();
        snapshot(QString("home-%1").arg(size.width()));
        host.home()->findChild<QPushButton*>("HomePrimary")->click();
        Check(!host.browser()->findChild<QLineEdit*>("roomNickname"));
        Check(host.browser()->findChild<QPushButton*>("createV2Room")->isVisible());
        Check(!host.browser()->findChild<QPushButton*>("joinV2Room")->isVisible());
        host.browser()->findChild<QLineEdit*>("roomName")->setText("Friday games");
        host.window().resize(size + QSize(20,20)); QCoreApplication::processEvents(); host.window().resize(size);
        Check(host.browser()->findChild<QLineEdit*>("roomName")->text() == "Friday games");
        snapshot(QString("create-%1").arg(size.width()));
        host.browser()->findChild<QPushButton*>("roomBack")->click();
        host.home()->findChild<QPushButton*>("HomeSecondary")->click();
        Check(host.browser()->findChild<QPushButton*>("joinV2Room")->isVisible());
        Check(!host.browser()->findChild<QPushButton*>("createV2Room")->isVisible());
        snapshot(QString("join-%1").arg(size.width()));
        host.browser()->findChild<QPushButton*>("roomBack")->click();
    }
    for (int i = 0; i < 3; ++i) {
        host.home()->findChild<QPushButton*>("HomePrimary")->click();
        Check(stack->currentWidget() == host.browser() && !host.keepingScreenAwake());
        host.browser()->findChild<QLineEdit*>("roomPassword")->setText("discard-on-back");
        host.browser()->findChild<QPushButton*>("roomBack")->click();
        Check(stack->currentWidget() == host.home() && host.browser()->findChild<QLineEdit*>("roomPassword")->text().isEmpty());
        host.home()->refreshRooms();
    }
    Check(host.browser()->directory().connectionAttempts() == attempts && stack->count() == 2);
    viewer.home()->findChild<QPushButton*>("HomeSecondary")->click();
    Check(viewer.browser()->findChild<QLineEdit*>("joinRoomId")->text().isEmpty());
    viewer.browser()->findChild<QPushButton*>("roomBack")->click();
    host.home()->findChild<QPushButton*>("HomePrimary")->click();
    host.browser()->findChild<QLineEdit*>("roomName")->setText("<b>Normal home</b>");
    host.browser()->findChild<QComboBox*>("createPreset")->setCurrentIndex(1);
    host.browser()->findChild<QComboBox*>("createBitrate")->setCurrentIndex(1);
    host.browser()->findChild<QLineEdit*>("roomPassword")->setText("normal-home-secret");
    host.browser()->findChild<QPushButton*>("createV2Room")->click();
    auto selectedRow = [&]() -> QWidget* {
        if (!host.session()) return nullptr;
        const auto id = QString::fromStdString(host.session()->session().status().roomId);
        for (auto* row : viewer.home()->findChildren<QWidget*>("HomeRoomRow"))
            if (row->property("roomId").toString() == id) return row;
        return nullptr;
    };
    Wait([&] { return host.session() && host.session()->session().status().phase == RoomPhase::Active &&
        !host.browser()->directory().running() && selectedRow(); });
    Check(host.keepingScreenAwake() && stack->count() == 3);
    Check(createdPreferences.preset == StreamPreset::Quality && createdPreferences.width == 320 &&
        createdPreferences.height == 180 && createdPreferences.fps == 30 &&
        createdPreferences.bitrateMode == SettingMode::Manual && createdPreferences.bitrateLimitBps == 2000000);
    auto* title = selectedRow()->findChild<QLabel*>("HomeInfoPrimary");
    Check(title && title->text() == "<b>Normal home</b>" && title->textFormat() == Qt::PlainText);
    auto* search = viewer.home()->findChild<QLineEdit*>("roomSearch");
    search->setText("no matching title"); Check(selectedRow()->isHidden());
    search->clear(); Check(!selectedRow()->isHidden());
    const auto* stableRow = selectedRow();
    viewer.home()->setPushedRooms({{QString::fromStdString(host.session()->session().status().roomId), "<b>Normal home</b>", 0, true, 0, true}});
    Check(selectedRow() == stableRow);
    selectedRow()->findChild<QPushButton*>("HomeTinyButton")->click();
    Check(!viewer.session() && viewer.browser()->findChild<QLineEdit*>("joinRoomId")->text() ==
        QString::fromStdString(host.session()->session().status().roomId));
    viewer.browser()->findChild<QLineEdit*>("roomPassword")->setText("normal-home-secret");
    viewer.browser()->findChild<QPushButton*>("joinV2Room")->click();
    Wait([&] { return viewer.session() && viewer.session()->session().status().phase == RoomPhase::Active; });
    Wait([&] { return viewer.session()->session().frameStatistics().received >= 3; });
    viewer.session()->close();
    Wait([&] { return !viewer.session() && viewer.home()->isVisible(); });
    Check(!viewer.keepingScreenAwake());
    host.window().close(); viewer.window().close();
    Wait([&] { return host.finished() && viewer.finished(); });
    Check(!host.browser()->directory().running() && !viewer.browser()->directory().running());
}
void BrowserScenario(const QUrl& origin) {
    ProfileSettingsScenario();
    using Directory = screenshare::room::qt::RoomDirectory;
    QTemporaryDir profiles; Check(profiles.isValid());
    const auto hostFile = profiles.filePath("host.ini"), viewerFile = profiles.filePath("viewer.ini");
    {
        QSettings raw(hostFile, QSettings::IniFormat); raw.setValue("nickname", QString("Bad") + QChar(0x202e)); raw.sync();
    }
    RoomProfile profile(hostFile);
    Check(profile.nickname().startsWith("Guest-") && profile.nickname().size() == 14);
    Check(RoomProfile(hostFile).nickname() == profile.nickname());
    Check(!profile.saveNickname(QString("Bad") + QChar(0x202e)));
    Check(!profile.saveNickname(QString(33, 'a')));
    Check(profile.saveNickname(QStringLiteral(" Cafe\u0301 ")) && profile.nickname() == QStringLiteral("Caf\u00e9"));
    Check(RoomProfile(hostFile).nickname() == QStringLiteral("Caf\u00e9"));
    auto audio = std::make_shared<proof::AudioEvidence>();
    RoomApplication hostApp(origin, Factory(audio), true, hostFile, false);
    bool softwareDecoderSelected = false;
    RoomApplication viewerApp(origin, [factory = Factory(audio), &softwareDecoderSelected](WindowsRoomRuntimeOptions options) {
        softwareDecoderSelected = !options.preferHardwareDecoding; return factory(std::move(options));
    }, true, viewerFile, false);
    auto& host = *hostApp.browser(); auto& viewer = *viewerApp.browser();
    Directory audit(true); Check(audit.Start(origin)); hostApp.show(); viewerApp.show();
    auto* hostStack = hostApp.window().findChild<QStackedWidget*>("AppPageStack");
    auto* viewerStack = viewerApp.window().findChild<QStackedWidget*>("AppPageStack");
    Check(hostStack && viewerStack && hostStack->count() == 1 && !host.isWindow());
    Check(!hostApp.keepingScreenAwake() && !viewerApp.keepingScreenAwake());
    Wait([&] { return audit.status().phase == Directory::Phase::Ready && host.directory().status().phase == Directory::Phase::Ready && viewer.directory().status().phase == Directory::Phase::Ready; });
    Check(audit.status().rooms.empty() && host.directory().connectionAttempts() == 1);
    Check(!host.findChild<QLineEdit*>("roomNickname") && host.profileName() == QStringLiteral("Caf\u00e9"));
    host.findChild<QLineEdit*>("roomName")->setText("<b>Plain room</b>");
    host.findChild<QLineEdit*>("roomPassword")->setText("browser-test-secret");
    host.findChild<QPushButton*>("createV2Room")->click();
    Wait([&] { return host.activeSession() && host.activeSession()->session().status().phase == RoomPhase::Active &&
        !host.directory().running() && viewer.directory().status().rooms.size() == 1 && audit.status().rooms.size() == 1; });
    auto* list = viewer.findChild<QTableWidget*>("publicRooms");
    Check(list->rowCount() == 1 && list->item(0, 0)->text() == "<b>Plain room</b>" && list->item(0, 3)->text() == "Required");
    const auto hostAttempts = host.directory().connectionAttempts();
    Check(hostStack->currentWidget() == host.activeSession() && hostStack->count() == 2);
    Check(host.activeSession()->window() == &hostApp.window() && hostApp.window().isVisible());
    Check(hostApp.keepingScreenAwake() && !host.isVisible());
    // Admission status can become Active before the Qt snapshot enables the
    // copy button. Wait for the actual control, not just the earlier room ID.
    Wait([&] { return !host.activeSession()->findChild<QLineEdit*>("roomLink")->text().isEmpty() &&
        host.activeSession()->findChild<QPushButton*>("copyRoomLink")->isEnabled(); });
    const auto roomLink = host.activeSession()->findChild<QLineEdit*>("roomLink")->text();
    Check(roomLink == MakeRoomLink(QString::fromStdString(host.activeSession()->session().status().roomId)));
    Check(!roomLink.contains("browser-test-secret") && !roomLink.contains(origin.toString()));
#ifndef SCREENSHARE_WINDOWS_UI_PROOF
    // Offscreen Qt clipboard only. Never replace the user's real Windows clipboard.
    host.activeSession()->findChild<QPushButton*>("copyRoomLink")->click(); Check(QApplication::clipboard()->text() == roomLink);
#endif
    for (const auto& invalid : {roomLink + "?password=secret", roomLink + "#token", QString("https://other.example/room"), QString("screenshare://user:secret@room/v2/id"), QString("screenshare://room/v3/id"), QString("screenshare://room/v2/%2e%2e")}) {
        Check(!ParseRoomReference(invalid));
        viewer.findChild<QLineEdit*>("joinRoomId")->setText(invalid); viewer.findChild<QPushButton*>("joinV2Room")->click();
        Check(!viewer.activeSession() && !viewer.findChild<QLabel*>("browserError")->text().isEmpty());
    }
    Check(!ParseRoomReference(QString(129, 'x')) && !MakeRoomLink(roomLink).size());
    const auto parsed = ParseRoomSessionConfig(QJsonObject{{"origin", origin.toString()}, {"roomId", roomLink}}, true);
    Check(parsed.room.origin == origin.toString().toStdString() && parsed.room.roomId == host.activeSession()->session().status().roomId);
    viewer.findChild<QLineEdit*>("roomPassword")->setText("wrong-password"); list->selectRow(0);
    viewer.findChild<QPushButton*>("joinSelectedRoom")->click();
    Wait([&] { return viewer.activeSession() && viewer.activeSession()->session().status().phase == RoomPhase::Failed; });
    viewer.activeSession()->close();
    Wait([&] { return !viewer.activeSession() && viewer.isVisible() && viewer.directory().status().phase == Directory::Phase::Ready; });
    Check(viewerStack->count() == 1 && viewerStack->currentWidget() == &viewer && !viewerApp.keepingScreenAwake());
    viewerApp.window().findChild<QPushButton*>("TitleProfile")->click();
    auto* profileDialog = viewerApp.window().findChild<QDialog*>("ProfilePreferences");
    Check(profileDialog);
    profileDialog->findChild<QLineEdit*>("profileNickname")->setText(QString(33, 'x'));
    // The edit enforces the persisted profile's 32-character limit.
    Check(profileDialog->findChild<QLineEdit*>("profileNickname")->text().size() == 32);
    profileDialog->findChild<QLineEdit*>("profileNickname")->setText(" Browser viewer ");
    profileDialog->findChild<QPushButton*>("saveProfile")->click();
    Check(viewer.profileName() == "Browser viewer");
    viewer.findChild<QLineEdit*>("roomPassword")->setText("browser-test-secret"); list->selectRow(0);
    viewer.findChild<QLineEdit*>("joinRoomId")->setText(roomLink);
    viewer.findChild<QComboBox*>("roomDecoder")->setCurrentIndex(1);
    viewer.findChild<QPushButton*>("joinV2Room")->click(); Check(viewer.activeSession());
    Check(softwareDecoderSelected && RoomProfile(viewerFile).decoder() == "software");
    unsigned frames = 0; auto present = viewer.activeSession()->session().frameReady;
    viewer.activeSession()->session().frameReady = [&](auto frame) { ++frames; present(std::move(frame)); };
    Wait([&] { return frames >= 10 && !viewer.directory().running() && audit.status().rooms.size() == 1 && audit.status().rooms[0].viewers == 1; });
    Check(host.directory().connectionAttempts() == hostAttempts); // Hidden browser never reopens.
    Check(viewer.findChild<QLineEdit*>("roomPassword")->text().isEmpty());
    Check(RoomProfile(viewerFile).nickname() == "Browser viewer");
    { QSettings saved(hostFile, QSettings::IniFormat); Check(saved.allKeys() == QStringList{"nickname"}); }
    { QSettings saved(viewerFile, QSettings::IniFormat); Check(saved.allKeys() == QStringList{"decoder/v1", "nickname", "playback/v1"}); }
    auto* hostWindow = host.activeSession(); auto* viewerWindow = viewer.activeSession();
    const auto streamRevision = hostWindow->session().status().stream.requestedRevision;
    hostWindow->findChild<QSpinBox*>("streamWidth")->setValue(1280);
    hostWindow->findChild<QSpinBox*>("streamHeight")->setValue(720);
    hostWindow->findChild<QComboBox*>("streamResolutionMode")->setCurrentIndex(int(ResolutionMode::Fixed));
    hostWindow->findChild<QComboBox*>("streamFpsMode")->setCurrentIndex(int(SettingMode::Manual));
    hostWindow->findChild<QComboBox*>("streamBitrateMode")->setCurrentIndex(int(SettingMode::Manual));
    hostWindow->findChild<QSpinBox*>("streamFps")->setValue(120);
    hostWindow->findChild<QSpinBox*>("streamBitrate")->setValue(9000000);
    for (int preset : {1, 0, 1}) {
        hostWindow->findChild<QComboBox*>("streamPreset")->setCurrentIndex(preset);
        Check(hostWindow->findChild<QSpinBox*>("streamWidth")->value() == 1280 &&
            hostWindow->findChild<QSpinBox*>("streamHeight")->value() == 720 &&
            hostWindow->findChild<QSpinBox*>("streamFps")->value() == 120 &&
            hostWindow->findChild<QSpinBox*>("streamBitrate")->value() == 9000000);
    }
    hostWindow->findChild<QCheckBox*>("uploadBudgetEnabled")->setChecked(true);
    hostWindow->findChild<QSpinBox*>("uploadBudget")->setValue(8000000);
    hostWindow->findChild<QPushButton*>("saveSessionDefaults")->click();
    Check(RoomProfile(hostFile).streamPreferences().width == 1280);
    Check(RoomProfile(hostFile).streamPreferences().aggregateUploadLimitBps == 8000000);
    const auto savedManual = RoomProfile(hostFile).streamPreferences();
    Check(savedManual.preset == StreamPreset::Quality && savedManual.resolution == ResolutionMode::Fixed &&
        savedManual.fpsMode == SettingMode::Manual && savedManual.bitrateMode == SettingMode::Manual &&
        savedManual.fps == 120 && savedManual.bitrateLimitBps == 9000000);
    Check(hostWindow->session().status().stream.requestedRevision == streamRevision); // Save does not apply.
    hostWindow->findChild<QSpinBox*>("streamWidth")->setValue(1279);
    hostWindow->findChild<QPushButton*>("saveSessionDefaults")->click();
    Check(RoomProfile(hostFile).streamPreferences().width == 1280);
    Check(hostWindow->findChild<QLabel*>("profileSaveState")->text().contains("invalid"));
    hostWindow->findChild<QSpinBox*>("streamWidth")->setValue(1280);
    viewerWindow->findChild<QSpinBox*>("playbackVolume")->setValue(37);
    viewerWindow->findChild<QCheckBox*>("playbackMuted")->setChecked(true);
    viewerWindow->findChild<QPushButton*>("saveSessionDefaults")->click();
    Check(RoomProfile(viewerFile).playback().volume == 37 && RoomProfile(viewerFile).playback().muted);
    Wait([&] { return hostWindow->session().status().members.size() == 2 && viewerWindow->findChild<QPushButton*>("updateNickname")->isEnabled(); });
    const auto staleRevision = hostWindow->session().status().revision;
    // Hold a host edit while another authenticated member advances the revision.
    hostWindow->findChild<QLineEdit*>("liveRoomName")->setText("Preserved draft");
    viewerWindow->findChild<QLineEdit*>("liveNickname")->setText(" New viewer ");
    viewerWindow->findChild<QPushButton*>("updateNickname")->click();
    Wait([&] {
        const auto state = hostWindow->session().status();
        return state.revision > staleRevision && state.members.size() == 2 && state.members[1].nickname == "New viewer" && !viewerWindow->session().roomUpdatePending();
    });
    Check(RoomProfile(viewerFile).nickname() == "Browser viewer"); // Session-only customization.
    hostWindow->findChild<QPushButton*>("updateRoomPolicy")->click();
    Wait([&] { return hostWindow->findChild<QLabel*>("roomUpdateState")->text().contains("changed while"); });
    Check(hostWindow->session().status().policy.name == "<b>Plain room</b>");
    Check(hostWindow->findChild<QLineEdit*>("liveRoomName")->text() == "Preserved draft");
    hostWindow->findChild<QPushButton*>("reloadRoomValues")->click();
    Wait([&] { return hostWindow->findChild<QLineEdit*>("liveRoomName")->text() == "<b>Plain room</b>"; });
    hostWindow->findChild<QLineEdit*>("liveRoomName")->setText(" Renamed room ");
    hostWindow->findChild<QSpinBox*>("liveViewerLimit")->setValue(1);
    hostWindow->findChild<QPushButton*>("updateRoomPolicy")->click();
    Wait([&] { return audit.status().rooms.size() == 1 && audit.status().rooms[0].name == "Renamed room" &&
        audit.status().rooms[0].status == "full" && viewerWindow->session().status().policy.viewerLimit == 1 &&
        hostWindow->findChild<QLineEdit*>("liveRoomName")->text() == "Renamed room"; });
    hostWindow->findChild<QCheckBox*>("livePublicRoom")->setChecked(false);
    hostWindow->findChild<QPushButton*>("updateRoomPolicy")->click();
    Wait([&] { return audit.status().rooms.empty() && !viewerWindow->session().status().policy.publicRoom && !hostWindow->session().roomUpdatePending(); });
    const auto frameCount = frames;
    Wait([&] { return frames >= frameCount + 10; }); // Mutations preserve the media session.
    RoomUpdateError mutationError = RoomUpdateError::None;
    viewerWindow->session().roomUpdated = [&](const auto& result) { mutationError = result.error; };
    viewerWindow->session().updatePolicy({"Unauthorized", true, 4}, viewerWindow->session().status().revision);
    Wait([&] { return !viewerWindow->session().roomUpdatePending(); }); Check(mutationError == RoomUpdateError::Forbidden);
    viewerWindow->session().updateNickname(std::string(33, 'x'), viewerWindow->session().status().revision);
    Wait([&] { return !viewerWindow->session().roomUpdatePending(); }); Check(mutationError == RoomUpdateError::Invalid);
    host.activeSession()->close();
    Wait([&] { return !host.activeSession() && host.isVisible() && audit.status().rooms.empty() &&
        viewer.activeSession()->session().status().phase == RoomPhase::Stopped; });
    viewer.activeSession()->close();
    Wait([&] { return !viewer.activeSession() && viewer.directory().status().phase == Directory::Phase::Ready; });
    // New room and viewer sessions consume saved defaults, including actual
    // runtime settings. Passwords/source handles/device identifiers stay absent.
    host.findChild<QPushButton*>("createV2Room")->click();
    Wait([&] { return host.activeSession() && host.activeSession()->session().status().phase == RoomPhase::Active; });
    Check(host.activeSession()->findChild<QSpinBox*>("streamWidth")->value() == 1280);
    Check(host.activeSession()->session().status().stream.preferences.width == 1280);
    viewer.findChild<QLineEdit*>("joinRoomId")->setText(QString::fromStdString(host.activeSession()->session().status().roomId));
    viewer.findChild<QPushButton*>("joinV2Room")->click();
    Wait([&] { return viewer.activeSession() && viewer.activeSession()->session().status().activePeers == 1; });
    Check(viewer.activeSession()->findChild<QSpinBox*>("playbackVolume")->value() == 37);
    Check(viewer.activeSession()->findChild<QCheckBox*>("playbackMuted")->isChecked());
    Check(viewer.activeSession()->session().status().playback.selected.volume == 37);
    Check(viewer.activeSession()->session().status().playback.selected.muted);
    for (const auto& path : {hostFile, viewerFile}) {
        QSettings saved(path, QSettings::IniFormat);
        for (const auto& key : saved.allKeys()) Check(key == "nickname" || key == "stream/v1" || key == "playback/v1" || key == "decoder/v1");
    }
    host.activeSession()->close(); viewer.activeSession()->close();
    Wait([&] { return !host.activeSession() && !viewer.activeSession() && viewer.directory().status().phase == Directory::Phase::Ready; });
    // Rapid visibility changes during an outstanding stop reopen just one current subscription.
    viewer.hide(); viewer.show(); viewer.hide(); viewer.show();
    Wait([&] { return viewer.directory().status().phase == Directory::Phase::Ready; });
    const auto attempts = viewer.directory().connectionAttempts();
    Check(viewer.directory().Start(origin));
    QTimer idle; bool idleDone = false; idle.setSingleShot(true); QObject::connect(&idle, &QTimer::timeout, [&] { idleDone = true; }); idle.start(150);
    Wait([&] { return idleDone; }); Check(viewer.directory().connectionAttempts() == attempts);
    // Close the real shell during a live session, including repeated close clicks.
    host.findChild<QPushButton*>("createV2Room")->click();
    Wait([&] { return host.activeSession() && host.activeSession()->session().status().phase == RoomPhase::Active; });
    unsigned hostClosed = 0, viewerClosed = 0;
    hostApp.closed = [&] { ++hostClosed; }; viewerApp.closed = [&] { ++viewerClosed; };
    Check(!hostApp.window().close()); Check(!hostApp.window().close());
    Check(hostApp.window().isVisible());
    viewerApp.window().close(); audit.Stop();
    Wait([&] { return hostClosed == 1 && viewerClosed == 1 && !host.directory().running() && !viewer.directory().running() && !audit.running(); });
    Check(!host.activeSession() && !hostApp.window().isVisible() && !viewerApp.window().isVisible());
    Check(!hostApp.keepingScreenAwake() && !viewerApp.keepingScreenAwake());
    Check(hostStack->count() == 1 && viewerStack->count() == 1);
    // Production facade rejects plaintext without opening a connection.
    Directory secure; Check(!secure.Start(origin)); Check(secure.connectionAttempts() == 0 && secure.status().phase == Directory::Phase::Failed);
    // Cancellation before the first network event must drain through the same
    // shell path, without briefly reopening the directory or orphaning a page.
    {
        RoomApplication pending(origin, Factory(audio), true, profiles.filePath("pending.ini"), false);
        unsigned closed = 0; pending.closed = [&] { ++closed; };
        pending.show(); pending.window().close(); pending.window().close();
        Wait([&] { return closed == 1 && !pending.browser()->directory().running(); });
        Check(pending.finished() && !pending.window().isVisible() && !pending.keepingScreenAwake());
    }
    {
        RoomSessionConfig config; config.room.origin = origin.toString().toStdString();
        config.room.host = true; config.room.nickname = "Cancelled host"; config.room.name = "Cancelled room";
        RoomApplication pending(config, Factory(audio), true);
        unsigned closed = 0; pending.closed = [&] { ++closed; };
        pending.show(); pending.window().close(); pending.window().close();
        Wait([&] { return closed == 1 && !pending.session()->session().running(); });
        Check(pending.finished() && !pending.window().isVisible() && !pending.keepingScreenAwake());
    }
}
void SourceSwitchScenario(const std::string& origin) {
    RoomSessionConfig config; config.room.origin = origin; config.room.host = true;
    config.room.nickname = "Source host"; config.room.name = "Source switch";
    config.media.preferences.resolution = ResolutionMode::Native;
    auto hostAudio = std::make_shared<proof::AudioEvidence>(); hostAudio->captureUnavailable = true;
    RoomApplication hostApp(config, Factory(hostAudio), true); hostApp.show();
    auto& host = *hostApp.session();
    Wait([&] { return host.session().status().phase == RoomPhase::Active; });
    config.room.host = false; config.room.roomId = host.session().status().roomId;
    auto audio = std::make_shared<proof::AudioEvidence>();
    RoomApplication viewerApp(config, Factory(audio), true); viewerApp.show();
    auto& viewer = *viewerApp.session();
    unsigned frames = 0, lastWidth = 0; auto present = viewer.session().frameReady;
    viewer.session().frameReady = [&](auto frame) { ++frames; lastWidth = frame.width; present(std::move(frame)); };
    Wait([&] { return frames >= 10 && host.findChild<QPushButton*>("switchCaptureSource")->isEnabled(); });
    Wait([&] { return host.session().status().audio.health.state == AudioEndpointState::Failed &&
        host.findChild<QLabel*>("audioHealth")->text().contains("Audio capture failed"); });
    const auto silentFrames = frames;
    Wait([&] { return frames >= silentFrames + 10 && audio->quietStreak >= 10; });
    Check(hostAudio->captureStarts == 1 && audio->audibleBlocks == 0 && host.session().status().audio.revision == 1);
    hostAudio->captureUnavailable = false;
    auto* retryCaptureAudio = host.findChild<QPushButton*>("switchAudioSource");
    Check(retryCaptureAudio->text() == "Retry selected audio"); retryCaptureAudio->click();
    Wait([&] { return host.session().status().audio.health.state == AudioEndpointState::Running && audio->audibleBlocks >= 10; });
    Check(host.session().status().audio.health.failures == 1);
    const auto roomBefore = host.session().status(); const auto widthBefore = lastWidth;
    auto* choices = host.findChild<QComboBox*>("liveCaptureSource");
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
    proof::TestWindow replacement; replacement.Resize();
    choices->addItem("Replacement proof window", QVariantMap{{"window", QVariant::fromValue<qulonglong>(reinterpret_cast<uint64_t>(replacement.handle()))}});
#else
    choices->addItem("Replacement synthetic display", QVariantMap{{"display", 1}});
#endif
    choices->setCurrentIndex(choices->count() - 1);
    host.findChild<QPushButton*>("switchCaptureSource")->click();
    try { Wait([&] { return !host.session().capturePending() && host.session().status().capture.revision == roomBefore.capture.revision + 1 && lastWidth != widthBefore; }); }
    catch (...) {
        const auto status = host.session().status();
        std::cerr << "Switch revision " << status.capture.revision << " before " << roomBefore.capture.revision << " widths " << widthBefore << "/" << lastWidth
            << " phase " << int(status.phase) << " pending " << host.session().capturePending() << " frames " << frames
            << " message " << host.findChild<QLabel*>("captureState")->text().toStdString() << '\n'; throw;
    }
    Check(host.session().status().roomId == roomBefore.roomId && host.session().status().peerId == roomBefore.peerId && host.session().status().activePeers == 1 &&
        host.session().status().revision == roomBefore.revision && host.session().status().stream.requestedRevision == roomBefore.stream.requestedRevision);
    const auto changedRevision = host.session().status().capture.revision;
    CaptureUpdateError error = CaptureUpdateError::None;
    auto notify = host.session().captureUpdated;
    host.session().captureUpdated = [&](const auto& result) { error = result.error; notify(result); };
    host.session().switchCapture({CaptureKind::Window, 0, 1, 30});
    Wait([&] { return !host.session().capturePending(); });
    Check(error == CaptureUpdateError::Failed && host.session().status().capture.revision == changedRevision);
    const auto beforeFrames = frames; const auto beforeAudio = audio->audibleBlocks.load();
    Wait([&] { return frames >= beforeFrames + 10 && audio->audibleBlocks > beforeAudio; });
    viewer.session().captureUpdated = [&](const auto& result) { error = result.error; };
    viewer.session().switchCapture({CaptureKind::Display, 0, 0, 30});
    Wait([&] { return !viewer.session().capturePending(); }); Check(error == CaptureUpdateError::Unsupported);
    // Switch the actual host widgets to production device-free silence. Verify
    // decoded Opus becomes silent, then resumes, without rebuilding the room/video.
    const auto audioRevision = host.session().status().audio.revision;
    auto* audioKind = host.findChild<QComboBox*>("liveAudioKind");
    auto* switchAudio = host.findChild<QPushButton*>("switchAudioSource");
    Wait([&] { return switchAudio->isEnabled(); }); audioKind->setCurrentIndex(3);
    Check(!host.findChild<QComboBox*>("liveAudioDevice")->isEnabled() &&
        !host.findChild<QSpinBox*>("liveAudioProcess")->isEnabled() &&
        !host.findChild<QPushButton*>("refreshAudioDevices")->isEnabled());
    switchAudio->click();
    Wait([&] { return !host.session().audioPending() && host.session().status().audio.revision == audioRevision + 1; });
    Check(host.session().status().audio.selected.kind == AudioKind::None &&
        host.findChild<QLabel*>("audioState")->text() == "No audio is being shared.");
    const auto settle = std::chrono::steady_clock::now() + 3s;
    Wait([&] { Check(std::chrono::steady_clock::now() < settle); return audio->quietStreak >= 30; });
    const auto quiet = audio->audibleBlocks.load(); const auto videoBefore = frames;
    const auto quietUntil = std::chrono::steady_clock::now() + 300ms;
    Wait([&] { return std::chrono::steady_clock::now() >= quietUntil; });
    Check(audio->audibleBlocks == quiet && frames > videoBefore);
    auto* devices = host.findChild<QComboBox*>("liveAudioDevice");
    audioKind->setCurrentIndex(0); devices->addItem("Injected missing device", "invalid"); devices->setCurrentIndex(devices->count() - 1);
    Wait([&] { return switchAudio->isEnabled(); }); switchAudio->click();
    Wait([&] { return !host.session().audioPending(); });
    Check(host.session().status().audio.revision == audioRevision + 1 && host.findChild<QLabel*>("audioState")->text().contains("previous source"));
    devices->setCurrentIndex(0); Wait([&] { return switchAudio->isEnabled(); }); switchAudio->click();
    Wait([&] { return !host.session().audioPending() && host.session().status().audio.revision == audioRevision + 2 && audio->audibleBlocks >= quiet + 10; });
    Check(!host.session().status().audio.microphoneProcessing);
    audioKind->setCurrentIndex(1); Wait([&] { return switchAudio->isEnabled(); }); switchAudio->click();
    Wait([&] { return !host.session().audioPending() && host.session().status().audio.microphoneProcessing &&
        host.findChild<QLabel*>("audioHealth")->text().contains("noise suppression") && audio->quietStreak >= 20; });
    const auto microphoneRevision = host.session().status().audio.revision;
    hostAudio->captureUnavailable = true;
    Wait([&] { return host.session().status().audio.health.state == AudioEndpointState::Failed; });
    const auto microphoneFailureFrames = frames;
    Wait([&] { return frames > microphoneFailureFrames + 10; });
    hostAudio->captureUnavailable = false;
    Wait([&] { return switchAudio->isEnabled(); }); switchAudio->click();
    Wait([&] { return !host.session().audioPending() && host.session().status().audio.revision == microphoneRevision + 1 &&
        host.session().status().audio.health.state == AudioEndpointState::Running; });
    Check(host.session().status().audio.microphoneProcessing);
    // Process audio must bypass the just-retired microphone processor too.
    audioKind->setCurrentIndex(2); host.findChild<QSpinBox*>("liveAudioProcess")->setValue(1);
    const auto beforeProcessAudio = audio->audibleBlocks.load();
    Wait([&] { return switchAudio->isEnabled(); }); switchAudio->click();
    Wait([&] { return !host.session().audioPending() && host.session().status().audio.selected.kind == AudioKind::Process &&
        !host.session().status().audio.microphoneProcessing && audio->audibleBlocks > beforeProcessAudio + 10; });
    audioKind->setCurrentIndex(0); Wait([&] { return switchAudio->isEnabled(); }); switchAudio->click();
    Wait([&] { return !host.session().audioPending() && host.session().status().audio.selected.kind == AudioKind::System; });
    const auto afterAudio = host.session().status();
    Check(afterAudio.roomId == roomBefore.roomId && afterAudio.peerId == roomBefore.peerId && afterAudio.revision == roomBefore.revision &&
        afterAudio.stream.requestedRevision == roomBefore.stream.requestedRevision && afterAudio.activePeers == 1);
    viewer.session().audioUpdated = [&](const auto& result) { error = result.error; };
    viewer.session().switchAudio({}); Wait([&] { return !viewer.session().audioPending(); }); Check(error == AudioUpdateError::Unsupported);
    const auto viewerBefore = viewer.session().status();
    auto* applyPlayback = viewer.findChild<QPushButton*>("applyPlayback");
    auto* mute = viewer.findChild<QCheckBox*>("playbackMuted");
    auto* volume = viewer.findChild<QSpinBox*>("playbackVolume");
    auto* output = viewer.findChild<QComboBox*>("playbackDevice");
    Wait([&] { return applyPlayback->isEnabled(); }); mute->setChecked(true); applyPlayback->click();
    Wait([&] { return !viewer.session().playbackPending() && audio->quietStreak >= 10; });
    Check(viewer.session().status().playback.selected.muted);
    const auto quietPlayback = audio->audibleBlocks.load(); const auto framesBeforePlayback = frames;
    Wait([&] { return frames >= framesBeforePlayback + 10; }); Check(audio->audibleBlocks == quietPlayback);
    output->addItem("Replacement synthetic output", "replacement"); output->setCurrentIndex(output->count() - 1);
    mute->setChecked(false); volume->setValue(50); Wait([&] { return applyPlayback->isEnabled(); }); applyPlayback->click();
    Wait([&] { return !viewer.session().playbackPending() && audio->audibleBlocks > quietPlayback + 10; });
    Check(viewer.session().status().playback.selected.volume == 50 && viewer.session().status().playback.selected.deviceId == L"replacement");
    const auto playbackRevision = viewer.session().status().playback.revision;
    output->addItem("Missing output", "invalid"); output->setCurrentIndex(output->count() - 1);
    Wait([&] { return applyPlayback->isEnabled(); }); applyPlayback->click();
    Wait([&] { return !viewer.session().playbackPending(); });
    Check(viewer.session().status().playback.revision == playbackRevision && viewer.findChild<QLabel*>("playbackState")->text().contains("Previous settings"));
    output->setCurrentIndex(output->findData("replacement"));
    audio->outputUnavailable = true;
    Wait([&] { return viewer.session().status().playback.health.state == AudioEndpointState::Failed &&
        viewer.findChild<QLabel*>("playbackHealth")->text().contains("Audio output failed"); });
    const auto failureFrames = frames; const auto startsBeforeRetry = audio->outputStarts.load();
    Wait([&] { return frames >= failureFrames + 10; });
    Check(audio->outputStarts == startsBeforeRetry && viewer.session().status().playback.revision == playbackRevision);
    audio->outputUnavailable = false; const auto recoveredAudio = audio->audibleBlocks.load();
    Wait([&] { return applyPlayback->isEnabled(); }); Check(applyPlayback->text() == "Retry playback"); applyPlayback->click();
    Wait([&] { return !viewer.session().playbackPending() && audio->audibleBlocks >= recoveredAudio + 10 &&
        viewer.session().status().playback.health.state == AudioEndpointState::Running; });
    Check(audio->outputStarts == startsBeforeRetry + 1 && viewer.session().status().playback.health.failures == 1 &&
        viewer.session().status().playback.selected.deviceId == L"replacement");
    Check(viewer.session().status().roomId == viewerBefore.roomId && viewer.session().status().peerId == viewerBefore.peerId &&
        viewer.session().status().revision == viewerBefore.revision);
    host.session().playbackUpdated = [&](const auto& result) { error = result.error; };
    host.session().updatePlayback({}); Wait([&] { return !host.session().playbackPending(); }); Check(error == AudioUpdateError::Unsupported);
    hostApp.window().close(); viewerApp.window().close();
    Wait([&] { return hostApp.finished() && viewerApp.finished() && !hostApp.window().isVisible() && !viewerApp.window().isVisible(); });
    Check(!host.session().running() && !viewer.session().running());
    Check(!hostApp.keepingScreenAwake() && !viewerApp.keepingScreenAwake());
}
void MutationAcknowledgementScenario(const std::string& origin) {
    RoomSessionConfig config; config.room.origin = origin; config.room.host = true;
    config.room.nickname = "Initial host"; config.room.name = "Acknowledgement recovery";
    RoomSessionWindow host(config, Factory(std::make_shared<proof::AudioEvidence>()), true); host.show();
    Wait([&] { return host.session().status().phase == RoomPhase::Active; });
    config.room.host = false; config.room.roomId = host.session().status().roomId; config.room.nickname = "Viewer";
    RoomSessionWindow viewer(config, Factory(std::make_shared<proof::AudioEvidence>()), true); viewer.show();
    unsigned frames = 0; auto present = viewer.session().frameReady;
    viewer.session().frameReady = [&](auto frame) { ++frames; present(std::move(frame)); };
    Wait([&] { return frames >= 10 && host.session().status().members.size() == 2; });
    const auto revision = host.session().status().revision;
    std::vector<RoomUpdateResult> results;
    auto notify = host.session().roomUpdated;
    host.session().roomUpdated = [&](const auto& result) { results.push_back(result); notify(result); };
    const auto started = std::chrono::steady_clock::now();
    host.session().updateNickname("Committed without reply", revision);
    Wait([&] { return host.session().status().revision == revision + 1; });
    Check(host.session().roomUpdatePending());
    Wait([&] { return results.size() == 1; });
    Check(results[0].error == RoomUpdateError::Unconfirmed && std::chrono::steady_clock::now() - started >= 10s);
    Check(host.findChild<QLabel*>("roomUpdateState")->text().contains("unknown"));
    Check(host.session().status().revision == revision + 1 && frames >= 30); // No automatic mutation retry.
    host.session().updatePolicy({"Reviewed second change", false, 4}, revision + 1);
    Wait([&] { return host.session().status().revision == revision + 2; });
    // First request's late response must not resolve the newer in-flight request.
    Wait([&] { return std::chrono::steady_clock::now() - started >= 13s; });
    Check(host.session().roomUpdatePending() && results.size() == 1);
    Wait([&] { return results.size() == 2; });
    Check(results[1].error == RoomUpdateError::None);
    Check(host.session().status().revision == revision + 2 && viewer.session().status().policy.name == "Reviewed second change");
    host.close(); viewer.close(); Wait([&] { return !host.session().running() && !viewer.session().running(); });
    std::cout << "{\"passed\":true,\"missing_ack_timeout\":true,\"late_ack_isolated\":true,\"no_retry\":true,\"frames\":" << frames << "}\n";
}
void ControllerScenario(const std::string& origin, bool physicalReader = false) {
    const auto devices = physicalReader ? screenshare::ViewerGamepad::ConnectedDevices() :
        std::vector<screenshare::ViewerGamepadDevice>{{"test-pad", "Recording controller"}};
    if (devices.size() != 1) throw std::runtime_error("Controller fixture requires exactly one connected device; found " + std::to_string(devices.size()));
    auto sink = std::make_shared<RecordingGamepadSink>();
    RoomSessionConfig config; config.room.origin = origin; config.room.host = true;
    config.room.nickname = "Controller host"; config.room.name = "Controller test";
    config.media.preferences.resolution = ResolutionMode::Native;
    config.media.inputSink = sink;
    RoomSessionWindow host(config, Factory(std::make_shared<proof::AudioEvidence>()), true);
    host.setAttribute(Qt::WA_ShowWithoutActivating); host.show();
    try { Wait([&] { return host.session().status().phase == RoomPhase::Active; }); }
    catch (...) { throw std::runtime_error("Controller host startup: phase=" + std::to_string(int(host.session().status().phase)) +
        "; error=" + std::to_string(int(host.session().status().error))); }
    config.room.host = false; config.room.roomId = host.session().status().roomId;
    config.room.nickname = "Controller viewer"; config.media.inputSink.reset();
    std::atomic<bool> plugged{true}; std::atomic<uint16_t> buttons{1};
    RoomSessionWindow viewer(config, Factory(std::make_shared<proof::AudioEvidence>()), true, nullptr,
        [devices] { return devices; },
        [&](std::string_view device) -> std::optional<screenshare::RemoteGamepadState> {
            Check(device == devices.front().id); if (!plugged) return {};
            if (physicalReader) return screenshare::ViewerGamepad::ReadState(device);
            screenshare::RemoteGamepadState state; state.buttons = buttons.load(); return state;
        });
    viewer.setAttribute(Qt::WA_ShowWithoutActivating); viewer.show();
    Wait([&] { return viewer.session().status().activePeers == 1 && viewer.session().input(); });
    viewer.findChild<QPushButton*>("refreshControllers")->click();
    auto* viewerConsent = viewer.findChild<QCheckBox*>("controllerConsent");
    auto* hostConsent = host.findChild<QCheckBox*>("controllerConsent");
    auto* request = viewer.findChild<QPushButton*>("controllerAction");
    auto* grant = host.findChild<QPushButton*>("controllerAction");
    Wait([&] { return host.findChild<QComboBox*>("controllerPeer")->count() == 1; });
    Wait([&] { return viewer.findChild<QComboBox*>("controllerPeer")->count() == 1; });
    unsigned authorization = 0;
    std::deque<std::string> transitions;
    std::string previousTransition;
    QTimer trace;
    QObject::connect(&trace, &QTimer::timeout, [&] {
        std::string line = "grant=" + std::to_string(authorization);
        for (auto* window : {&host, &viewer}) {
            line += window == &host ? " host:" : " viewer:";
            if (auto port = window->session().input()) for (const auto& state : port->Read())
                line += " epoch=" + std::to_string(state.permission) + " granted=" + std::to_string(state.granted) +
                    " requested=" + std::to_string(state.requested) + " reason=" + std::to_string(int(state.reason)) +
                    " releasing=" + std::to_string(state.revokePending);
        }
        if (line != previousTransition) {
            previousTransition = line; transitions.push_back(std::move(line));
            if (transitions.size() > 64) transitions.pop_front();
        }
    });
    trace.start(1);
    auto authorize = [&] {
        ++authorization;
        const auto before = sink->applied.load();
        viewerConsent->setChecked(true);
        try { Wait([&] { return request->isEnabled(); }); }
        catch (...) { throw std::runtime_error("Controller request disabled: " + viewer.findChild<QLabel*>("controllerStatus")->text().toStdString() +
            "; consent=" + std::to_string(viewerConsent->isChecked()) + "; devices=" + std::to_string(viewer.findChild<QComboBox*>("controllerDevice")->count())); }
        request->click();
        Check(!grant->isEnabled()); hostConsent->setChecked(true);
        Wait([&] { return grant->isEnabled(); }); grant->click();
        Check(!hostConsent->isChecked());
        try { Wait([&] { return physicalReader ? sink->applied > before : sink->buttons == buttons.load(); }); }
        catch (...) {
            for (const auto& line : transitions) std::cerr << line << '\n';
            throw std::runtime_error("Controller state missing at grant " + std::to_string(authorization) +
            "; host=" + host.findChild<QLabel*>("controllerStatus")->text().toStdString() +
            "; viewer=" + viewer.findChild<QLabel*>("controllerStatus")->text().toStdString()); }
    };
    Check(!request->isEnabled() && !grant->isEnabled()); authorize();
    // Exercise complete permission/poller lifetimes without refreshing the
    // selected device. Every cycle requires new consent at both ends.
    for (int cycle = 0; cycle < 10; ++cycle) {
        host.revokeControl();
        Wait([&] { return sink->buttons == 0 && !viewerConsent->isChecked(); });
        authorize();
    }
    if (!physicalReader) { buttons = 2; Wait([&] { return sink->buttons == 2; }); }
    QEvent inactive(QEvent::WindowDeactivate); QApplication::sendEvent(&viewer, &inactive);
    Wait([&] { return sink->buttons == 0 && !viewerConsent->isChecked(); });
    authorize(); host.revokeControl(); Wait([&] { return sink->buttons == 0 && !viewerConsent->isChecked(); });
    authorize();
    CaptureSelection selection;
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
    selection.kind = CaptureKind::Window; selection.window = reinterpret_cast<uint64_t>(captureWindow);
#else
    selection.display = 1;
#endif
    host.session().switchCapture(selection);
    Wait([&] { return !host.session().capturePending() && sink->buttons == 0 && !viewerConsent->isChecked(); });
    authorize(); plugged = false;
    Wait([&] { return sink->buttons == 0 && !viewerConsent->isChecked(); });
    Check(sink->released >= 3 && sink->applied >= 4 && host.session().status().activePeers == 1);
    plugged = true; sink->fail = true;
    viewerConsent->setChecked(true); Wait([&] { return request->isEnabled(); }); request->click();
    hostConsent->setChecked(true); Wait([&] { return grant->isEnabled(); }); grant->click();
    Wait([&] { return !viewerConsent->isChecked() && host.findChild<QLabel*>("controllerStatus")->text().contains("unavailable"); });
    Check(host.session().status().activePeers == 1 && !sink->buttons);
    viewer.session().stop(); host.session().stop();
    Wait([&] { return !viewer.session().running() && !host.session().running(); });
}
class RecordingInputPresentation final:public FramePresentationBackend {
public:
    bool Present(HWND,uint32_t,uint32_t,bool,bool,const screenshare::Nv12D3D11Presenter::FrameView& frame,screenshare::Nv12D3D11Presenter::ScaleMode) override {return frame.width>0 && (frame.texture || frame.dataSize>0);}
    void Reset() noexcept override {}
    void Update(HWND,uint32_t,uint32_t,bool,bool,screenshare::Nv12D3D11Presenter::ScaleMode) override {}
    uint32_t MaximumFrameLatency() const noexcept override {return 1;}
};
void DesktopInputScenario(const std::string& origin) {
    auto target=std::make_shared<screenshare::input::DesktopTargetState>();
    auto evidence=std::make_shared<DesktopInputEvidence>();
    RoomSessionConfig config;config.room.origin=origin;config.room.host=true;config.room.name="Mapped input";config.room.nickname="Input host";
    config.media.inputTarget=target;
    config.media.inputSink=std::make_shared<screenshare::input::DesktopSink>(target,nullptr,
        [evidence](auto,uint8_t){return std::make_unique<RecordingDesktopDevice>(evidence);});
    config.media.preferences.resolution=ResolutionMode::Fixed;config.media.preferences.width=640;config.media.preferences.height=480;
    RoomSessionWindow host(config,Factory(std::make_shared<proof::AudioEvidence>()),true);host.setAttribute(Qt::WA_ShowWithoutActivating);host.show();
    Wait([&]{return host.session().status().phase==RoomPhase::Active;});
    config.room.host=false;config.room.roomId=host.session().status().roomId;config.room.nickname="Input viewer";
    config.media.inputSink.reset();
    FramePresentationFactory presentation;
#ifndef SCREENSHARE_WINDOWS_UI_PROOF
    presentation=[] {return std::make_unique<RecordingInputPresentation>();};
#endif
    RoomSessionWindow viewer(config,Factory(std::make_shared<proof::AudioEvidence>()),true,nullptr,
        [] {return std::vector<screenshare::ViewerGamepadDevice>{};},[](auto)->std::optional<screenshare::RemoteGamepadState>{return {};},presentation);
    viewer.setAttribute(Qt::WA_ShowWithoutActivating);viewer.show();
    auto* video=dynamic_cast<VideoFrameWidget*>(viewer.findChild<QWidget*>("roomVideo"));Check(video!=nullptr);
    Wait([&]{return video->presentedInputMapping().Valid();});
    const auto original=video->presentedInputMapping();Check(original.width==640 && original.height==480);
    auto* viewerCaps=viewer.findChild<QComboBox*>("inputCapabilities");auto* hostCaps=host.findChild<QComboBox*>("inputCapabilities");
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
    constexpr uint8_t caps=screenshare::input::Mouse;
#else
    constexpr uint8_t caps=screenshare::input::Mouse|screenshare::input::Keyboard;
    Check(original.top>0); // Encoded padding, in addition to widget letterboxing.
#endif
    viewerCaps->setCurrentIndex(viewerCaps->findData(caps));hostCaps->setCurrentIndex(hostCaps->findData(caps));
    auto authorize=[&] {
        Wait([&]{return viewer.findChild<QComboBox*>("controllerPeer")->count()==1 && host.findChild<QComboBox*>("controllerPeer")->count()==1;});
        viewer.findChild<QCheckBox*>("controllerConsent")->setChecked(true);
        auto* request=viewer.findChild<QPushButton*>("controllerAction");Wait([&]{return request->isEnabled();});request->click();
        auto* grant=host.findChild<QPushButton*>("controllerAction");Check(!grant->isEnabled());
        host.findChild<QCheckBox*>("controllerConsent")->setChecked(true);Wait([&]{return grant->isEnabled();});grant->click();
        Wait([&]{for(const auto& state:viewer.session().input()->Read())if(state.granted==caps)return true;return false;});
        // Drive the real preview event route after the panel's grant observation.
        Wait([&]{return video->hasMouseTracking();});
    };
    auto move=[&](QPointF point) {QMouseEvent event(QEvent::MouseMove,point,point,Qt::NoButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(video,&event);};
    authorize();move(QPointF(video->width()/2.0,video->height()/2.0));
    Wait([&]{return evidence->applied>0;});Check(std::abs(evidence->x-.5f)<.02f && std::abs(evidence->y-.5f)<.02f);
    const auto before=evidence->applied.load();move({0,0});
    const auto until=std::chrono::steady_clock::now()+80ms;Wait([&]{return std::chrono::steady_clock::now()>until;});Check(evidence->applied==before);
#ifndef SCREENSHARE_WINDOWS_UI_PROOF
    QKeyEvent key(QEvent::KeyPress,Qt::Key_A,Qt::NoModifier,0x1e,0x41,0);QApplication::sendEvent(video,&key);
    Wait([&]{return evidence->keys==1;});
#endif
    QEvent inactive(QEvent::WindowDeactivate);QApplication::sendEvent(&viewer,&inactive);
    Wait([&]{return evidence->released>0 && !viewer.findChild<QCheckBox*>("controllerConsent")->isChecked();});
    authorize();
    CaptureSelection selection;
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
    selection.kind=CaptureKind::Window;selection.window=reinterpret_cast<uint64_t>(captureWindow);
#else
    selection.display=1;
#endif
    host.session().switchCapture(selection);
    Wait([&]{return !host.session().capturePending() && video->presentedInputMapping().generation!=original.generation && video->presentedInputMapping().Valid();});
    Wait([&]{return !viewer.findChild<QCheckBox*>("controllerConsent")->isChecked();});authorize();
    const auto applied=evidence->applied.load();screenshare::input::Event stale;stale.kind=screenshare::input::Kind::Pointer;stale.sourceGeneration=original.generation;
    const auto peer=viewer.session().input()->Read().front().peer;Check(viewer.session().input()->Submit(peer,stale));
    Wait([&]{return !viewer.session().input()->Read().front().granted;});Check(evidence->applied==applied);
    viewer.session().stop();host.session().stop();Wait([&]{return !viewer.session().running() && !host.session().running();});
}
int main(int argc, char** argv) {
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString&) {});
    QApplication application(argc, argv); application.setQuitOnLastWindowClosed(false);
    QApplication::setStyle("Fusion");
#ifndef SCREENSHARE_WINDOWS_UI_PROOF
    // Qt's offscreen backend does not enumerate the Windows font directory.
    const auto fonts = QDir(qEnvironmentVariable("WINDIR", "C:/Windows")).filePath("Fonts");
    Check(QFontDatabase::addApplicationFont(QDir(fonts).filePath("segoeui.ttf")) >= 0);
    Check(QFontDatabase::addApplicationFont(QDir(fonts).filePath("segoeuib.ttf")) >= 0);
#endif
    webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
    webrtc::InitializeLogging(std::move(logging)); webrtc::WinsockInitializer winsock;
    if (winsock.error() || !webrtc::InitializeSSL()) return 1;
    int result = 0;
    try {
        Check(argc == 2 || (argc == 3 && (std::string(argv[2]) == "mutation-ack-delay" || std::string(argv[2]) == "controllers" || std::string(argv[2]) == "controllers-physical" || std::string(argv[2]) == "desktop-input")));
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
        screenshare::WindowsMediaRuntime mediaRuntime; Check(SUCCEEDED(mediaRuntime.result()));
        NativePresentationRecovery();
        proof::TestWindow capture; captureWindow = capture.handle();
#endif
        if(argc==3 && std::string(argv[2])=="desktop-input") {
            DesktopInputScenario(argv[1]);std::cout<<"{\"passed\":true,\"mapped_input\":true,\"physical_input\":false}\n";
        }
        else if (argc == 3 && (std::string(argv[2]) == "controllers" || std::string(argv[2]) == "controllers-physical")) {
            const bool physicalReader = std::string(argv[2]) == "controllers-physical";
            ControllerScenario(argv[1], physicalReader);
            std::cout << "{\"passed\":true,\"controllers\":true,\"physical_input\":false,\"physicalControllerRead\":"
                      << (physicalReader ? "true" : "false") << ",\"successfulGrantCycles\":14,\"deniedGrantChecks\":1}\n";
        }
        else if (argc == 3) MutationAcknowledgementScenario(argv[1]);
        else {
        RoomSessionConfig config; config.room.origin = argv[1]; config.room.host = true;
        config.room.nickname = "UiHost"; config.room.name = "UI media";
        QTemporaryDir reports; Check(reports.isValid()); config.reportFile = reports.filePath("room-report.json");
        config.media.preferences.resolution = ResolutionMode::Fixed;
        config.media.preferences.width = 320; config.media.preferences.height = 180; config.media.preferences.fps = 30;
        auto hostAudio = std::make_shared<proof::AudioEvidence>();
        RoomSessionWindow host(config, Factory(hostAudio), true); host.show();
        Wait([&] { return host.session().status().phase == RoomPhase::Active; });
        Check(!host.session().start(config));
        host.findChild<QPushButton*>("saveRoomReport")->click();
        { QFile saved(config.reportFile); Check(saved.open(QIODevice::ReadOnly)); const auto bytes = saved.readAll();
          Check(!bytes.contains("UiHost") && !bytes.contains("UI media"));
          Check(host.findChild<QLabel*>("roomReportResult")->text().startsWith("Saved diagnostic report:")); }
        config.room.host = false; config.room.roomId = host.session().status().roomId; config.room.nickname = "UiHost";
        auto viewerAudio = std::make_shared<proof::AudioEvidence>();
        RoomSessionWindow viewer(config, Factory(viewerAudio), true); viewer.show();
        unsigned original = 0, changed = 0;
        auto present = viewer.session().frameReady;
        viewer.session().frameReady = [&](auto frame) {
            Check(frame.pixels().size() == frame.width * frame.height * 3 / 2 && frame.nv12.empty());
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
            Check(bool(frame.native));
#else
            Check(bool(frame.retainedPixels));
#endif
            if (frame.width == 320) ++original; else if (frame.width == 160) ++changed; else Check(false);
            present(std::move(frame));
        };
        Wait([&] { return original >= 20 && viewerAudio->audibleBlocks >= 20; });
        Wait([&] { return host.findChild<QLabel*>("roomMembers")->text().contains(QString::fromStdString(viewer.session().status().peerId)); });
        Check(host.findChild<QLabel*>("roomMembers")->text().contains(QString::fromStdString(host.session().status().peerId)));
        host.findChild<QSpinBox*>("liveViewerLimit")->setValue(5);
        Check(host.findChild<QLabel*>("capacityWarning")->isVisible());
        host.findChild<QSpinBox*>("liveViewerLimit")->setValue(4);
        Check(!host.findChild<QLabel*>("capacityWarning")->isVisible());
        auto* localDiagnostics = viewer.findChild<QLabel*>("viewerPresentationDiagnostics");
        Check(localDiagnostics);
        Wait([&] { return localDiagnostics->text().contains("do not measure end-to-end latency"); });
        Check(localDiagnostics->text().contains("Graphics errors") && localDiagnostics->text().contains("recovery backoff"));
        Check(viewer.session().frameStatistics().retained >= 20 && viewer.session().frameStatistics().converted == 0 && viewer.session().frameStatistics().repacked == 0);
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
        Check(viewer.session().frameStatistics().gpuRetained >= 20 && viewer.session().frameStatistics().gpuReadbacks >= 20);
        Check(localDiagnostics->text().contains("CPU readbacks")); // Pixel assertions explicitly request this fallback.
#endif
        auto* width = host.findChild<QSpinBox*>("streamWidth"); auto* height = host.findChild<QSpinBox*>("streamHeight");
        auto* apply = host.findChild<QPushButton*>("applyStream");
        Check(width && height && apply && apply->isEnabled());
        const auto before = host.session().status().stream.requestedRevision;
        width->setValue(321); apply->click();
        Check(!host.findChild<QLabel*>("roomError")->text().isEmpty());
        Check(host.session().status().stream.requestedRevision == before);
        // Programmatic widget actions; no physical input and no per-edit queue.
        for (int i = 0; i < 100; ++i) { width->setValue(200 + (i % 10) * 2); apply->click(); }
        width->setValue(160); height->setValue(90); apply->click();
        Check(host.session().settingsPending());
        Wait([&] {
            const auto stream = host.session().status().stream;
            return changed >= 20 && stream.requestedRevision == before + 1 && stream.peers.size() == 1 &&
                stream.peers[0].observedRevision == stream.requestedRevision && !host.session().settingsPending();
        });
        host.findChild<QCheckBox*>("uploadBudgetEnabled")->setChecked(true);
        host.findChild<QSpinBox*>("uploadBudget")->setValue(160000); apply->click();
        Wait([&] { const auto stream = host.session().status().stream;
            return stream.peers.size() == 1 && stream.peers[0].appliedRevision == stream.requestedRevision && stream.peers[0].appliedVideoBitrateBps == 0;
        });
        Wait([&] { return host.findChild<QLabel*>("uploadState")->text().contains("1 viewer(s) paused"); });
        auto* diagnostics = host.findChild<QTableWidget*>("peerDiagnostics");
        Wait([&] { return diagnostics->rowCount() == 1 && diagnostics->item(0, 1)->text() == "upload-paused"; });
        diagnostics->selectRow(0);
        Check(host.findChild<QLabel*>("peerDiagnosticsDetails")->text().contains("Physical display and end-to-end latency: unknown"));
        const auto diagnosticPeer = diagnostics->item(0, 0)->data(Qt::UserRole);
        host.findChild<QPushButton*>("openPeerDetails")->click();
        auto* popup = host.findChild<QDialog*>("peerDetailsDialog");
        auto* popupText = popup->findChild<QPlainTextEdit*>("peerDetailsText");
        Check(popup->isVisible() && !popup->isModal() && popup->property("peerId") == diagnosticPeer);
        Check(popupText->toPlainText().contains("Selected-path RTT") && popupText->toPlainText().contains("not image/input latency"));
        Check(popupText->toPlainText().contains("Source scaling:") && popupText->toPlainText().contains("Active image in source canvas:"));
        const auto drainUntil = std::chrono::steady_clock::now() + 500ms;
        Wait([&] { return std::chrono::steady_clock::now() >= drainUntil; });
        const auto pausedFrames = changed;
        const auto audioBeforePause = viewerAudio->audibleBlocks.load();
        const auto pauseUntil = std::chrono::steady_clock::now() + 300ms;
        Wait([&] { return std::chrono::steady_clock::now() >= pauseUntil; });
        Check(changed <= pausedFrames + 1 && viewerAudio->audibleBlocks > audioBeforePause);
        host.findChild<QSpinBox*>("uploadBudget")->setValue(1000000); apply->click();
        const auto resumeFrames = changed;
        Wait([&] { const auto stream = host.session().status().stream;
            return changed >= resumeFrames + 10 && stream.peers[0].appliedVideoBitrateBps == 672000;
        });
        host.findChild<QCheckBox*>("uploadBudgetEnabled")->setChecked(false); apply->click();
        Wait([&] { const auto stream = host.session().status().stream;
            return !stream.preferences.aggregateUploadLimitBps && stream.peers[0].appliedVideoBitrateBps == 2000000;
        });
        Wait([&] { return diagnostics->item(0, 1)->text() == "source-observed"; });
        Wait([&] { return diagnostics->item(0, 5)->text() == QString::fromUtf8("160 × 90"); });
        Wait([&] {
            const auto stream = host.session().status().stream;
            const auto& peer = stream.peers[0];
            return peer.receiver.observation && peer.receiver.observation->presentation &&
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
                peer.receiver.observation->decoder == CodecImplementation::MfH264Hardware &&
#else
                peer.receiver.observation->decoder == CodecImplementation::MfH264Software &&
#endif
                stream.capture.state == HostMediaState::Running && peer.delivery.delivered > 0 &&
                peer.recovery.state == PeerLifecycleState::Connected && peer.appliedPreferences &&
                peer.appliedPreferences->width == 160 && peer.settingsError == SettingsApplyError::None;
        });
        Wait([&] { return popupText->toPlainText().contains("Receiver presentation:") &&
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
            popupText->toPlainText().contains("receiver decoder: mf-h264-hardware") &&
#else
            popupText->toPlainText().contains("receiver decoder: mf-h264-software") &&
#endif
            popupText->toPlainText().contains("Capture: running"); });
        // Exercise the real frontend snapshot consumer independently of the
        // native SetParameters rejection test. No production fault switch.
        Wait([&] { const auto stream = host.session().status().stream;
            return !host.session().settingsPending() && stream.peers.size() == 1 &&
                stream.peers[0].observedRevision == stream.requestedRevision &&
                stream.peers[0].appliedRevision == stream.requestedRevision;
        });
        auto partialStatus = host.session().status();
        auto rejectedPeer = partialStatus.stream.peers[0]; rejectedPeer.peerId = "rejected-fixture";
        rejectedPeer.rejected = true; rejectedPeer.settingsError = SettingsApplyError::SenderRejected;
        --rejectedPeer.appliedRevision;
        partialStatus.stream.peers.push_back(rejectedPeer);
        host.session().statusChanged(partialStatus);
        Check(host.findChild<QLabel*>("streamSettingsState")->text().contains("1 applied, 0 pending, 1 rejected"));
        Check(host.findChild<QLabel*>("streamSettingsState")->text().contains("Apply to retry"));
        Check(diagnostics->rowCount() == 2 && diagnostics->item(1, 0)->toolTip().contains("sender-rejected"));
        host.session().statusChanged(host.session().status());
        Check(diagnostics->rowCount() == 1);
        const auto source = host.session().status().stream.peers[0].source;
#ifndef SCREENSHARE_WINDOWS_UI_PROOF
        Check(source.imageLeft == 0 && source.imageTop == 0 && source.imageWidth == 160 && source.imageHeight == 90);
        Check(source.scalingPath == SourceScalingPath::Cpu && source.scaled > 0 && source.gpuScaled == 0);
        Wait([&] { return popupText->toPlainText().contains("Source scaling: cpu."); });
#else
        // The WGC fixture is 640x400 with window chrome, not the synthetic 16:9
        // source. It must preserve that taller aspect ratio with even pillarboxes.
        Check(source.imageTop == 0 && source.imageHeight == 90 && source.imageWidth > 0 && source.imageWidth < 160);
        Check(source.imageLeft > 0 && source.imageLeft % 2 == 0 && source.imageWidth % 2 == 0);
        Check(std::abs(160 - source.imageWidth - 2 * source.imageLeft) <= 2);
#endif
        Check(host.findChild<QLabel*>("peerDiagnosticsDetails")->text().contains("Receiver-reported decode"));
        Check(diagnostics->item(diagnostics->currentRow(), 0)->data(Qt::UserRole) == diagnosticPeer);
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
        auto* video = static_cast<VideoFrameWidget*>(viewer.findChild<QWidget*>("roomVideo"));
        Check(video && video->presentedFrameCount() >= 20);
        Check(video->presentationStats().maximumFrameLatency == 1);
#endif
        viewer.close(); Check(viewer.session().running()); // Close waits for drain.
        Wait([&] { return !viewer.session().running() && !viewer.isVisible(); });
        Wait([&] { return popupText->toPlainText().contains("viewer has left"); });
        Check(!host.findChild<QPushButton*>("openPeerDetails")->isEnabled());
        host.findChild<QPushButton*>("stopRoom")->click();
        Wait([&] { return !host.session().running(); });
        Check(host.session().status().phase == RoomPhase::Stopped);
        host.close();
        // Hold native retirement while proving Qt continues to dispatch timers.
        std::promise<void> release; auto barrier = release.get_future().share();
        QtRoomSession held(nullptr, [barrier](auto) { return [barrier](auto, auto) { return std::make_unique<HeldRuntime>(barrier); }; }, true);
        config.room.host = true; config.room.roomId.clear();
        unsigned finished = 0, heartbeats = 0;
        held.finished = [&](const auto&) { ++finished; };
        QTimer heartbeat; QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; }); heartbeat.start(1);
        try {
            Check(held.start(config)); Wait([&] { return held.status().phase == RoomPhase::Active; });
            held.stop(); held.stop(); const auto previous = heartbeats;
            Wait([&] { return heartbeats >= previous + 10; });
            Check(held.running() && finished == 0);
        } catch (...) { release.set_value(); throw; }
        release.set_value(); Wait([&] { return !held.running(); }); Check(finished == 1);
        Check(held.start(config)); Wait([&] { return held.status().phase == RoomPhase::Active; });
        held.stop(); Wait([&] { return !held.running(); }); Check(finished == 2);
        BrowserScenario(QUrl(QString::fromLocal8Bit(argv[1])));
        NormalHomeScenario(QUrl(QString::fromLocal8Bit(argv[1])));
        MutationLifecycle(argv[1]);
          SourceSwitchScenario(argv[1]);
        std::cout << "{\"passed\":true,\"qt_ui\":true,\"normal_home\":true,\"browser\":true,\"directory_push\":true,\"nickname_persistence\":true,\"coalesced_settings\":true,\"responsive_stop\":true,\"restart_owner\":true,\"original_frames\":" << original << ",\"changed_frames\":" << changed << "}\n";
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    webrtc::CleanupSSL(); return result;
}
