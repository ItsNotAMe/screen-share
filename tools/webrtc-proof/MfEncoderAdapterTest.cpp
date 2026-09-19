#include "LifecycleDiagnostics.h"
#include "media/webrtc/MfVideoEncoderFactory.h"
#include "media/webrtc/MfHardwareSession.h"
#include "media/webrtc/MappedVideoBuffer.h"
#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "common_video/h264/h264_common.h"
#include "common_video/h264/pps_parser.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
class Sink final : public webrtc::EncodedImageCallback {
public:
    Result OnEncodedImage(const webrtc::EncodedImage& image, const webrtc::CodecSpecificInfo*) override {
        std::unique_lock lock(mutex);
        images.push_back(image);
        changed.notify_all();
        if (hold && !changed.wait_for(lock, std::chrono::seconds(5), [&] { return !hold; })) timedOut = true;
        return Result(Result::OK);
    }
    void OnFrameDropped(uint32_t, int, bool) override { std::lock_guard lock(mutex); ++dropped; }
    void Wait(size_t count) {
        std::unique_lock lock(mutex);
        Require(changed.wait_for(lock, std::chrono::seconds(5), [&] { return images.size() >= count; }), "Encode callback timeout");
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<webrtc::EncodedImage> images;
    unsigned dropped = 0;
    bool hold = false, timedOut = false;
};
webrtc::VideoFrame Frame(uint32_t timestamp) {
    auto pixels = webrtc::I420Buffer::Create(640, 360);
    pixels->InitializeData();
    return webrtc::VideoFrame::Builder().set_video_frame_buffer(pixels)
        .set_rtp_timestamp(timestamp).set_timestamp_us(int64_t(timestamp) * 1000).set_ntp_time_ms(123456).build();
}
void Rate(webrtc::VideoEncoder& encoder, uint32_t bitrate) {
    webrtc::VideoBitrateAllocation allocation;
    allocation.SetBitrate(0, 0, bitrate);
    // Exercise an FPS-driven transform restart as well as bitrate assignment.
    encoder.SetRates({allocation, bitrate ? 30.0 : 0.0});
}
void Run(bool useHardware, bool gpuInput) {
    auto hardware = useHardware ? std::make_shared<screenshare::media::MfHardwareSession>(
        std::make_shared<screenshare::media::D3dVideoDevice>()) : nullptr;
    auto makeFrame = [&](uint32_t timestamp) {
        auto frame = Frame(timestamp);
        if (gpuInput) {
            std::vector<uint8_t> pixels(640 * 360 * 3 / 2, 128);
            frame.set_video_frame_buffer(hardware->device->UploadNv12(640, 360, pixels));
        }
        if (timestamp >= 100) frame.set_video_frame_buffer(screenshare::media::WithMapping(frame.video_frame_buffer(), {},
            timestamp == 200 ? screenshare::media::StreamPreset::Quality : screenshare::media::StreamPreset::Gaming));
        return frame;
    };
    screenshare::media::MfVideoEncoderFactory factory(hardware);
    auto lowerLevel = factory.GetSupportedFormats().front();
    lowerLevel.parameters["profile-level-id"] = "64001f";
    Require(!factory.QueryCodecSupport(lowerLevel, std::nullopt, std::nullopt).is_supported,
        "Encoder would exceed a lower negotiated H264 level");
    auto encoder = factory.Create(webrtc::CreateEnvironment(), factory.GetSupportedFormats().front());
    Sink sink;
    // On failure, the encoder must be released while its callback still exists.
    struct Cleanup { webrtc::VideoEncoder& encoder; ~Cleanup() { encoder.Release(); } } cleanup{*encoder};
    webrtc::VideoCodec codec;
    codec.codecType = webrtc::kVideoCodecH264;
    codec.width = 640; codec.height = 360; codec.maxFramerate = 60;
    codec.startBitrate = 1000;
    webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 2, 1200);
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        Require(encoder->InitEncode(&codec, settings) == 0, "MF encoder initialization failed");
        Require(!useHardware || encoder->GetEncoderInfo().is_hardware_accelerated, "Hardware lifecycle test fell back");
        encoder->RegisterEncodeCompleteCallback(&sink);
        const auto before = sink.images.size();
        Require(encoder->Encode(makeFrame(1), nullptr) == 0, "First encode failed");
        sink.Wait(before + 1);
        Rate(*encoder, 0); // Also a worker barrier.
        Require(!sink.images.back().PlayoutDelay(), "Unconfigured input changed playout policy");
        for (unsigned i = 0; i < 30; ++i) Require(encoder->Encode(makeFrame(2 + i), nullptr) == 0, "Suspended input rejected");
        Rate(*encoder, 400'000);
        Require(sink.images.size() == before + 1, "Zero-rate suspension emitted video");
        const std::vector types{webrtc::VideoFrameType::kVideoFrameKey};
        Require(encoder->Encode(makeFrame(100), &types) == 0, "Resume encode failed");
        sink.Wait(before + 2);
        Rate(*encoder, 400'000);
        Require(sink.images.back()._frameType == webrtc::VideoFrameType::kVideoFrameKey, "Keyframe request lost");
        Require(sink.images.back().PlayoutDelay() == webrtc::VideoPlayoutDelay(webrtc::TimeDelta::Millis(10), webrtc::TimeDelta::Millis(10)), "Gaming playout policy lost");
        bool foundSps = false, foundCabac = false;
        const auto& key = sink.images.back();
        for (const auto& nalu : webrtc::H264::FindNaluIndices(std::span(key.data(), key.size()))) {
            if (nalu.payload_size >= 4 && (key.data()[nalu.payload_start_offset] & 31) == 7) {
                const auto* sps = key.data() + nalu.payload_start_offset;
                Require(sps[1] == 100 && sps[3] <= 42, "Encoded SPS exceeds negotiated High level 4.2");
                foundSps = true;
            }
            if (nalu.payload_size > 1 && (key.data()[nalu.payload_start_offset] & 31) == 8) {
                const auto pps = webrtc::PpsParser::ParsePps(std::span(key.data() + nalu.payload_start_offset + 1, nalu.payload_size - 1));
                foundCabac = pps && pps->entropy_coding_mode_flag;
            }
        }
        Require(foundSps, "Requested keyframe omitted SPS");
        Require(useHardware || foundCabac, "Software High-profile output did not enable CABAC");
        Require(sink.images.back().RtpTimestamp() == 100 && sink.images.back().NtpTimeMs() == 123456,
            "Encoder timestamps lost");
        {
            std::lock_guard lock(sink.mutex);
            sink.hold = true;
        }
        Require(encoder->Encode(makeFrame(200), nullptr) == 0, "Burst initial encode failed");
        sink.Wait(before + 3); // Worker blocked in callback; all following input must coalesce.
        { std::lock_guard lock(sink.mutex);
          Require(sink.images.back().PlayoutDelay() == webrtc::VideoPlayoutDelay{}, "Quality did not restore adaptive playout"); }
        for (unsigned i = 201; i <= 300; ++i) Require(encoder->Encode(makeFrame(i), nullptr) == 0, "Burst input rejected");
        {
            std::lock_guard lock(sink.mutex);
            sink.hold = false;
            sink.changed.notify_all();
        }
        sink.Wait(before + 4);
        Rate(*encoder, 400'000);
        Require(sink.images.size() == before + 4 && sink.images.back().RtpTimestamp() == 300,
            "Pending raw frame backlog was replayed");
        Require(sink.images.back().PlayoutDelay() == webrtc::VideoPlayoutDelay(webrtc::TimeDelta::Millis(10), webrtc::TimeDelta::Millis(10)), "Coalesced frame retained stale preset");
        encoder->Release();
        encoder->Release();
        Require(encoder->Encode(makeFrame(400), nullptr) == WEBRTC_VIDEO_CODEC_UNINITIALIZED, "Encoder active after release");
    }
    Require(!sink.timedOut && sink.dropped == 3 * (30 + 99), "Dropped-frame accounting or callback gate failed");
    Require(!hardware || (!hardware->quarantined && hardware->hardwareFrames == 12), "Hardware lifecycle lost frames or failed");
    Require(!gpuInput || hardware->device->readbackCount() == 0, "Owned GPU encoding performed CPU readback");
    std::cout << "MF encoder: zero-rate/resume, keyframe, 100-frame burst coalescing and three reset/release cycles passed.\n";
}
void StableFrameRate(bool useHardware) {
    auto hardware = useHardware ? std::make_shared<screenshare::media::MfHardwareSession>(
        std::make_shared<screenshare::media::D3dVideoDevice>()) : nullptr;
    screenshare::media::MfVideoEncoderFactory factory(hardware);
    auto encoder = factory.Create(webrtc::CreateEnvironment(), factory.GetSupportedFormats().front());
    Sink sink;
    struct Cleanup { webrtc::VideoEncoder& encoder; ~Cleanup() { encoder.Release(); } } cleanup{*encoder};
    webrtc::VideoCodec codec; codec.codecType = webrtc::kVideoCodecH264;
    codec.width = 640; codec.height = 360; codec.maxFramerate = 60; codec.startBitrate = 1000;
    Require(encoder->InitEncode(&codec, {webrtc::VideoEncoder::Capabilities(false), 2, 1200}) == 0,
        "Stable cadence initialization failed");
    encoder->RegisterEncodeCompleteCallback(&sink);
    unsigned timestamp = 0;
    for (double fps : {60.0, 59.0, 58.0, 60.0, 30.0, 29.0, 31.0}) {
        webrtc::VideoBitrateAllocation allocation; allocation.SetBitrate(0, 0, 1000000);
        encoder->SetRates({allocation, fps});
        Require(encoder->Encode(Frame(++timestamp), nullptr) == 0, "Stable cadence encode failed");
        sink.Wait(timestamp);
        // SetRates is a worker barrier; output cannot still be mutating here.
        encoder->SetRates({allocation, fps});
        const bool mustBeKey = timestamp == 1 || timestamp == 5;
        Require((sink.images.back()._frameType == webrtc::VideoFrameType::kVideoFrameKey) == mustBeKey,
            "FPS jitter restarted codec or real cadence change lost IDR");
    }
    Require(!useHardware || encoder->GetEncoderInfo().is_hardware_accelerated,
        "Stable cadence test unexpectedly fell back");
    std::cout << "Small FPS variation preserves codec history; 60-to-30 change emits IDR.\n";
}
void UnavailableFrameRate() {
    screenshare::media::MfVideoEncoderFactory factory;
    auto encoder = factory.Create(webrtc::CreateEnvironment(), factory.GetSupportedFormats().front());
    Sink sink;
    struct Cleanup { webrtc::VideoEncoder& encoder; ~Cleanup() { encoder.Release(); } } cleanup{*encoder};
    webrtc::VideoCodec codec;
    codec.codecType = webrtc::kVideoCodecH264;
    codec.width = 640; codec.height = 360; codec.maxFramerate = 30; codec.startBitrate = 1000;
    Require(encoder->InitEncode(&codec, {webrtc::VideoEncoder::Capabilities(false), 2, 1200}) == 0,
        "FPS fallback initialization failed");
    encoder->RegisterEncodeCompleteCallback(&sink);
    uint32_t timestamp = 1;
    for (double fps : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(), std::numeric_limits<double>::max(), 0.1}) {
        Rate(*encoder, 0);
        webrtc::VideoBitrateAllocation allocation; allocation.SetBitrate(0, 0, 400000);
        encoder->SetRates({allocation, fps});
        Require(encoder->Encode(Frame(timestamp), nullptr) == 0, "FPS fallback resume rejected");
        sink.Wait(timestamp);
        Rate(*encoder, 0); // Barrier before observing callback data.
        Require(sink.images.back().RtpTimestamp() == timestamp, "FPS fallback failed to resume current frame");
        ++timestamp;
    }
    std::cout << "MF encoder: missing/nonfinite/extreme FPS targets preserve bitrate resume.\n";
}
}
int main(int argc, char** argv) {
    try {
        bool hardware = false, gpuInput = false;
        int cycles = 1;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--hardware") hardware = true;
            else if (option == "--gpu-input") gpuInput = true;
            else if (option == "--cycles" && i + 1 < argc) {
                const std::string count = argv[++i]; size_t consumed = 0;
                cycles = std::stoi(count, &consumed);
                Require(consumed == count.size() && cycles >= 1 && cycles <= 100, "Invalid cycle count");
            } else throw std::runtime_error("Usage: MfEncoderAdapterTest [--hardware [--gpu-input]] [--cycles 1..100]");
        }
        Require(!gpuInput || hardware, "--gpu-input requires --hardware");
        if (!hardware) UnavailableFrameRate();
        StableFrameRate(hardware);
        proof::LifecycleSample(0, 0);
        for (int cycle = 1; cycle <= cycles; ++cycle) {
            const auto started = std::chrono::steady_clock::now();
            Run(hardware, gpuInput);
            proof::LifecycleSample(cycle, std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        }
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
