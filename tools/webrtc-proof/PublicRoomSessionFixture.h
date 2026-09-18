#pragma once
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
#include "ObservedCaptureSource.h"
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
    class InputSink final : public screenshare::input::Sink {
    public:
        std::atomic<unsigned> applied{0}, releases{0};
        std::atomic<bool> pressed{false};
        bool Grant(const std::string&, uint8_t, int) override { return true; }
        bool Apply(const std::string&, const screenshare::input::Event& e) override {
            if (e.kind == screenshare::input::Kind::Key) pressed = e.down;
            ++applied; return true;
        }
        void Release(const std::string&) noexcept override { pressed = false; ++releases; }
    };
    std::shared_ptr<InputSink> input = std::make_shared<InputSink>();
    std::atomic<unsigned> responseFrames{0};
    std::atomic<unsigned> frames{0}, invalid{0}, destroyed{0};
    std::atomic<bool> failDelivery{false};
    std::atomic<bool> restart{false};
    std::atomic<bool> pauseAdvance{false};
    std::atomic<unsigned> offers{0};
    std::atomic<unsigned> smallFrames{0};
    std::shared_ptr<proof::CaptureLifetime> capture = std::make_shared<proof::CaptureLifetime>();
    std::shared_ptr<proof::AudioEvidence> audio = std::make_shared<proof::AudioEvidence>();
    MediaEngine::PacketFactory packetFactory;
    CaptureSession::Factory captureFactory;
    std::optional<StreamPreferences> preferences;
    bool fastAudioExperiment = false; // Proof-only; production defaults are untouched.
    bool localizedInputResponse = false;
    void OnFrame(const webrtc::VideoFrame& frame) override {
        auto pixels = frame.video_frame_buffer()->ToI420();
        if (pixels && pixels->DataY()[pixels->StrideY() * (pixels->height() / 2) + pixels->width() / 2] > 185) ++responseFrames;
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
    Runtime(RoomIdentity identity, RoomSend send, std::shared_ptr<Evidence> evidence,
        std::shared_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> frames = {})
        : evidence_(std::move(evidence)), send_(send) {
#ifdef SCREENSHARE_WINDOWS_ROOM_PROOF
        WindowsRoomRuntimeOptions windows;
        windows.capture.sourceType = screenshare::CaptureSourceType::Window;
        windows.capture.windowHandle = reinterpret_cast<uint64_t>(captureWindow);
        windows.capture.targetWidth = 640; windows.capture.targetHeight = 360; windows.capture.targetFps = 30;
        windows.preferences.resolution = ResolutionMode::Fixed;
        windows.preferences.width = 640; windows.preferences.height = 360; windows.preferences.fps = 30;
        windows.audioEndpoints = proof::SyntheticAudio(evidence_->audio);
        windows.audioForSelection = [audio = evidence_->audio](auto selection) { return proof::SyntheticAudioSelectionWithEvidence(selection, audio); };
        windows.playbackForSelection = [audio = evidence_->audio](auto selection) { return proof::SyntheticPlayback(selection, audio); };
        windows.frames = frames ? std::move(frames) : evidence_;
        windows.inputSink = evidence_->input;
        native_ = WindowsRoomRuntimeFactory(std::move(windows))(identity, std::move(send));
#else
        NativeRoomRuntimeOptions options;
        options.inputSink = evidence_->input;
        auto endpoints = proof::SyntheticAudio(evidence_->audio);
        if (identity.host) {
            options.audioSwitch = std::make_shared<AudioSwitchControl>(screenshare::media::AudioSelection{}, endpoints.capture, ProcessMicrophone);
            endpoints.capture = [control = options.audioSwitch] { return std::make_unique<SwitchablePcmCapture>(control); };
            options.audioForSelection = [audio = evidence_->audio](auto selection) { return proof::SyntheticAudioSelectionWithEvidence(selection, audio); };
        } else {
            options.playback = std::make_shared<PlaybackControl>(PlaybackSelection{}, endpoints.playout);
            endpoints.playout = [control = options.playback] { return std::make_unique<ControlledPcmPlayout>(control); };
            options.playbackForSelection = [audio = evidence_->audio](auto selection) { return proof::SyntheticPlayback(selection, audio); };
        }
        options.engine = [endpoints, packetFactory = evidence_->packetFactory] {
            return std::make_unique<MediaEngine>(CreatePcmAudioDeviceModule(endpoints,
                std::make_shared<PcmAudioDiagnostics>()), std::make_unique<MfVideoEncoderFactory>(), std::make_unique<MfVideoDecoderFactory>(), packetFactory);
        };
        options.capture = [lifetime = evidence_->capture] { return std::make_unique<proof::ObservedCaptureSource>(lifetime); };
        if (evidence_->captureFactory) options.capture = evidence_->captureFactory;
        options.deliver = [evidence = evidence_](auto& source, const auto& sample) {
            if (evidence->failDelivery.exchange(false)) throw std::runtime_error("Injected viewer delivery failure");
            if (evidence->input->pressed) {
                auto response = *std::static_pointer_cast<SyntheticCaptureResource>(sample.resource);
                if (evidence->localizedInputResponse) {
                    const int left = std::max(0, response.width / 2 - 16), right = std::min(response.width, response.width / 2 + 16);
                    const int top = std::max(0, response.height / 2 - 16), bottom = std::min(response.height, response.height / 2 + 16);
                    for (int y = top; y < bottom; ++y)
                        std::fill(response.luma.begin() + y * response.width + left,
                            response.luma.begin() + y * response.width + right, uint8_t(210));
                } else std::fill(response.luma.begin(), response.luma.end(), uint8_t(210));
                source.Push(response, sample.capturedAt); return;
            }
            source.Push(*std::static_pointer_cast<SyntheticCaptureResource>(sample.resource), sample.capturedAt);
        };
        options.frames = frames ? std::move(frames) : evidence_;
        options.preferences.resolution = ResolutionMode::Fixed;
        options.preferences.width = 640; options.preferences.height = 360; options.preferences.fps = 30;
        if (evidence_->preferences) options.preferences = *evidence_->preferences;
        options.connection.audio_jitter_buffer_fast_accelerate = evidence_->fastAudioExperiment;
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
    std::future<AudioUpdateResult> UpdatePlayback(screenshare::media::PlaybackSelection selection) override { return native_->UpdatePlayback(std::move(selection)); }
    PlaybackStatus Playback() const override { return native_->Playback(); }
    std::shared_ptr<screenshare::input::Port> Input() const override { return native_->Input(); }
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
