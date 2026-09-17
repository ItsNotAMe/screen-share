#include "media/webrtc/CaptureVideoSource.h"
#include "media/webrtc/TransportSendRate.h"
#include "media/webrtc/ReceiverTelemetryChannel.h"
#include "api/make_ref_counted.h"
#include "rtc_base/logging.h"
#include "rtc_base/thread.h"
#include "core/WindowsMediaRuntime.h"
#include "media/GpuSubmissionQueue.h"
#include <iostream>
#include <string_view>
#include <limits>
#include <future>
#include "SettingsFailureTest.h"

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
    GpuSubmissionQueue<std::shared_ptr<bool>> pendingGpu;
    std::vector<std::shared_ptr<bool>> completions;
    auto ready = [&] { return pendingGpu.Ready([](const auto& completion) { return *completion; }); };
    for (unsigned i = 0; i < 4; ++i) {
        Require(ready(), "GPU queue refused work within its bound");
        completions.push_back(std::make_shared<bool>(false)); pendingGpu.Submitted(completions.back());
    }
    for (unsigned i = 0; i < 100; ++i) Require(!ready() && pendingGpu.size() == 4, "Stalled GPU grew its queue");
    *completions[0] = true;
    Require(ready() && pendingGpu.size() == 3, "Completed GPU work did not restore capacity");
    completions.push_back(std::make_shared<bool>(false)); pendingGpu.Submitted(completions.back());
    Require(!ready(), "GPU completion allowed more than one replacement submission");
    for (const auto& completion : completions) *completion = true;
    Require(ready() && pendingGpu.size() == 0, "Completed GPU tokens were retained");
    Require(argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--gpu"), "Unknown settings test argument");
    webrtc::LoggingConfig logging;
    logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE);
    logging.set_log_to_stderr(false);
    Require(webrtc::InitializeLogging(std::move(logging)), "Logging initialization failed");
    settings_test::Run();
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
        video->encoder_implementation = invalid ? "untrusted implementation / path" : "Media Foundation H264 hardware (D3D11/NV12)";
        video->total_encode_time = invalid ? -1 : 0.2; video->frames_encoded = 100;
        video->retransmitted_packets_sent = invalid ? UINT64_MAX : 3;
        video->nack_count = 2; video->pli_count = 1;
        report->AddStats(std::move(video));
        auto remote = std::make_unique<webrtc::RTCRemoteInboundRtpStreamStats>("remote-video", report->timestamp());
        remote->fraction_lost = invalid ? 1.5 : 0.02; remote->jitter = invalid ? std::numeric_limits<double>::quiet_NaN() : 0.003;
        report->AddStats(std::move(remote)); collector->OnStatsDelivered(report);
    };
    networkSample(7000000, 1000); Require(!rate->Read().sender.payloadBps, "First RTP counter fabricated a rate");
    networkSample(8000000, 101000);
    auto sender = rate->Read().sender;
    Require(sender.payloadBps == 800000 && sender.encodedFps == 30 && sender.rttMs == 25 && sender.jitterMs == 3 &&
        sender.lossFraction == 0.02 && sender.availableOutgoingBps == 5000000 && sender.limitingReason == VideoLimitReason::Bandwidth &&
        sender.encoder == CodecImplementation::MfH264Hardware && sender.meanEncodeMs == 2 &&
        sender.retransmittedPackets == 3 && sender.nackCount == 2 && sender.pliCount == 1,
        "Sender units/selected-pair/RTCP mapping incorrect");
    const auto staleSender = rate->Read(rate->sampled + std::chrono::seconds(3)).sender;
    Require(!staleSender.payloadBps && !staleSender.rttMs && staleSender.limitingReason == VideoLimitReason::Unknown &&
        staleSender.encoder == CodecImplementation::Unknown && !staleSender.meanEncodeMs && !staleSender.retransmittedPackets,
        "Stale sender values escaped expiry");
    networkSample(9000000, 101000); Require(rate->Read().sender.payloadBps == 0, "Zero payload rate became unknown");
    networkSample(10000000, 1); Require(!rate->Read().sender.payloadBps, "Reset RTP counter fabricated a rate");
    networkSample(11000000, 1000, "replacement-video", true); sender = rate->Read().sender;
    Require(!sender.payloadBps && !sender.encodedFps && !sender.availableOutgoingBps && !sender.jitterMs && !sender.lossFraction &&
        sender.limitingReason == VideoLimitReason::Unknown && sender.encoder == CodecImplementation::Unknown &&
        !sender.meanEncodeMs && !sender.retransmittedPackets,
        "Invalid or replacement stats were trusted");
    sample(12000000, "transport", 0); Require(!rate->Read().sender.rttMs, "Missing path retained prior measurements");
    auto receiverMailbox = std::make_shared<ReceiverStatsMailbox>();
    auto receiverCollector = webrtc::make_ref_counted<ReceiverStatsCallback>(receiverMailbox);
    auto receiverReport = webrtc::RTCStatsReport::Create(webrtc::Timestamp::Micros(1000000));
    auto inbound = std::make_unique<webrtc::RTCInboundRtpStreamStats>("video", receiverReport->timestamp());
    inbound->kind = "video"; inbound->frame_width = 320; inbound->frame_height = 180; inbound->frames_decoded = 30;
    inbound->frames_per_second = 29.97; inbound->frames_dropped = 7;
    inbound->jitter_buffer_delay = 2.0; inbound->jitter_buffer_emitted_count = 100;
    inbound->decoder_implementation = "Media Foundation H264 (CPU NV12)";
    receiverReport->AddStats(std::move(inbound));
    receiverCollector->OnStatsDelivered(receiverReport);
    // Codec labels are allowlisted; cumulative buffering averages are reported
    // only with a finite nonnegative delay and a nonzero emitted count.
    Require(receiverMailbox->video && receiverMailbox->video->fpsMilli == 29970 && receiverMailbox->serial == 1,
        "Receiver collector lost decoder stats");
    Require(receiverMailbox->video->decoderDrops == 7 && receiverMailbox->video->jitterBufferMeanMs == 20 &&
        receiverMailbox->video->decoder == CodecImplementation::MfH264Software, "Receiver codec/buffering mapping incorrect");
    auto emptyReport = webrtc::RTCStatsReport::Create(webrtc::Timestamp::Micros(2000000));
    receiverCollector->OnStatsDelivered(emptyReport);
    Require(!receiverMailbox->video && receiverMailbox->serial == 2, "Absent decoder retained old values");
    auto replacementMailbox = std::make_shared<ReceiverStatsMailbox>();
    receiverCollector->OnStatsDelivered(receiverReport);
    Require(!replacementMailbox->video && replacementMailbox->serial == 0, "Retired stats callback published to new generation");
    auto invalidReport = webrtc::RTCStatsReport::Create(webrtc::Timestamp::Micros(3000000));
    auto invalidInbound = std::make_unique<webrtc::RTCInboundRtpStreamStats>("video", invalidReport->timestamp());
    invalidInbound->kind = "video"; invalidInbound->frame_width = 320; invalidInbound->frame_height = 180;
    invalidInbound->frames_decoded = 1; invalidInbound->jitter_buffer_delay = std::numeric_limits<double>::infinity();
    invalidInbound->jitter_buffer_emitted_count = 0; invalidInbound->decoder_implementation = "untrusted implementation / path";
    invalidReport->AddStats(std::move(invalidInbound)); receiverCollector->OnStatsDelivered(invalidReport);
    Require(receiverMailbox->video && !receiverMailbox->video->decoderDrops && !receiverMailbox->video->jitterBufferMeanMs &&
        receiverMailbox->video->decoder == CodecImplementation::Unknown, "Unknown receiver measurements were fabricated");
    auto ambiguous = std::make_unique<webrtc::RTCInboundRtpStreamStats>("other", invalidReport->timestamp());
    ambiguous->kind = "video"; invalidReport->AddStats(std::move(ambiguous)); receiverCollector->OnStatsDelivered(invalidReport);
    Require(!receiverMailbox->video, "Ambiguous video receiver selected arbitrarily");
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
    preferences.resolution = ResolutionMode::Fixed; preferences.width = 320; preferences.height = 180;
    adaptive->Configure(preferences, 3);
    adaptive->Push(pixels, beginning + std::chrono::seconds(4));
    Require(adaptive->settingsStats().observedRevision == 2 && adaptive->settingsStats().width == 640,
        "A dropped frame falsely acknowledged new dimensions");
    adaptive->Push(pixels, beginning + std::chrono::seconds(5));
    Require(adaptive->settingsStats().observedRevision == 3 && adaptive->settingsStats().width == 320,
        "Accepted frame did not acknowledge its actual dimensions");
    fixed->RemoveSink(&fixedSink); adaptive->RemoveSink(&adaptiveSink);
    uint64_t gpuFallbacks = 0;
    if (argc == 2) {
        screenshare::WindowsMediaRuntime runtime;
        Require(SUCCEEDED(runtime.result()), "MTA lease failed");
        auto device = std::make_shared<D3dVideoDevice>();
        std::vector<uint8_t> nv12(640 * 360 * 3 / 2, 128);
        std::fill_n(nv12.begin(), 640 * 360, uint8_t(80));
        for (int y = 0; y < 180; ++y) std::fill_n(nv12.begin() + y * 640 + 480, 160, uint8_t(160));
        for (size_t i = 640 * 360; i < nv12.size(); i += 2) { nv12[i] = 90; nv12[i + 1] = 170; }
        auto texture = device->UploadNv12(640, 360, nv12);
        struct NativeSink : webrtc::VideoSinkInterface<webrtc::VideoFrame> {
            webrtc::scoped_refptr<webrtc::VideoFrameBuffer> last;
            unsigned frames = 0;
            void OnFrame(const webrtc::VideoFrame& frame) override { last = frame.video_frame_buffer(); ++frames; }
        } gpuSink;
        auto gpuSource = webrtc::make_ref_counted<CaptureVideoSource>();
        preferences = {}; preferences.resolution = ResolutionMode::Fixed;
        preferences.width = 320; preferences.height = 180;
        gpuSource->Configure(preferences, 1);
        gpuSource->AddOrUpdateSink(&gpuSink, webrtc::VideoSinkWants());
        auto at = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        auto push = [&] { gpuSource->PushBuffer(texture, at); at += std::chrono::milliseconds(100); };
        push();
        Require(gpuSink.last && gpuSink.last->type() == webrtc::VideoFrameBuffer::Type::kNative &&
            gpuSink.last->width() == 320 && gpuSink.last->height() == 180 && device->readbackCount() == 0 &&
            gpuSource->settingsStats().gpuScaled == 1, "GPU downscale used CPU readback");
        auto held = gpuSink.last;
        preferences.height = 240; gpuSource->Configure(preferences, 2); push();
        const auto letterbox = gpuSource->settingsStats();
        Require(letterbox.imageLeft == 0 && letterbox.imageTop == 30 && letterbox.imageWidth == 320 &&
            letterbox.imageHeight == 180 && letterbox.scalingPath == SourceScalingPath::Gpu &&
            device->readbackCount() == 0, "GPU letterbox geometry/readback incorrect");
        auto boxed = gpuSink.last->ToI420();
        Require(boxed && boxed->DataY()[0] == 16 &&
            std::abs(int(boxed->DataY()[120 * boxed->StrideY() + 160]) - 80) <= 1 &&
            std::abs(int(boxed->DataU()[60 * boxed->StrideU() + 80]) - 90) <= 1 &&
            std::abs(int(boxed->DataV()[60 * boxed->StrideV() + 80]) - 170) <= 1 &&
            boxed->DataU()[0] == 128 && boxed->DataV()[0] == 128, "GPU letterbox changed YUV colors or black bars");
        const auto reads = device->readbackCount();
        preferences.width = 1280; preferences.height = 720; gpuSource->Configure(preferences, 3); push();
        Require(gpuSink.last->width() == 1280 && gpuSink.last->height() == 720 &&
            device->readbackCount() == reads, "GPU upscale used readback");
        // Another viewer has independent dimensions; its work cannot mutate retained output.
        NativeSink otherSink;
        auto other = webrtc::make_ref_counted<CaptureVideoSource>();
        preferences.width = 240; preferences.height = 320; other->Configure(preferences, 1);
        other->AddOrUpdateSink(&otherSink, webrtc::VideoSinkWants()); other->PushBuffer(texture, at);
        Require(otherSink.last->width() == 240 && otherSink.last->height() == 320 &&
            other->settingsStats().imageWidth == 240 && other->settingsStats().imageHeight == 134 &&
            gpuSink.last->width() == 1280 && device->readbackCount() == reads, "Viewer scaling leaked across sources");
        auto retained = held->ToI420();
        Require(retained && retained->width() == 320 && retained->height() == 180 &&
            std::abs(int(retained->DataY()[0]) - 80) <= 1 &&
            std::abs(int(retained->DataY()[20 * retained->StrideY() + 280]) - 160) <= 1 &&
            std::abs(int(retained->DataY()[140 * retained->StrideY() + 280]) - 80) <= 1,
            "GPU scale flipped/cropped input or later output overwrote retained frame");
        // Auto must not upscale a source merely because its configured maximum is larger.
        preferences.resolution = ResolutionMode::Auto; preferences.width = 1280; preferences.height = 720;
        gpuSource->Configure(preferences, 4); push();
        Require(gpuSink.last.get() == texture.get() && gpuSource->settingsStats().scalingPath == SourceScalingPath::Unchanged,
            "Auto upscaled native source");
        webrtc::VideoSinkWants reduced;
        reduced.max_pixel_count = 160 * 90; reduced.is_active = true;
        gpuSource->AddOrUpdateSink(&gpuSink, reduced); push();
        Require(gpuSink.last->width() * gpuSink.last->height() <= 160 * 90 &&
            gpuSource->settingsStats().scalingPath == SourceScalingPath::Gpu, "Auto adaptation bypassed GPU scaling");
        const auto beforeConcurrent = device->readbackCount();
        auto deliver = [&](auto source) {
            for (int i = 0; i < 20; ++i) source->PushBuffer(texture, at + std::chrono::milliseconds(i * 40));
        };
        auto firstViewer = std::async(std::launch::async, [&] { deliver(gpuSource); });
        auto secondViewer = std::async(std::launch::async, [&] { deliver(other); });
        firstViewer.get(); secondViewer.get(); at += std::chrono::seconds(1);
        Require(device->readbackCount() == beforeConcurrent && gpuSink.last->width() <= 160 &&
            otherSink.last->width() == 240, "Concurrent viewers used readback or shared dimensions");
        Require(device->maximumPendingScales() > 0 && device->maximumPendingScales() <= 4 && device->scalingFailures() == 0,
            "Concurrent scaling exceeded submission bound or treated backpressure as a device failure");
        bool badRectangle = false;
        try { texture->Scale(320, 180, 2, 0, 320, 180); } catch (const std::invalid_argument&) { badRectangle = true; }
        Require(badRectangle && device->scalingFailures() == 0, "Invalid rectangle reached GPU or quarantined healthy device");
        Require(texture->ToI420() != nullptr, "Source readback for pixel validation failed"); // Also drains submitted GPU work for the failure test.
        // Unsupported GPU input exercises real shader failure, quarantine and CPU fallback.
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 640; desc.Height = 360; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_NV12; desc.Usage = D3D11_USAGE_DEFAULT; // No shader-resource binding.
        D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = nv12.data(); initial.SysMemPitch = 640;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> unsupported;
        Require(SUCCEEDED(device->device()->CreateTexture2D(&desc, &initial, &unsupported)), "Fallback input creation failed");
        auto unsupportedFrame = webrtc::make_ref_counted<D3dVideoFrameBuffer>(device, unsupported, 640, 360);
        preferences.resolution = ResolutionMode::Fixed; preferences.width = 320; preferences.height = 240;
        gpuSource->Configure(preferences, 5);
        gpuSource->PushBuffer(unsupportedFrame, at); at += std::chrono::milliseconds(100);
        Require(gpuSink.last->type() == webrtc::VideoFrameBuffer::Type::kI420 && device->scalingFailures() == 1 &&
            gpuSource->settingsStats().scalingPath == SourceScalingPath::CpuReadback, "Failed GPU scaler did not fall back");
        push();
        gpuFallbacks = gpuSource->settingsStats().gpuReadbackFallbacks;
        Require(gpuFallbacks == 2 && device->scalingFailures() == 1 && gpuSink.last->ToI420()->DataY()[0] == 16,
            "Quarantined scaler retried or fallback lost letterboxing");
        const auto framesBeforeRetire = gpuSink.frames;
        const auto readsBeforeRetire = device->readbackCount();
        device->Retire(); push();
        Require(gpuSink.frames == framesBeforeRetire && device->readbackCount() == readsBeforeRetire &&
            !held->ToI420(), "Retired GPU frame was scaled or read back");
        gpuSource->RemoveSink(&gpuSink); other->RemoveSink(&otherSink);
        // The final retained texture may be released by a signaling executor
        // after WebRTC has restricted cross-thread invokes. Exercise a live
        // scaler's complete teardown without widening that thread's permissions.
        auto releasing = webrtc::Thread::Create(); Require(releasing->Start(), "Release thread startup failed");
        auto teardownDevice = std::make_shared<D3dVideoDevice>();
        std::weak_ptr<D3dVideoDevice> lifetime = teardownDevice;
        std::vector<uint8_t> teardownPixels(320 * 180 * 3 / 2, 128);
        auto teardownSource = teardownDevice->UploadNv12(320, 180, teardownPixels);
        auto teardownFrame = teardownSource->Scale(160, 90, 0, 0, 160, 90);
        Require(bool(teardownFrame), "Teardown regression requires a live scaler");
        teardownSource = nullptr; teardownDevice.reset();
        releasing->BlockingCall([&] {
            releasing->DisallowAllInvokes();
            teardownFrame = nullptr;
        });
        Require(lifetime.expired(), "GPU owner leaked after restricted-thread final release");
        releasing->Stop();
    }
    std::cout << "{\"passed\":true,\"mode\":\"stream-settings\",\"manual_frames\":" << fixedSink.frames
              << ",\"auto_frames_before_recovery\":" << adaptiveSink.frames - 3
              << ",\"fixed_canvas\":[160,120],\"active_image\":[160,90],\"gpu_scaling_fallbacks\":" << gpuFallbacks << "}\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
