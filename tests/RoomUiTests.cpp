#include "ui/RoomSessionWindow.h"
#include "shared/RoomLink.h"
#include "shared/StreamPreferencesJson.h"
#include "shared/RoomStreamDiagnostics.h"
#include <QClipboard>
#include "ui/RoomBrowserWindow.h"
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
#include <QPlainTextEdit>
#include <iostream>
#include <source_location>
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
#include "../tools/webrtc-proof/CaptureTestWindow.h"
#include "core/WindowsMediaRuntime.h"
#include "render/Nv12D3D11Presenter.h"
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
};
void NativePresentationRecovery() {
    auto fail = std::make_shared<std::atomic_bool>(false);
    VideoFrameWidget widget(nullptr, [fail] { return std::make_unique<FaultingNativeRenderer>(fail); });
    widget.resize(320, 180); widget.setLowLatency(true); widget.show();
    auto send = [&] {
        screenshare::Nv12VideoFrame frame; frame.width = 320; frame.height = 180;
        frame.nv12.resize(320 * 180 * 3 / 2, 128); Check(widget.setVideoFrame(std::move(frame)));
    };
    Wait([&] { send(); return widget.presentedFrameCount() >= 3; });
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
}
#endif
QtRoomSession::Factory Factory(std::shared_ptr<proof::AudioEvidence> audio) {
    return [audio](WindowsRoomRuntimeOptions windows) -> RoomRuntimeFactory {
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
        windows.capture.sourceType = screenshare::CaptureSourceType::Window;
        windows.capture.windowHandle = reinterpret_cast<uint64_t>(captureWindow);
        windows.audioEndpoints = proof::SyntheticAudio(audio);
        windows.audioForSelection = proof::SyntheticAudioSelection;
        windows.playbackForSelection = [audio](auto selection) { return proof::SyntheticPlayback(selection, audio); };
        return WindowsRoomRuntimeFactory(std::move(windows));
#else
        return [windows, audio](auto identity, auto send) {
            NativeRoomRuntimeOptions options; options.preferences = windows.preferences; options.frames = windows.frames;
            auto endpoints = proof::SyntheticAudio(audio);
            if (!identity.host) {
                options.playback = std::make_shared<PlaybackControl>(PlaybackSelection{windows.playbackDeviceId, windows.playbackVolume, windows.playbackMuted}, endpoints.playout);
                endpoints.playout = [control = options.playback] { return std::make_unique<ControlledPcmPlayout>(control); };
                options.playbackForSelection = [audio](auto selection) { return proof::SyntheticPlayback(selection, audio); };
            }
            if (identity.host) {
                options.audioSwitch = std::make_shared<AudioSwitchControl>(AudioSelection{}, endpoints.capture);
                endpoints.capture = [control = options.audioSwitch] { return std::make_unique<SwitchablePcmCapture>(control); };
                options.audioForSelection = proof::SyntheticAudioSelection;
            }
            options.engine = [endpoints] {
                return std::make_unique<MediaEngine>(CreatePcmAudioDeviceModule(endpoints,
                    std::make_shared<PcmAudioDiagnostics>()), std::make_unique<MfVideoEncoderFactory>(), std::make_unique<MfVideoDecoderFactory>());
            };
            options.capture = [] { return std::make_unique<SyntheticCaptureSource>(320, 180, 30); };
            options.captureForSelection = [](CaptureSelection selection) -> CaptureSession::Factory {
                return [selection]() -> std::unique_ptr<ICaptureSource> {
                    if (selection.display == 63 || selection.window == 1) throw std::runtime_error("Injected capture startup failure");
                    return std::make_unique<SyntheticCaptureSource>(640, 360, selection.fps);
                };
            };
            options.deliver = [](auto& source, const auto& sample) { source.Push(*std::static_pointer_cast<SyntheticCaptureResource>(sample.resource), sample.capturedAt); };
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
    Check(StreamPeerJson(diagnostic, 3)["receiver"].toObject()["decodeFps"].toDouble(-1) == 0);
    diagnostic.receiver.stale = true;
    const auto expiredReceiver = StreamPeerJson(diagnostic, 3)["receiver"].toObject();
    Check(expiredReceiver["sampleState"] == "stale" && expiredReceiver["width"].isNull() && expiredReceiver["framesDecoded"].isNull());
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
    // A directory cannot be a settings file. Failed writes must not change the
    // in-memory defaults later consumed by browser sessions.
    RoomProfile unwritable(files.path());
    Check(!unwritable.savePlayback({12, true})); Check(unwritable.playback().volume == 100);
    auto valid = automatic; valid.width = 1280;
    Check(!unwritable.saveStreamPreferences(valid)); Check(unwritable.streamPreferences().width == 1920);
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
    RoomBrowserWindow host(origin, Factory(audio), true, hostFile, false);
    RoomBrowserWindow viewer(origin, Factory(audio), true, viewerFile, false);
    Directory audit(true); Check(audit.Start(origin)); host.show(); viewer.show();
    Wait([&] { return audit.status().phase == Directory::Phase::Ready && host.directory().status().phase == Directory::Phase::Ready && viewer.directory().status().phase == Directory::Phase::Ready; });
    Check(audit.status().rooms.empty() && host.directory().connectionAttempts() == 1);
    Check(host.findChild<QLineEdit*>("roomNickname")->text() == QStringLiteral("Caf\u00e9"));
    host.findChild<QLineEdit*>("roomName")->setText("<b>Plain room</b>");
    host.findChild<QLineEdit*>("roomPassword")->setText("browser-test-secret");
    host.findChild<QPushButton*>("createV2Room")->click();
    Wait([&] { return host.activeSession() && host.activeSession()->session().status().phase == RoomPhase::Active &&
        !host.directory().running() && viewer.directory().status().rooms.size() == 1 && audit.status().rooms.size() == 1; });
    auto* list = viewer.findChild<QTableWidget*>("publicRooms");
    Check(list->rowCount() == 1 && list->item(0, 0)->text() == "<b>Plain room</b>" && list->item(0, 3)->text() == "Required");
    const auto hostAttempts = host.directory().connectionAttempts();
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
    viewer.findChild<QLineEdit*>("roomNickname")->setText(" Browser viewer ");
    viewer.findChild<QLineEdit*>("roomPassword")->setText("browser-test-secret"); list->selectRow(0);
    viewer.findChild<QLineEdit*>("joinRoomId")->setText(roomLink);
    viewer.findChild<QPushButton*>("joinV2Room")->click(); Check(viewer.activeSession());
    unsigned frames = 0; auto present = viewer.activeSession()->session().frameReady;
    viewer.activeSession()->session().frameReady = [&](auto frame) { ++frames; present(std::move(frame)); };
    Wait([&] { return frames >= 10 && !viewer.directory().running() && audit.status().rooms.size() == 1 && audit.status().rooms[0].viewers == 1; });
    Check(host.directory().connectionAttempts() == hostAttempts); // Hidden browser never reopens.
    Check(viewer.findChild<QLineEdit*>("roomPassword")->text().isEmpty());
    Check(RoomProfile(viewerFile).nickname() == "Browser viewer");
    for (const auto& path : {hostFile, viewerFile}) { QSettings saved(path, QSettings::IniFormat); Check(saved.allKeys() == QStringList{"nickname"}); }
    auto* hostWindow = host.activeSession(); auto* viewerWindow = viewer.activeSession();
    const auto streamRevision = hostWindow->session().status().stream.requestedRevision;
    hostWindow->findChild<QSpinBox*>("streamWidth")->setValue(1280);
    hostWindow->findChild<QSpinBox*>("streamHeight")->setValue(720);
    hostWindow->findChild<QCheckBox*>("uploadBudgetEnabled")->setChecked(true);
    hostWindow->findChild<QSpinBox*>("uploadBudget")->setValue(8000000);
    hostWindow->findChild<QPushButton*>("saveSessionDefaults")->click();
    Check(RoomProfile(hostFile).streamPreferences().width == 1280);
    Check(RoomProfile(hostFile).streamPreferences().aggregateUploadLimitBps == 8000000);
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
        for (const auto& key : saved.allKeys()) Check(key == "nickname" || key == "stream/v1" || key == "playback/v1");
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
    host.close(); viewer.close(); audit.Stop();
    Wait([&] { return !host.directory().running() && !viewer.directory().running() && !audit.running(); });
    // Production facade rejects plaintext without opening a connection.
    Directory secure; Check(!secure.Start(origin)); Check(secure.connectionAttempts() == 0 && secure.status().phase == Directory::Phase::Failed);
}
void SourceSwitchScenario(const std::string& origin) {
    RoomSessionConfig config; config.room.origin = origin; config.room.host = true;
    config.room.nickname = "Source host"; config.room.name = "Source switch";
    config.media.preferences.resolution = ResolutionMode::Native;
    RoomSessionWindow host(config, Factory(std::make_shared<proof::AudioEvidence>()), true); host.show();
    Wait([&] { return host.session().status().phase == RoomPhase::Active; });
    config.room.host = false; config.room.roomId = host.session().status().roomId;
    auto audio = std::make_shared<proof::AudioEvidence>();
    RoomSessionWindow viewer(config, Factory(audio), true); viewer.show();
    unsigned frames = 0, lastWidth = 0; auto present = viewer.session().frameReady;
    viewer.session().frameReady = [&](auto frame) { ++frames; lastWidth = frame.width; present(std::move(frame)); };
    Wait([&] { return frames >= 10 && host.findChild<QPushButton*>("switchCaptureSource")->isEnabled(); });
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
    Check(viewer.session().status().roomId == viewerBefore.roomId && viewer.session().status().peerId == viewerBefore.peerId &&
        viewer.session().status().revision == viewerBefore.revision);
    host.session().playbackUpdated = [&](const auto& result) { error = result.error; };
    host.session().updatePlayback({}); Wait([&] { return !host.session().playbackPending(); }); Check(error == AudioUpdateError::Unsupported);
    host.close(); viewer.close(); Wait([&] { return !host.session().running() && !viewer.session().running(); });
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
int main(int argc, char** argv) {
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString&) {});
    QApplication application(argc, argv); application.setQuitOnLastWindowClosed(false);
    webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
    webrtc::InitializeLogging(std::move(logging)); webrtc::WinsockInitializer winsock;
    if (winsock.error() || !webrtc::InitializeSSL()) return 1;
    int result = 0;
    try {
        Check(argc == 2 || (argc == 3 && std::string(argv[2]) == "mutation-ack-delay"));
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
        screenshare::WindowsMediaRuntime mediaRuntime; Check(SUCCEEDED(mediaRuntime.result()));
        NativePresentationRecovery();
        proof::TestWindow capture; captureWindow = capture.handle();
#endif
        if (argc == 3) MutationAcknowledgementScenario(argv[1]);
        else {
        RoomSessionConfig config; config.room.origin = argv[1]; config.room.host = true;
        config.room.nickname = "UiHost"; config.room.name = "UI media";
        config.media.preferences.resolution = ResolutionMode::Fixed;
        config.media.preferences.width = 320; config.media.preferences.height = 180; config.media.preferences.fps = 30;
        auto hostAudio = std::make_shared<proof::AudioEvidence>();
        RoomSessionWindow host(config, Factory(hostAudio), true); host.show();
        Wait([&] { return host.session().status().phase == RoomPhase::Active; });
        Check(!host.session().start(config));
        config.room.host = false; config.room.roomId = host.session().status().roomId; config.room.nickname = "UiHost";
        auto viewerAudio = std::make_shared<proof::AudioEvidence>();
        RoomSessionWindow viewer(config, Factory(viewerAudio), true); viewer.show();
        unsigned original = 0, changed = 0;
        auto present = viewer.session().frameReady;
        viewer.session().frameReady = [&](auto frame) {
            Check(frame.pixels().size() == frame.width * frame.height * 3 / 2 && frame.retainedPixels && frame.nv12.empty());
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
        Check(host.findChild<QLabel*>("peerDiagnosticsDetails")->text().contains("Remote display, latency and congestion reason: unknown"));
        const auto diagnosticPeer = diagnostics->item(0, 0)->data(Qt::UserRole);
        host.findChild<QPushButton*>("openPeerDetails")->click();
        auto* popup = host.findChild<QDialog*>("peerDetailsDialog");
        auto* popupText = popup->findChild<QPlainTextEdit*>("peerDetailsText");
        Check(popup->isVisible() && !popup->isModal() && popup->property("peerId") == diagnosticPeer);
        Check(popupText->toPlainText().contains("Selected-path RTT") && popupText->toPlainText().contains("not image/input latency"));
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
        MutationLifecycle(argv[1]);
        SourceSwitchScenario(argv[1]);
        std::cout << "{\"passed\":true,\"qt_ui\":true,\"browser\":true,\"directory_push\":true,\"nickname_persistence\":true,\"coalesced_settings\":true,\"responsive_stop\":true,\"restart_owner\":true,\"original_frames\":" << original << ",\"changed_frames\":" << changed << "}\n";
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    webrtc::CleanupSSL(); return result;
}
