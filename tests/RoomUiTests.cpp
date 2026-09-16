#include "ui/RoomSessionWindow.h"
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
#include <iostream>
#include <source_location>
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
#include "../tools/webrtc-proof/CaptureTestWindow.h"
#include "core/WindowsMediaRuntime.h"
HWND captureWindow = nullptr;
#endif
using namespace screenshare::media;
using namespace screenshare::v2;
using namespace std::chrono_literals;
void Check(bool value, std::source_location where = std::source_location::current()) {
    if (!value) throw std::runtime_error("Room UI proof failed at " + std::to_string(where.line()));
}
template<class F> void Wait(F condition) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!condition()) {
        Check(std::chrono::steady_clock::now() < deadline);
        QCoreApplication::processEvents(); std::this_thread::sleep_for(1ms);
    }
}
QtRoomSession::Factory Factory(std::shared_ptr<proof::AudioEvidence> audio) {
    return [audio](WindowsRoomRuntimeOptions windows) -> RoomRuntimeFactory {
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
        windows.capture.sourceType = screenshare::CaptureSourceType::Window;
        windows.capture.windowHandle = reinterpret_cast<uint64_t>(captureWindow);
        windows.audioEndpoints = proof::SyntheticAudio(audio);
        return WindowsRoomRuntimeFactory(std::move(windows));
#else
        return [windows, audio](auto identity, auto send) {
            NativeRoomRuntimeOptions options; options.preferences = windows.preferences; options.frames = windows.frames;
            options.engine = [audio] {
                return std::make_unique<MediaEngine>(CreatePcmAudioDeviceModule(proof::SyntheticAudio(audio),
                    std::make_shared<PcmAudioDiagnostics>()), std::make_unique<MfVideoEncoderFactory>(), std::make_unique<MfVideoDecoderFactory>());
            };
            options.capture = [] { return std::make_unique<SyntheticCaptureSource>(320, 180, 30); };
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
void BrowserScenario(const QUrl& origin) {
    using Directory = screenshare::room::qt::RoomDirectory;
    QTemporaryDir profiles; Check(profiles.isValid());
    const auto hostFile = profiles.filePath("host.ini"), viewerFile = profiles.filePath("viewer.ini");
    {
        QSettings raw(hostFile, QSettings::IniFormat); raw.setValue("nickname", QString("Bad") + QChar(0x202e)); raw.sync();
    }
    RoomProfile profile(hostFile);
    Check(profile.nickname() == "Guest");
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
    viewer.findChild<QLineEdit*>("roomPassword")->setText("wrong-password"); list->selectRow(0);
    viewer.findChild<QPushButton*>("joinSelectedRoom")->click();
    Wait([&] { return viewer.activeSession() && viewer.activeSession()->session().status().phase == RoomPhase::Failed; });
    viewer.activeSession()->close();
    Wait([&] { return !viewer.activeSession() && viewer.isVisible() && viewer.directory().status().phase == Directory::Phase::Ready; });
    viewer.findChild<QLineEdit*>("roomNickname")->setText(" Browser viewer ");
    viewer.findChild<QLineEdit*>("roomPassword")->setText("browser-test-secret"); list->selectRow(0);
    viewer.findChild<QPushButton*>("joinSelectedRoom")->click(); Check(viewer.activeSession());
    unsigned frames = 0; auto present = viewer.activeSession()->session().frameReady;
    viewer.activeSession()->session().frameReady = [&](auto frame) { ++frames; present(std::move(frame)); };
    Wait([&] { return frames >= 10 && !viewer.directory().running() && audit.status().rooms.size() == 1 && audit.status().rooms[0].viewers == 1; });
    Check(host.directory().connectionAttempts() == hostAttempts); // Hidden browser never reopens.
    Check(viewer.findChild<QLineEdit*>("roomPassword")->text().isEmpty());
    Check(RoomProfile(viewerFile).nickname() == "Browser viewer");
    for (const auto& path : {hostFile, viewerFile}) { QSettings saved(path, QSettings::IniFormat); Check(saved.allKeys() == QStringList{"nickname"}); }
    host.activeSession()->close();
    Wait([&] { return !host.activeSession() && host.isVisible() && audit.status().rooms.empty() &&
        viewer.activeSession()->session().status().phase == RoomPhase::Stopped; });
    viewer.activeSession()->close();
    Wait([&] { return !viewer.activeSession() && viewer.directory().status().phase == Directory::Phase::Ready; });
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
int main(int argc, char** argv) {
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString&) {});
    QApplication application(argc, argv); application.setQuitOnLastWindowClosed(false);
    webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
    webrtc::InitializeLogging(std::move(logging)); webrtc::WinsockInitializer winsock;
    if (winsock.error() || !webrtc::InitializeSSL()) return 1;
    int result = 0;
    try {
        Check(argc == 2);
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
        screenshare::WindowsMediaRuntime mediaRuntime; Check(SUCCEEDED(mediaRuntime.result()));
        proof::TestWindow capture; captureWindow = capture.handle();
#endif
        RoomSessionConfig config; config.room.origin = argv[1]; config.room.host = true;
        config.room.nickname = "UiHost"; config.room.name = "UI media";
        config.media.preferences.resolution = ResolutionMode::Fixed;
        config.media.preferences.width = 320; config.media.preferences.height = 180; config.media.preferences.fps = 30;
        auto hostAudio = std::make_shared<proof::AudioEvidence>();
        RoomSessionWindow host(config, Factory(hostAudio), true); host.show();
        Wait([&] { return host.session().status().phase == RoomPhase::Active; });
        Check(!host.session().start(config));
        config.room.host = false; config.room.roomId = host.session().status().roomId; config.room.nickname = "UiViewer";
        auto viewerAudio = std::make_shared<proof::AudioEvidence>();
        RoomSessionWindow viewer(config, Factory(viewerAudio), true); viewer.show();
        unsigned original = 0, changed = 0;
        auto present = viewer.session().frameReady;
        viewer.session().frameReady = [&](auto frame) {
            Check(frame.bytes == frame.width * frame.height * 3 / 2);
            if (frame.width == 320) ++original; else if (frame.width == 160) ++changed; else Check(false);
            present(std::move(frame));
        };
        Wait([&] { return original >= 20 && viewerAudio->audibleBlocks >= 20; });
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
#ifdef SCREENSHARE_WINDOWS_UI_PROOF
        auto* video = static_cast<VideoFrameWidget*>(viewer.findChild<QWidget*>("roomVideo"));
        Check(video && video->presentedFrameCount() >= 20);
#endif
        viewer.close(); Check(viewer.session().running()); // Close waits for drain.
        Wait([&] { return !viewer.session().running() && !viewer.isVisible(); });
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
        std::cout << "{\"passed\":true,\"qt_ui\":true,\"browser\":true,\"directory_push\":true,\"nickname_persistence\":true,\"coalesced_settings\":true,\"responsive_stop\":true,\"restart_owner\":true,\"original_frames\":" << original << ",\"changed_frames\":" << changed << "}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    webrtc::CleanupSSL(); return result;
}
