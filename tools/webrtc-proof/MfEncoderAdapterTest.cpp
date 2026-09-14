#include "LifecycleDiagnostics.h"
#include "media/webrtc/MfVideoEncoderFactory.h"
#include "media/webrtc/MfHardwareSession.h"
#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "common_video/h264/h264_common.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
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
    encoder.SetRates({allocation, bitrate ? 60.0 : 0.0});
}
void Run(bool useHardware) {
    auto hardware = useHardware ? std::make_shared<screenshare::media::MfHardwareSession>(
        std::make_shared<screenshare::media::D3dVideoDevice>()) : nullptr;
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
        Require(encoder->Encode(Frame(1), nullptr) == 0, "First encode failed");
        sink.Wait(before + 1);
        Rate(*encoder, 0); // Also a worker barrier.
        for (unsigned i = 0; i < 30; ++i) Require(encoder->Encode(Frame(2 + i), nullptr) == 0, "Suspended input rejected");
        Rate(*encoder, 400'000);
        Require(sink.images.size() == before + 1, "Zero-rate suspension emitted video");
        const std::vector types{webrtc::VideoFrameType::kVideoFrameKey};
        Require(encoder->Encode(Frame(100), &types) == 0, "Resume encode failed");
        sink.Wait(before + 2);
        Rate(*encoder, 400'000);
        Require(sink.images.back()._frameType == webrtc::VideoFrameType::kVideoFrameKey, "Keyframe request lost");
        bool foundSps = false;
        const auto& key = sink.images.back();
        for (const auto& nalu : webrtc::H264::FindNaluIndices(std::span(key.data(), key.size()))) {
            if (nalu.payload_size >= 4 && (key.data()[nalu.payload_start_offset] & 31) == 7) {
                const auto* sps = key.data() + nalu.payload_start_offset;
                Require(sps[1] == 100 && sps[3] <= 42, "Encoded SPS exceeds negotiated High level 4.2");
                foundSps = true;
            }
        }
        Require(foundSps, "Requested keyframe omitted SPS");
        Require(sink.images.back().RtpTimestamp() == 100 && sink.images.back().NtpTimeMs() == 123456,
            "Encoder timestamps lost");
        {
            std::lock_guard lock(sink.mutex);
            sink.hold = true;
        }
        Require(encoder->Encode(Frame(200), nullptr) == 0, "Burst initial encode failed");
        sink.Wait(before + 3); // Worker blocked in callback; all following input must coalesce.
        for (unsigned i = 201; i <= 300; ++i) Require(encoder->Encode(Frame(i), nullptr) == 0, "Burst input rejected");
        {
            std::lock_guard lock(sink.mutex);
            sink.hold = false;
            sink.changed.notify_all();
        }
        sink.Wait(before + 4);
        Rate(*encoder, 400'000);
        Require(sink.images.size() == before + 4 && sink.images.back().RtpTimestamp() == 300,
            "Pending raw frame backlog was replayed");
        encoder->Release();
        encoder->Release();
        Require(encoder->Encode(Frame(400), nullptr) == WEBRTC_VIDEO_CODEC_UNINITIALIZED, "Encoder active after release");
    }
    Require(!sink.timedOut && sink.dropped == 3 * (30 + 99), "Dropped-frame accounting or callback gate failed");
    Require(!hardware || (!hardware->quarantined && hardware->hardwareFrames == 12), "Hardware lifecycle lost frames or failed");
    std::cout << "MF encoder: zero-rate/resume, keyframe, 100-frame burst coalescing and three reset/release cycles passed.\n";
}
}
int main(int argc, char** argv) {
    try {
        bool hardware = false;
        int cycles = 1;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--hardware") hardware = true;
            else if (option == "--cycles" && i + 1 < argc) {
                const std::string count = argv[++i]; size_t consumed = 0;
                cycles = std::stoi(count, &consumed);
                Require(consumed == count.size() && cycles >= 1 && cycles <= 100, "Invalid cycle count");
            } else throw std::runtime_error("Usage: MfEncoderAdapterTest [--hardware] [--cycles 1..100]");
        }
        proof::LifecycleSample(0, 0);
        for (int cycle = 1; cycle <= cycles; ++cycle) {
            const auto started = std::chrono::steady_clock::now();
            Run(hardware);
            proof::LifecycleSample(cycle, std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        }
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
