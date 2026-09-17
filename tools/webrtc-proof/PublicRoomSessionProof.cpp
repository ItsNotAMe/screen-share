#include "api/RoomSession.h"
#include "media/webrtc/NativeRoomRuntime.h"
#ifdef SCREENSHARE_WINDOWS_ROOM_PROOF
#include "media/webrtc/WindowsRoomRuntime.h"
#include "CaptureTestWindow.h"
#include "core/WindowsMediaRuntime.h"
#endif
#include "media/HostMediaSession.h"
#include "media/webrtc/MediaEngine.h"
#include "media/webrtc/MediaPeer.h"
#include "media/webrtc/RoomPeerNegotiation.h"
#include "media/webrtc/CaptureVideoSource.h"
#include "media/webrtc/MfVideoEncoderFactory.h"
#include "media/webrtc/MfVideoDecoderFactory.h"
#include "media/webrtc/PcmAudioDeviceModule.h"
#include "SyntheticAudio.h"
#include "api/make_ref_counted.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/win32_socket_init.h"
#include "rtc_base/logging.h"
#include <QCoreApplication>
#include <array>
#include <map>
#include <iostream>
#include <source_location>
using namespace screenshare::media;
using namespace screenshare::v2;
using namespace std::chrono_literals;
void Check(bool ok, std::source_location location = std::source_location::current()) {
    if (!ok) throw std::runtime_error("Public session proof failed at line " + std::to_string(location.line()));
}
#ifdef SCREENSHARE_WINDOWS_ROOM_PROOF
HWND captureWindow = nullptr;
#endif
struct Evidence : webrtc::VideoSinkInterface<webrtc::VideoFrame> {
    std::atomic<unsigned> frames{0}, invalid{0}, destroyed{0};
    std::atomic<bool> failDelivery{false};
    std::atomic<bool> restart{false};
    std::atomic<bool> pauseAdvance{false};
    std::atomic<unsigned> offers{0};
    std::atomic<unsigned> smallFrames{0};
    std::shared_ptr<proof::AudioEvidence> audio = std::make_shared<proof::AudioEvidence>();
    void OnFrame(const webrtc::VideoFrame& frame) override {
        auto pixels = frame.video_frame_buffer()->ToI420();
        const bool reducedFrame = frame.width() == 320 && frame.height() == 180;
        if ((!reducedFrame && (frame.width() != 640 || frame.height() != 360)) || !pixels ||
            pixels->DataY()[pixels->StrideY() * (pixels->height() / 2) + pixels->width() / 2] < 35) ++invalid;
        ++frames;
        if (reducedFrame) ++smallFrames;
    }
};
// Only synthetic dependencies and evidence remain diagnostic-owned.
class Runtime final : public RoomRuntime {
    std::shared_ptr<Evidence> evidence_;
    std::unique_ptr<RoomRuntime> native_;
    RoomSend send_;
    std::string remote_, connection_;
public:
    Runtime(RoomIdentity identity, RoomSend send, std::shared_ptr<Evidence> evidence)
        : evidence_(std::move(evidence)), send_(send) {
#ifdef SCREENSHARE_WINDOWS_ROOM_PROOF
        WindowsRoomRuntimeOptions windows;
        windows.capture.sourceType = screenshare::CaptureSourceType::Window;
        windows.capture.windowHandle = reinterpret_cast<uint64_t>(captureWindow);
        windows.capture.targetWidth = 640; windows.capture.targetHeight = 360; windows.capture.targetFps = 30;
        windows.preferences.resolution = ResolutionMode::Fixed;
        windows.preferences.width = 640; windows.preferences.height = 360; windows.preferences.fps = 30;
        windows.audioEndpoints = proof::SyntheticAudio(evidence_->audio);
        windows.audioForSelection = proof::SyntheticAudioSelection;
        windows.frames = evidence_;
        native_ = WindowsRoomRuntimeFactory(std::move(windows))(identity, std::move(send));
#else
        NativeRoomRuntimeOptions options;
        auto endpoints = proof::SyntheticAudio(evidence_->audio);
        if (identity.host) {
            options.audioSwitch = std::make_shared<AudioSwitchControl>(screenshare::media::AudioSelection{}, endpoints.capture);
            endpoints.capture = [control = options.audioSwitch] { return std::make_unique<SwitchablePcmCapture>(control); };
            options.audioForSelection = proof::SyntheticAudioSelection;
        }
        options.engine = [endpoints] {
            return std::make_unique<MediaEngine>(CreatePcmAudioDeviceModule(endpoints,
                std::make_shared<PcmAudioDiagnostics>()), std::make_unique<MfVideoEncoderFactory>(), std::make_unique<MfVideoDecoderFactory>());
        };
        options.capture = [] { return std::make_unique<SyntheticCaptureSource>(640, 360, 30); };
        options.deliver = [evidence = evidence_](auto& source, const auto& sample) {
            if (evidence->failDelivery.exchange(false)) throw std::runtime_error("Injected viewer delivery failure");
            source.Push(*std::static_pointer_cast<SyntheticCaptureResource>(sample.resource), sample.capturedAt);
        };
        options.frames = evidence_;
        options.preferences.resolution = ResolutionMode::Fixed;
        options.preferences.width = 640; options.preferences.height = 360; options.preferences.fps = 30;
        native_ = CreateNativeRoomRuntime(std::move(identity), std::move(send), std::move(options));
#endif
    }
    ~Runtime() override { native_.reset(); ++evidence_->destroyed; }
    bool Ready(const std::string& id) override { return native_->Ready(id); }
    bool Add(const std::string& id) override { remote_ = id; return native_->Add(id); }
    void Remove(const std::string& id) noexcept override { native_->Remove(id); }
    bool Receive(const std::string& id, RoomPeerSignal signal) override {
        if (signal.kind == RoomPeerSignal::Kind::Offer) { connection_ = signal.connectionId; ++evidence_->offers; }
        return native_->Receive(id, std::move(signal));
    }
    std::vector<std::string> FailedPeers() const override { return native_->FailedPeers(); }
    StreamUpdateResult UpdateStreamPreferences(const StreamPreferences& preferences) override { return native_->UpdateStreamPreferences(preferences); }
    StreamStatus StreamSettings() const override { return native_->StreamSettings(); }
    std::future<AudioUpdateResult> SwitchAudioSource(screenshare::media::AudioSelection selection) override { return native_->SwitchAudioSource(std::move(selection)); }
    AudioSelectionStatus AudioSelection() const override { return native_->AudioSelection(); }
    void Advance() override {
        try {
            if (evidence_->pauseAdvance) return; // Media threads keep running; telemetry cannot publish.
            native_->Advance();
            if (evidence_->restart.exchange(false))
                Check(send_(remote_, {RoomPeerSignal::Kind::RestartRequest, connection_, {}, {}}));
        }
        catch (const std::exception& error) { std::cerr << "Native runtime: " << error.what() << '\n'; throw; }
    }
    std::shared_future<void> BeginStop() override { return native_->BeginStop(); }
};
template<class Predicate> void Wait(Predicate condition) {
    auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!condition()) { Check(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(5ms); }
}
template<class Future> auto Get(Future& future) { Check(future.wait_for(20s) == std::future_status::ready); return future.get(); }
class HeldRuntime final : public RoomRuntime {
    std::shared_future<void> barrier_;
    std::shared_ptr<Evidence> evidence_;
public:
    HeldRuntime(std::shared_future<void> barrier, std::shared_ptr<Evidence> evidence)
        : barrier_(std::move(barrier)), evidence_(std::move(evidence)) {}
    ~HeldRuntime() override { ++evidence_->destroyed; }
    void Advance() override {}
    bool Ready(const std::string&) override { return true; }
    bool Add(const std::string&) override { return true; }
    void Remove(const std::string&) noexcept override {}
    bool Receive(const std::string&, RoomPeerSignal) override { return false; }
    std::shared_future<void> BeginStop() override { return barrier_; }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
    webrtc::InitializeLogging(std::move(logging)); webrtc::WinsockInitializer winsock;
    if (winsock.error() || !webrtc::InitializeSSL()) return 1;
    int result = 0;
    try {
        Check(argc == 2);
#ifdef SCREENSHARE_WINDOWS_ROOM_PROOF
        screenshare::WindowsMediaRuntime windowsRuntime;
        Check(SUCCEEDED(windowsRuntime.result()));
        proof::TestWindow window;
        captureWindow = window.handle();
#endif
        auto hostEvidence = std::make_shared<Evidence>();
        auto factory = [](auto evidence) { return [evidence](auto identity, auto send) { return std::make_unique<Runtime>(identity, std::move(send), evidence); }; };
        RoomOptions hostOptions; hostOptions.origin = argv[1]; hostOptions.host = true; hostOptions.nickname = "FacadeHost"; hostOptions.name = "Facade media";
        RoomSession host(factory(hostEvidence), true);
        auto start = host.Start(hostOptions); auto duplicate = host.Start(hostOptions);
        Check(Get(duplicate).error == RoomError::Busy); Check(Get(start).error == RoomError::None);
        std::array<std::unique_ptr<RoomSession>, 4> viewers;
        std::array<std::shared_ptr<Evidence>, 4> evidence;
        auto options = hostOptions; options.host = false; options.roomId = host.Status().roomId;
        for (size_t i = 0; i < viewers.size(); ++i) {
            evidence[i] = std::make_shared<Evidence>(); options.nickname = "FacadeViewer" + std::to_string(i);
            viewers[i] = std::make_unique<RoomSession>(factory(evidence[i]), true);
            auto joining = viewers[i]->Start(options); const auto joined = Get(joining);
            if (joined.error != RoomError::None) std::cerr << "Join error " << int(joined.error) << "; host error " << int(host.Status().error) << '\n';
            Check(joined.error == RoomError::None);
        }
        Wait([&] { for (auto& value : evidence) if (value->frames < 45 || value->audio->audibleBlocks < 20) return false; return host.Status().activePeers == 4; });
        const auto beforeAudioSwitch = host.Status();
        auto audioSwitch = host.SwitchAudioSource({AudioKind::None});
        Check(Get(audioSwitch).error == AudioUpdateError::None);
        // Observe real decoded silence instead of assuming a fixed jitter/codec
        // drain time. This is a correctness deadline, not a gaming latency gate.
        const auto quietDeadline = std::chrono::steady_clock::now() + 3s;
        while (!std::all_of(evidence.begin(), evidence.end(), [](const auto& value) { return value->audio->quietStreak >= 30; })) {
            Check(std::chrono::steady_clock::now() < quietDeadline); std::this_thread::sleep_for(5ms);
        }
        std::array<uint64_t, 4> quietBlocks;
        std::array<unsigned, 4> playingFrames;
        for (size_t i = 0; i < 4; ++i) { quietBlocks[i] = evidence[i]->audio->audibleBlocks; playingFrames[i] = evidence[i]->frames; }
        std::this_thread::sleep_for(300ms);
        for (size_t i = 0; i < 4; ++i) {
            if (evidence[i]->audio->audibleBlocks != quietBlocks[i] || evidence[i]->frames <= playingFrames[i])
                std::cerr << "Audio switch viewer " << i << " nonzero blocks " << quietBlocks[i] << '/' << evidence[i]->audio->audibleBlocks
                    << " frames " << playingFrames[i] << '/' << evidence[i]->frames << " peak " << evidence[i]->audio->lastPeak << '\n';
            Check(evidence[i]->audio->audibleBlocks == quietBlocks[i] && evidence[i]->frames > playingFrames[i]);
        }
        audioSwitch = host.SwitchAudioSource({}); Check(Get(audioSwitch).error == AudioUpdateError::None);
        Wait([&] { for (size_t i = 0; i < 4; ++i) if (evidence[i]->audio->audibleBlocks < quietBlocks[i] + 10) return false; return true; });
        Check(host.Status().audio.revision == beforeAudioSwitch.audio.revision + 2 && host.Status().activePeers == 4 &&
            host.Status().revision == beforeAudioSwitch.revision && host.Status().stream.requestedRevision == beforeAudioSwitch.stream.requestedRevision);
        Wait([&] { const auto peers = host.Status().stream.peers;
            return peers.size() == 4 && std::all_of(peers.begin(), peers.end(), [](const auto& peer) {
                return peer.transportSendBps.value_or(0) > 0 && peer.receiver.observation && peer.receiver.observation->framesDecoded > 0 && !peer.receiver.stale;
            });
        });
        StreamPreferences live;
        const auto pausedPeer = viewers[0]->Status().peerId;
        const auto framesBeforeTelemetryPause = evidence[0]->frames.load();
        evidence[0]->pauseAdvance = true;
        try {
            Wait([&] { const auto peers = host.Status().stream.peers;
                return std::any_of(peers.begin(), peers.end(), [&](const auto& peer) {
                    return peer.peerId == pausedPeer && peer.receiver.stale && !peer.receiver.observation;
                });
            });
            Check(evidence[0]->frames > framesBeforeTelemetryPause);
        } catch (...) { evidence[0]->pauseAdvance = false; throw; }
        evidence[0]->pauseAdvance = false;
        Wait([&] { const auto peers = host.Status().stream.peers;
            return peers.size() == 4 && std::all_of(peers.begin(), peers.end(), [](const auto& peer) {
                return peer.receiver.observation && !peer.receiver.stale;
            });
        });
        live.resolution = ResolutionMode::Fixed; live.width = 320; live.height = 180;
        live.fps = 20; live.bitrateMode = SettingMode::Manual; live.bitrateLimitBps = 1000000;
        live.aggregateUploadLimitBps = 4000000;
        auto invalidPreferences = live; invalidPreferences.width = 319;
        const auto originalRevision = host.Status().stream.requestedRevision;
        auto invalidUpdate = host.UpdateStreamPreferences(invalidPreferences);
        Check(Get(invalidUpdate).error == StreamUpdateError::Invalid);
        Check(host.Status().stream.requestedRevision == originalRevision);
        auto viewerUpdate = viewers[0]->UpdateStreamPreferences(live);
        Check(Get(viewerUpdate).error == StreamUpdateError::Unsupported);
        auto update = host.UpdateStreamPreferences(live); const auto accepted = Get(update);
        Check(accepted.error == StreamUpdateError::None && accepted.revision > originalRevision);
        Wait([&] {
            const auto status = host.Status().stream;
            if (status.peers.size() != 4) return false;
            for (const auto& peer : status.peers)
                if (peer.rejected || peer.appliedRevision != accepted.revision || peer.observedRevision != accepted.revision || peer.width != 320 || peer.height != 180 ||
                    peer.allocatedVideoBitrateBps != 672000 || peer.appliedVideoBitrateBps != 672000) return false;
            for (const auto& value : evidence) if (value->smallFrames < 10) return false;
            return true;
        });
        const unsigned restartingBefore = evidence[0]->frames, healthyBefore = evidence[1]->frames;
        evidence[0]->restart = true;
        Wait([&] { return evidence[0]->offers >= 2 && evidence[0]->frames >= restartingBefore + 30 && evidence[1]->frames >= healthyBefore + 30; });
        Wait([&] { const auto peers = host.Status().stream.peers;
            return peers.size() == 4 && std::all_of(peers.begin(), peers.end(), [](const auto& peer) {
                return peer.receiver.observation && peer.receiver.observation->width == 320 && !peer.receiver.stale;
            });
        });
        auto stopViewer = viewers[3]->Stop(); Get(stopViewer);
        Check(evidence[3]->destroyed == 1);
        Wait([&] { return host.Status().activePeers == 3; });
        Wait([&] { const auto status = host.Status().stream;
            return status.peers.size() == 3 && std::all_of(status.peers.begin(), status.peers.end(), [&](const auto& peer) {
                return peer.appliedRevision == status.requestedRevision && peer.appliedVideoBitrateBps == 938666;
            });
        });
        auto retiredEvidence = evidence[3];
        evidence[3] = std::make_shared<Evidence>();
        viewers[3] = std::make_unique<RoomSession>(factory(evidence[3]), true);
        auto rejoining = viewers[3]->Start(options); Check(Get(rejoining).error == RoomError::None);
        Wait([&] { return host.Status().activePeers == 4 && evidence[3]->smallFrames >= 30 && evidence[3]->audio->audibleBlocks >= 20; });
        Wait([&] { const auto status = host.Status().stream;
            return status.peers.size() == 4 && std::all_of(status.peers.begin(), status.peers.end(), [&](const auto& peer) {
                return peer.appliedRevision == status.requestedRevision && peer.appliedVideoBitrateBps == 672000 &&
                    peer.receiver.observation && peer.receiver.observation->width == 320 && !peer.receiver.stale;
            });
        });
        live.width = 640; live.height = 360; live.fps = 30;
        live.bitrateMode = SettingMode::Auto; live.bitrateLimitBps.reset();
        auto restore = host.UpdateStreamPreferences(live); const auto restored = Get(restore);
        Check(restored.error == StreamUpdateError::None && restored.revision > accepted.revision);
        Wait([&] {
            const auto status = host.Status().stream;
            if (status.peers.size() != 4) return false;
            for (const auto& peer : status.peers)
                if (peer.rejected || peer.appliedRevision != restored.revision || peer.observedRevision != restored.revision || peer.width != 640) return false;
            return true;
        });
        auto leaveAgain = viewers[3]->Stop(); Get(leaveAgain);
        Wait([&] { return host.Status().activePeers == 3; });
        Check(retiredEvidence->destroyed == 1);
#ifndef SCREENSHARE_WINDOWS_ROOM_PROOF
        hostEvidence->failDelivery = true;
        Wait([&] { return host.Status().activePeers == 2 && host.Status().failedPeers == 1; });
        std::array<unsigned, 3> before{};
        for (size_t i = 0; i < before.size(); ++i) before[i] = evidence[i]->frames;
        Wait([&] {
            unsigned progressing = 0;
            for (size_t i = 0; i < before.size(); ++i) if (evidence[i]->frames >= before[i] + 20) ++progressing;
            return progressing == 2;
        });
#endif
        auto stopping = host.Stop(); auto sameStop = host.Stop(); Get(stopping); Get(sameStop);
        Check(hostEvidence->destroyed == 1 && host.Status().phase == RoomPhase::Stopped);
        auto stoppedUpdate = host.UpdateStreamPreferences(live);
        Check(Get(stoppedUpdate).error == StreamUpdateError::Unavailable);
        for (auto& viewer : viewers) { auto done = viewer->Stop(); Get(done); }
        unsigned frames = 0;
        for (auto& value : evidence) { Check(value->invalid == 0 && value->destroyed == 1); frames += value->frames; }
        // Cancellation is ordered even when the admission command has not run.
        RoomSession cancelled(factory(std::make_shared<Evidence>()), true);
        auto pending = cancelled.Start(hostOptions); auto cancelledStop = cancelled.Stop(); Get(cancelledStop);
        Check(Get(pending).error == RoomError::Cancelled);
        std::promise<void> release; auto barrier = release.get_future().share();
        auto heldEvidence = std::make_shared<Evidence>();
        RoomSession held([&](auto, auto) { return std::make_unique<HeldRuntime>(barrier, heldEvidence); }, true);
        hostOptions.publicRoom = false;
        std::shared_future<void> heldStop;
        try {
            auto heldStart = held.Start(hostOptions); Check(Get(heldStart).error == RoomError::None);
            heldStop = held.Stop();
            Wait([&] { return held.Status().phase == RoomPhase::Stopping; });
            Check(heldStop.wait_for(100ms) == std::future_status::timeout && heldEvidence->destroyed == 0);
        } catch (...) { release.set_value(); throw; }
        release.set_value(); Get(heldStop); Check(heldEvidence->destroyed == 1);
        // Production transport rejects the diagnostic plaintext origin.
        RoomSession secure(factory(std::make_shared<Evidence>()));
        auto rejected = secure.Start(hostOptions); Check(Get(rejected).error == RoomError::Admission);
        auto secureStop = secure.Stop(); Get(secureStop);
        RoomSession failedCapture([](auto identity, auto send) {
            NativeRoomRuntimeOptions options;
            options.engine = []() -> std::unique_ptr<MediaEngine> { throw std::runtime_error("Engine must not start after failed capture"); };
            options.capture = []() -> std::unique_ptr<ICaptureSource> { throw std::runtime_error("Injected capture startup failure"); };
            options.deliver = [](auto&, const auto&) {};
            return CreateNativeRoomRuntime(identity, std::move(send), std::move(options));
        }, true);
        auto failedStart = failedCapture.Start(hostOptions); Get(failedStart);
        Wait([&] { return failedCapture.Status().phase == RoomPhase::Failed; });
        Check(failedCapture.Status().error == RoomError::Media);
        auto failedStop = failedCapture.Stop(); Get(failedStop);
        std::cout << "{\"passed\":true,\"public_session\":true,\"native_runtime\":true,\"rejoin\":true,\"viewers\":4,\"decoded_frames\":" << frames << ",\"cancel_admission\":true,\"coalesced_stop\":true,\"media_drain_barrier\":true,\"production_tls_required\":true}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    webrtc::CleanupSSL(); return result;
}
