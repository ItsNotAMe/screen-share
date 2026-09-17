#include "media/webrtc/CaptureVideoSource.h"
#include "media/webrtc/TransportSendRate.h"
#include "media/webrtc/ReceiverTelemetryChannel.h"
#include "api/make_ref_counted.h"
#include "rtc_base/logging.h"
#include "core/WindowsMediaRuntime.h"
#include <iostream>
#include <string_view>
#include <limits>

using namespace screenshare::media;
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Sink : webrtc::VideoSinkInterface<webrtc::VideoFrame> {
    int frames = 0, width = 0, height = 0;
    webrtc::scoped_refptr<webrtc::I420BufferInterface> last;
    void OnFrame(const webrtc::VideoFrame& frame) override {
        ++frames; width = frame.width(); height = frame.height(); last = frame.video_frame_buffer()->ToI420();
    }
};
int main(int argc, char** argv) try {
    Require(argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--gpu"), "Unknown settings test argument");
    webrtc::LoggingConfig logging;
    logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE);
    logging.set_log_to_stderr(false);
    Require(webrtc::InitializeLogging(std::move(logging)), "Logging initialization failed");
    StreamPreferences preferences;
    auto limits = ValidateStreamPreferences(preferences);
    Require(limits.maxVideoBitrateBps == 12441600 && limits.initialVideoBitrateBps == 3000000,
            "Default bitrate calculation changed");
    Require(limits.degradation == StreamDegradation::MaintainFps, "Gaming did not prioritize FPS");
    preferences.fpsMode = SettingMode::Auto; preferences.preset = StreamPreset::Quality;
    Require(ValidateStreamPreferences(preferences).degradation == StreamDegradation::Balanced, "Quality Auto mapping failed");
    preferences.resolution = ResolutionMode::Fixed;
    Require(ValidateStreamPreferences(preferences).degradation == StreamDegradation::MaintainResolution, "Fixed size mapping failed");
    preferences.fpsMode = SettingMode::Manual;
    Require(ValidateStreamPreferences(preferences).degradation == StreamDegradation::Disabled, "Manual adaptation enabled");
    preferences.bitrateMode = SettingMode::Manual;
    bool invalid = false;
    try { ValidateStreamPreferences(preferences); } catch (const std::invalid_argument&) { invalid = true; }
    Require(invalid, "Manual bitrate accepted without a value");
    preferences.bitrateLimitBps = 200000;
    Require(ValidateStreamPreferences(preferences).initialVideoBitrateBps == 200000, "Initial rate exceeds selected limit");
    preferences.width = 3840; preferences.height = 2160; preferences.fps = 240;
    preferences.bitrateMode = SettingMode::Auto; preferences.bitrateLimitBps.reset();
    Require(ValidateStreamPreferences(preferences).maxVideoBitrateBps == 40000000, "Auto ceiling clamp overflowed");
    preferences.aggregateUploadLimitBps = 4000000;
    Require(AllocateViewerVideo(preferences, 4) == 672000, "Four-viewer audio/overhead reservation failed");
    Require(AllocateViewerVideo(preferences, 3) == 938666, "Departure did not redistribute allowance");
    preferences.bitrateLimitBps = 200000;
    Require(AllocateViewerVideo(preferences, 4) == 200000, "Aggregate allocation exceeded individual cap");
    preferences.aggregateUploadLimitBps = 160000;
    Require(AllocateViewerVideo(preferences, 1) == 0, "Audio-only budget failed to pause video");
    for (int budget : {160000, 1000000, 4000000, 1000000000}) {
        preferences.aggregateUploadLimitBps = budget;
        for (size_t viewers = 1; viewers <= 63; ++viewers) {
            const auto share = AllocateViewerVideo(preferences, viewers);
            Require(int64_t(share) * viewers <= std::max<int64_t>(0, int64_t(budget) * 4 / 5 - int64_t(viewers) * 128000), "Allocation exceeded available video budget");
            Require(share <= *preferences.bitrateLimitBps, "Individual cap exceeded");
        }
    }
    preferences.aggregateUploadLimitBps = 159999; invalid = false;
    try { ValidateStreamPreferences(preferences); } catch (const std::invalid_argument&) { invalid = true; }
    Require(invalid, "Invalid aggregate allowance accepted");
    auto rate = std::make_shared<TransportSendRate>();
    Require(!rate->Read().stale && !rate->Read().bitsPerSecond, "Unmeasured transport was not unknown");
    auto collector = webrtc::make_ref_counted<TransportSendRateCallback>(rate);
    auto sample = [&](int64_t microseconds, const char* id, uint64_t bytes) {
        auto report = webrtc::RTCStatsReport::Create(webrtc::Timestamp::Micros(microseconds));
        auto transport = std::make_unique<webrtc::RTCTransportStats>(id, report->timestamp());
        transport->bytes_sent = bytes; report->AddStats(std::move(transport)); collector->OnStatsDelivered(report);
    };
    sample(1000000, "transport", 1000); Require(!rate->bitsPerSecond, "First counter sample fabricated a rate");
    sample(2000000, "transport", 201000); Require(rate->bitsPerSecond == 1600000, "Transport rate units incorrect");
    const auto measuredAt = rate->sampled;
    Require(rate->Read(measuredAt + std::chrono::milliseconds(2999)).bitsPerSecond == 1600000, "Fresh sample expired early");
    const auto expired = rate->Read(measuredAt + std::chrono::seconds(3));
    Require(expired.stale && !expired.bitsPerSecond, "Stale transport measurement escaped deadline");
    sample(3000000, "transport", 100); Require(!rate->bitsPerSecond, "Counter reset fabricated a rate");
    sample(4000000, "replacement", 999999); Require(!rate->bitsPerSecond, "Replacement transport reused old counters");
    sample(4000000, "replacement", 999999); Require(!rate->bitsPerSecond, "Duplicate timestamp fabricated a rate");
    sample(5000000, "replacement", 999999);
    Require(rate->Read().bitsPerSecond == 0 && !rate->Read().stale, "Measured zero confused with missing sample");
    auto nextGeneration = std::make_shared<TransportSendRate>();
    sample(6000000, "replacement", 1000000);
    Require(!nextGeneration->Read().bitsPerSecond, "Late retired-generation callback contaminated a new peer");

    Sink fixedSink, adaptiveSink;
    auto networkSample = [&](int64_t timestamp, uint64_t bytes, const char* id = "outbound-video", bool invalid = false) {
        auto report = webrtc::RTCStatsReport::Create(webrtc::Timestamp::Micros(timestamp));
        auto transport = std::make_unique<webrtc::RTCTransportStats>("transport", report->timestamp());
        transport->bytes_sent = bytes + 1000; transport->selected_candidate_pair_id = "selected";
        report->AddStats(std::move(transport));
        for (const auto* name : {"selected", "unused"}) {
            auto pair = std::make_unique<webrtc::RTCIceCandidatePairStats>(name, report->timestamp());
            pair->current_round_trip_time = std::string_view(name) == "selected" ? 0.025 : 0.001;
            pair->available_outgoing_bitrate = invalid ? std::numeric_limits<double>::infinity() : 5000000;
            report->AddStats(std::move(pair));
        }
        auto video = std::make_unique<webrtc::RTCOutboundRtpStreamStats>(id, report->timestamp());
        video->kind = "video"; video->bytes_sent = bytes; video->frames_per_second = invalid ? -1 : 30;
        video->quality_limitation_reason = invalid ? "unrecognized" : "bandwidth"; video->remote_id = "remote-video";
        report->AddStats(std::move(video));
        auto remote = std::make_unique<webrtc::RTCRemoteInboundRtpStreamStats>("remote-video", report->timestamp());
        remote->fraction_lost = invalid ? 1.5 : 0.02; remote->jitter = invalid ? std::numeric_limits<double>::quiet_NaN() : 0.003;
        report->AddStats(std::move(remote)); collector->OnStatsDelivered(report);
    };
    networkSample(7000000, 1000); Require(!rate->Read().sender.payloadBps, "First RTP counter fabricated a rate");
    networkSample(8000000, 101000);
    auto sender = rate->Read().sender;
    Require(sender.payloadBps == 800000 && sender.encodedFps == 30 && sender.rttMs == 25 && sender.jitterMs == 3 &&
        sender.lossFraction == 0.02 && sender.availableOutgoingBps == 5000000 && sender.limitingReason == VideoLimitReason::Bandwidth,
        "Sender units/selected-pair/RTCP mapping incorrect");
    const auto staleSender = rate->Read(rate->sampled + std::chrono::seconds(3)).sender;
    Require(!staleSender.payloadBps && !staleSender.rttMs && staleSender.limitingReason == VideoLimitReason::Unknown, "Stale sender values escaped expiry");
    networkSample(9000000, 101000); Require(rate->Read().sender.payloadBps == 0, "Zero payload rate became unknown");
    networkSample(10000000, 1); Require(!rate->Read().sender.payloadBps, "Reset RTP counter fabricated a rate");
    networkSample(11000000, 1000, "replacement-video", true); sender = rate->Read().sender;
    Require(!sender.payloadBps && !sender.encodedFps && !sender.availableOutgoingBps && !sender.jitterMs && !sender.lossFraction &&
        sender.limitingReason == VideoLimitReason::Unknown, "Invalid or replacement stats were trusted");
    sample(12000000, "transport", 0); Require(!rate->Read().sender.rttMs, "Missing path retained prior measurements");
    auto receiverMailbox = std::make_shared<ReceiverStatsMailbox>();
    auto receiverCollector = webrtc::make_ref_counted<ReceiverStatsCallback>(receiverMailbox);
    auto receiverReport = webrtc::RTCStatsReport::Create(webrtc::Timestamp::Micros(1000000));
    auto inbound = std::make_unique<webrtc::RTCInboundRtpStreamStats>("video", receiverReport->timestamp());
    inbound->kind = "video"; inbound->frame_width = 320; inbound->frame_height = 180; inbound->frames_decoded = 30;
    inbound->frames_per_second = 29.97; receiverReport->AddStats(std::move(inbound));
    receiverCollector->OnStatsDelivered(receiverReport);
    Require(receiverMailbox->video && receiverMailbox->video->fpsMilli == 29970 && receiverMailbox->serial == 1,
        "Receiver collector lost decoder stats");
    auto emptyReport = webrtc::RTCStatsReport::Create(webrtc::Timestamp::Micros(2000000));
    receiverCollector->OnStatsDelivered(emptyReport);
    Require(!receiverMailbox->video && receiverMailbox->serial == 2, "Absent decoder retained old values");
    auto replacementMailbox = std::make_shared<ReceiverStatsMailbox>();
    receiverCollector->OnStatsDelivered(receiverReport);
    Require(!replacementMailbox->video && replacementMailbox->serial == 0, "Retired stats callback published to new generation");
    auto fixed = webrtc::make_ref_counted<CaptureVideoSource>();
    auto adaptive = webrtc::make_ref_counted<CaptureVideoSource>();
    preferences = {};
    preferences.resolution = ResolutionMode::Fixed; preferences.width = 160; preferences.height = 120; preferences.fps = 30;
    fixed->Configure(preferences, 1);
    preferences.resolution = ResolutionMode::Auto; preferences.width = 640; preferences.height = 360;
    preferences.fpsMode = SettingMode::Auto;
    adaptive->Configure(preferences, 1);
    webrtc::VideoSinkWants wants;
    wants.max_pixel_count = 160 * 90; wants.max_framerate_fps = 10; wants.is_active = true;
    fixed->AddOrUpdateSink(&fixedSink, wants); adaptive->AddOrUpdateSink(&adaptiveSink, wants);
    SyntheticCaptureResource pixels;
    pixels.width = 640; pixels.height = 360; pixels.luma.resize(640 * 360, 80);
    const auto beginning = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    for (int i = 0; i < 60; ++i) {
        const auto at = beginning + std::chrono::microseconds(i * 33334);
        fixed->Push(pixels, at); adaptive->Push(pixels, at);
    }
    Require(fixedSink.width == 160 && fixedSink.height == 120 && fixedSink.frames >= 55,
            "Manual resolution/FPS obeyed adaptive sink reductions");
    Require(adaptiveSink.width * adaptiveSink.height <= 160 * 90 && adaptiveSink.frames <= 22 && adaptiveSink.frames >= 18,
            "Auto did not obey WebRTC pixel/FPS restrictions");
    auto fixedStats = fixed->settingsStats();
    Require(fixedStats.observedRevision == 1 && fixedStats.imageWidth == 160 && fixedStats.imageHeight == 90,
            "Fixed aspect-ratio mapping is incorrect");
    Require(fixedSink.last->DataY()[0] == 16 && fixedSink.last->DataY()[60 * fixedSink.last->StrideY() + 80] == 80,
            "Fixed canvas cropped or stretched source instead of letterboxing");
    const int reducedWidth = adaptiveSink.width;
    wants.max_pixel_count = 640 * 360; wants.max_framerate_fps = 30;
    adaptive->AddOrUpdateSink(&adaptiveSink, wants);
    adaptive->Push(pixels, beginning + std::chrono::seconds(3));
    Require(adaptiveSink.width > reducedWidth && adaptiveSink.width <= 640 && adaptiveSink.height <= 360,
            "Auto did not recover upward or upscaled beyond source");
    Require(fixed->settingsStats().width == 160, "One source's restrictions changed another source");
    preferences.resolution = ResolutionMode::Native;
    adaptive->Configure(preferences, 2);
    adaptive->Push(pixels, beginning + std::chrono::seconds(4));
    Require(adaptiveSink.width == 640 && adaptiveSink.height == 360 && adaptive->settingsStats().observedRevision == 2,
            "Native resolution did not follow source");
    invalid = false;
    try { adaptive->Configure(preferences, 2); } catch (const std::invalid_argument&) { invalid = true; }
    Require(invalid && adaptive->requestedRevision() == 2, "Stale settings revision accepted");
    preferences.width = 641;
    try { adaptive->Configure(preferences, 3); } catch (const std::invalid_argument&) {}
    Require(adaptive->requestedRevision() == 2, "Invalid settings replaced working revision");
    fixed->RemoveSink(&fixedSink); adaptive->RemoveSink(&adaptiveSink);
    uint64_t gpuFallbacks = 0;
    if (argc == 2) {
        screenshare::WindowsMediaRuntime runtime;
        Require(SUCCEEDED(runtime.result()), "MTA lease failed");
        auto device = std::make_shared<D3dVideoDevice>();
        std::vector<uint8_t> nv12(640 * 360 * 3 / 2, 128);
        std::fill_n(nv12.begin(), 640 * 360, uint8_t(80));
        auto texture = device->UploadNv12(640, 360, nv12);
        Sink gpuSink;
        auto gpuSource = webrtc::make_ref_counted<CaptureVideoSource>();
        preferences = {}; preferences.resolution = ResolutionMode::Fixed;
        preferences.width = 320; preferences.height = 180;
        gpuSource->Configure(preferences, 1);
        gpuSource->AddOrUpdateSink(&gpuSink, webrtc::VideoSinkWants());
        gpuSource->PushBuffer(texture, std::chrono::steady_clock::now() - std::chrono::milliseconds(100));
        gpuSource->RemoveSink(&gpuSink);
        gpuFallbacks = gpuSource->settingsStats().gpuReadbackFallbacks;
        Require(gpuSink.width == 320 && gpuSink.height == 180 && gpuSink.last->DataY()[0] == 80 &&
                gpuFallbacks == 1 && device->readbackCount() == 1,
                "GPU scaling fallback lost pixels or was not reported");
        struct NativeSink : webrtc::VideoSinkInterface<webrtc::VideoFrame> {
            bool native = false;
            void OnFrame(const webrtc::VideoFrame& frame) override {
                native = frame.width() == 640 && frame.height() == 360 &&
                    frame.video_frame_buffer()->type() == webrtc::VideoFrameBuffer::Type::kNative;
            }
        } nativeSink;
        preferences.width = 640; preferences.height = 360;
        gpuSource->Configure(preferences, 2);
        gpuSource->AddOrUpdateSink(&nativeSink, webrtc::VideoSinkWants());
        gpuSource->PushBuffer(texture);
        gpuSource->RemoveSink(&nativeSink);
        Require(nativeSink.native && gpuSource->settingsStats().gpuReadbackFallbacks == 1 && device->readbackCount() == 1,
                "Matching fixed dimensions caused unnecessary GPU readback");
    }
    std::cout << "{\"passed\":true,\"mode\":\"stream-settings\",\"manual_frames\":" << fixedSink.frames
              << ",\"auto_frames_before_recovery\":" << adaptiveSink.frames - 2
              << ",\"fixed_canvas\":[160,120],\"active_image\":[160,90],\"gpu_scaling_fallbacks\":" << gpuFallbacks << "}\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
