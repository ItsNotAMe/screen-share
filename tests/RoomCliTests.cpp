#include "cli/RoomCli.h"
#include "shared/LatestRoomVideoFrame.h"
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
#include "api/make_ref_counted.h"
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
#include "../tools/webrtc-proof/CaptureTestWindow.h"
#include "core/WindowsMediaRuntime.h"
#include "render/ReceiverPreviewWindow.h"
HWND captureWindow = nullptr;
#endif

using namespace screenshare::media;
using namespace screenshare::v2;
using namespace std::chrono_literals;
void Check(bool value) { if (!value) throw std::runtime_error("Room CLI integration failed"); }
template<class F> void Reject(F fn) {
    bool rejected = false;
    try { fn(); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected);
}
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
    options.audioForSelection = proof::SyntheticAudioSelection;
    options.playbackForSelection = [audio](auto selection) { return proof::SyntheticPlayback(selection, audio); };
    return WindowsRoomRuntimeFactory(std::move(options));
#else
    return [preferences = config.media.preferences, audio, frames](auto identity, auto send) {
        NativeRoomRuntimeOptions options;
        options.preferences = preferences; options.frames = frames;
        auto endpoints = proof::SyntheticAudio(audio);
        if (!identity.host) {
            options.playback = std::make_shared<PlaybackControl>(PlaybackSelection{}, endpoints.playout);
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
            return [selection] { return std::make_unique<SyntheticCaptureSource>(640, 360, selection.fps); };
        };
        options.deliver = [](auto& source, const auto& sample) {
            source.Push(*std::static_pointer_cast<SyntheticCaptureResource>(sample.resource), sample.capturedAt);
        };
        return CreateNativeRoomRuntime(identity, std::move(send), std::move(options));
    };
#endif
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
        PresentationOwnership();
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
        screenshare::WindowsMediaRuntime mediaRuntime;
        Check(SUCCEEDED(mediaRuntime.result()));
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
        object["audioChanges"] = QJsonArray{QJsonObject{{"atMs", 2500}, {"source", "system"}}};
        auto invalidAudio = object; invalidAudio["audioChanges"] = QJsonArray{QJsonObject{{"atMs", 100}, {"source", "process"}}};
        Reject([&] { ParseRoomSessionConfig(invalidAudio, true); });
        invalidAudio["audioChanges"] = QJsonArray{QJsonObject{{"atMs", 100}, {"source", "system"}, {"processId", 1}}};
        Reject([&] { ParseRoomSessionConfig(invalidAudio, true); });
        Reject([&] { ParseRoomSessionConfig(object); }); // Production cannot opt into plaintext.
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
        const auto host = ParseRoomSessionConfig(object, true);
        std::mutex mutex; std::string roomId;
        std::atomic<bool> stopHost{false}, applied{false}, stopped{false}, accepted{false}, budgetReported{false}, rateReported{false}, sourceChanged{false}, audioChanged{false};
        auto hostAudio = std::make_shared<proof::AudioEvidence>();
        RoomCliHooks hostHooks;
        hostHooks.pump = [&] { return !stopHost; };
        hostHooks.report = [&](const QJsonObject& value) {
            const auto encoded = QJsonDocument(value).toJson();
            Check(!encoded.contains("test-only-password") && !encoded.contains("token"));
            if (value["type"] == "settings") { Check(value["error"].toInt() == 0); accepted = true; }
            if (value["type"] == "capture") { Check(value["error"].toInt() == 0 && value["revision"].toInt() == 2); sourceChanged = true; }
            if (value["type"] == "audio") { Check(value["error"].toInt() == 0 && value["revision"].toInt() == 2); audioChanged = true; }
            if (value["phase"] == "active") {
                std::lock_guard lock(mutex); roomId = value["roomId"].toString().toStdString();
            }
            for (const auto& peer : value["peers"].toArray()) {
                const auto row = peer.toObject();
                if (row["allocatedVideoBps"].toInt() == 1472000 && row["appliedVideoBps"].toInt() == 1472000) budgetReported = true;
                if (row["transportSendBps"].toDouble() > 0) rateReported = true;
                if (row["width"].toInt() == 160 && row["appliedRevision"] == row["observedRevision"] && !row["rejected"].toBool()) applied = true;
            }
            if (value["phase"] == "stopped") stopped = true;
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
        object["playbackChanges"] = QJsonArray{QJsonObject{{"atMs", 2000}, {"muted", true}}, QJsonObject{{"atMs", 4000}, {"deviceId", "replacement"}, {"volume", 50}}};
        auto invalidPlayback = object; invalidPlayback["playbackChanges"] = QJsonArray{QJsonObject{{"atMs", 100}, {"volume", 101}}};
        Reject([&] { ParseRoomSessionConfig(invalidPlayback, true); });
        invalidPlayback = object; invalidPlayback["host"] = true;
        Reject([&] { ParseRoomSessionConfig(invalidPlayback, true); });
        invalidPlayback = object; invalidPlayback["playbackChanges"] = QJsonArray{QJsonObject{{"atMs", 100}}, QJsonObject{{"atMs", 100}}};
        Reject([&] { ParseRoomSessionConfig(invalidPlayback, true); });
        const auto viewer = ParseRoomSessionConfig(object, true);
        auto frames = std::make_shared<LatestRoomVideoFrame>();
        auto audio = std::make_shared<proof::AudioEvidence>();
        unsigned original = 0, changed = 0;
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
        screenshare::ReceiverPreviewWindow preview;
        preview.SetLowLatency(true);
        preview.Show();
#endif
        RoomCliHooks viewerHooks;
        int playbackChanges = 0;
        viewerHooks.report = [&](const QJsonObject& value) {
            if (value["type"] == "playback") { Check(value["error"].toInt() == 0); ++playbackChanges; }
        };
        viewerHooks.pump = [&] {
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
            Check(preview.PumpMessages());
#endif
            if (auto frame = frames->Take()) {
                Check(frame->pixels().size() == frame->width * frame->height * 3 / 2 && frame->retainedPixels && frame->nv12.empty());
                Check(frame->pixels()[frame->width * frame->height / 2 + frame->width / 2] >= 35);
                if (frame->width == 320) ++original;
                else if (frame->width == 160) ++changed;
                else Check(false);
#ifdef SCREENSHARE_WINDOWS_CLI_PROOF
                preview.PresentFrame(*frame);
#endif
            }
            return true;
        };
        const int viewing = RunRoomCliSession(viewer, Factory(viewer, audio, frames), viewerHooks, true);
        stopHost = true;
        Check(playbackChanges == 2);
        Check(hosting.get() == 0 && viewing == 0 && stopped && accepted && applied && budgetReported && rateReported && sourceChanged && audioChanged);
        Check(original >= 10 && changed >= 10 && audio->audibleBlocks >= 20);
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
        Check(RunRoomCliSession(host, Factory(host, hostAudio), cancelHooks, true) == 0);
        Check(cancelledAdmission && cancelledStopped);
        auto missing = viewer; missing.room.roomId = "nonexistent-room";
        bool admissionError = false;
        RoomCliHooks missingHooks;
        missingHooks.report = [&](const QJsonObject& value) { if (value["type"] == "admission-error") admissionError = true; };
        Check(RunRoomCliSession(missing, Factory(missing, audio), missingHooks, true) == 1 && admissionError);
        std::cout << "{\"passed\":true,\"cli_session\":true,\"live_settings\":true,\"bounded_presentation\":true,\"silent_audio\":true,\"original_frames\":" << original << ",\"changed_frames\":" << changed << "}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; exitCode = 1; }
    webrtc::CleanupSSL(); return exitCode;
}
