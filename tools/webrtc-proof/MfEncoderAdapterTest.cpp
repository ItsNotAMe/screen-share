#include "LifecycleDiagnostics.h"
#include "media/webrtc/MfVideoEncoderFactory.h"
#include "media/webrtc/MfHardwareSession.h"
#include "media/webrtc/MappedVideoBuffer.h"
#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video/nv12_buffer.h"
#include "codec/H264StreamDecoder.h"
#include "codec/InputMappingSei.h"
#include "../../frontend/shared/LatestRoomVideoFrame.h"
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
#include <array>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <future>
#include <thread>

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
class CountedNv12 : public webrtc::NV12BufferInterface {
    webrtc::scoped_refptr<webrtc::NV12Buffer> pixels_;
    std::atomic<unsigned>& conversions_;
public:
    CountedNv12(webrtc::scoped_refptr<webrtc::NV12Buffer> pixels, std::atomic<unsigned>& conversions)
        : pixels_(std::move(pixels)), conversions_(conversions) {}
    int width() const override { return pixels_->width(); }
    int height() const override { return pixels_->height(); }
    int StrideY() const override { return pixels_->StrideY(); }
    int StrideUV() const override { return pixels_->StrideUV(); }
    const uint8_t* DataY() const override { return pixels_->DataY(); }
    const uint8_t* DataUV() const override { return pixels_->DataUV(); }
    webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override { ++conversions_; return pixels_->ToI420(); }
};
void Nv12PlaneValidation() {
    screenshare::H264StreamEncoder encoder;
    bool unavailable=false;try{encoder.EncodeNv12({});}catch(const std::logic_error&){unavailable=true;}
    Require(unavailable,"NV12 planes accepted before encoder startup");
    screenshare::H264StreamEncoderConfig config;config.width=640;config.height=360;encoder.Start(config);
    // Last rows have no trailing padding. Only visible bytes are required.
    std::vector<uint8_t> y(672*359+640,80),uv(704*179+640,128);
    const screenshare::Nv12FrameView valid{640,360,y,uv,672,704};
    for(int failure=0;failure<7;++failure) {
        auto bad=valid;
        if(failure==0)bad.y=bad.y.first(bad.y.size()-1);
        if(failure==1)bad.uv=bad.uv.first(bad.uv.size()-1);
        if(failure==2)bad.strideY=-1;
        if(failure==3)bad.strideUv=638;
        if(failure==4)bad.width=638;
        if(failure==5)bad.height=359;
        if(failure==6)bad.y={};
        bool rejected=false;try{encoder.EncodeNv12(bad);}catch(const std::invalid_argument&){rejected=true;}
        Require(rejected,"Malformed NV12 planes were accepted");
    }
    auto packets=encoder.EncodeNv12(valid);
    Require(packets.size()==1 && packets.front().timestamp100ns==0 && packets.front().isKeyframe,
        "NV12 validation consumed frame state or minimal padded planes failed");
    encoder.Stop();unavailable=false;
    try{encoder.EncodeNv12(valid);}catch(const std::logic_error&){unavailable=true;}
    Require(unavailable,"NV12 planes accepted after Stop");
}
void DirectNv12Input(bool gpuInput=false) {
    using namespace screenshare::media;
    MfVideoEncoderFactory factory;
    std::array<Sink,4> sinks;
    std::atomic<unsigned> conversions{0};
    auto gpu=gpuInput?std::make_shared<D3dVideoDevice>():nullptr;
    // Callback owners and counters outlive every encoder, including exceptions.
    std::array<std::unique_ptr<webrtc::VideoEncoder>,4> encoders;
    std::array<screenshare::H264StreamDecoder,4> decoders;
    webrtc::VideoCodec codec;codec.codecType=webrtc::kVideoCodecH264;
    codec.width=640;codec.height=360;codec.maxFramerate=30;codec.startBitrate=1000;
    for(size_t i=0;i<encoders.size();++i) {
        encoders[i]=factory.Create(webrtc::CreateEnvironment(),factory.GetSupportedFormats().front());
        Require(encoders[i]->InitEncode(&codec,{webrtc::VideoEncoder::Capabilities(false),2,1200})==0,"NV12 encoder startup failed");
        encoders[i]->RegisterEncodeCompleteCallback(&sinks[i]);decoders[i].Start(4096,4096);
    }
    for(unsigned index=0;index<8;++index) {
        // Unequal padded plane strides catch accidental tight-packing assumptions.
        auto pixels=webrtc::NV12Buffer::Create(640,360,672,704);
        for(int y=0;y<360;++y) {
            std::fill_n(pixels->MutableDataY()+y*pixels->StrideY(),pixels->StrideY(),uint8_t(255));
            std::fill_n(pixels->MutableDataY()+y*pixels->StrideY(),320,uint8_t(70+index*3));
            std::fill_n(pixels->MutableDataY()+y*pixels->StrideY()+320,320,uint8_t(130+index*3));
        }
        for(int y=0;y<180;++y) {
            auto* row=pixels->MutableDataUV()+y*pixels->StrideUV();std::fill_n(row,pixels->StrideUV(),uint8_t(255));
            for(int x=0;x<640;x+=2){row[x]=uint8_t(90+index);row[x+1]=uint8_t(170-index);}
        }
        auto counted=webrtc::make_ref_counted<CountedNv12>(pixels,conversions);
        webrtc::scoped_refptr<webrtc::VideoFrameBuffer> input=counted;
        if(gpu) {
            std::vector<uint8_t> packed(640*360*3/2);
            for(int y=0;y<360;++y)std::copy_n(pixels->DataY()+y*pixels->StrideY(),640,packed.data()+y*640);
            for(int y=0;y<180;++y)std::copy_n(pixels->DataUV()+y*pixels->StrideUV(),640,packed.data()+640*360+y*640);
            input=gpu->UploadNv12(640,360,packed);
        }
        for(size_t i=0;i<encoders.size();++i) {
            auto buffer=WithMapping(input,{17,640,360,0,0,640,360},i%2?StreamPreset::Quality:StreamPreset::Gaming);
            auto frame=webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer).set_rtp_timestamp(index+1)
                .set_timestamp_us(int64_t(index+1)*33333).set_ntp_time_ms(123456).build();
            Require(encoders[i]->Encode(frame,nullptr)==0,"NV12 input rejected");
        }
        for(size_t i=0;i<encoders.size();++i) {
            sinks[i].Wait(index+1);Rate(*encoders[i],1000000); // Owner-worker barrier.
            const auto& image=sinks[i].images.back();
            Require(image.RtpTimestamp()==index+1 && image.NtpTimeMs()==123456,"NV12 timestamps lost");
            Require(screenshare::input::ReadMappingSei({image.data(),image.size()}).generation==17,"NV12 frame mapping lost");
            screenshare::EncodedPacket packet;packet.timestamp100ns=(index+1)*333333;
            packet.duration100ns=333333;packet.bytes.resize(image.size());std::memcpy(packet.bytes.data(),image.data(),image.size());
            auto decoded=decoders[i].DecodePacket(packet);
            Require(decoded.size()==1 && decoded.front().width==640 && decoded.front().height==360,"NV12 decode delayed or resized");
            const auto* bytes=reinterpret_cast<const uint8_t*>(decoded.front().data.data());
            Require(decoded.front().data.size()==640*360*3/2 && std::abs(int(bytes[100*640+100])-int(70+index*3))<=4 &&
                std::abs(int(bytes[100*640+500])-int(130+index*3))<=4 &&
                std::abs(int(bytes[640*360+50*640+100])-int(90+index))<=4 &&
                std::abs(int(bytes[640*360+50*640+101])-int(170-index))<=4,"NV12 stride, chroma or frame freshness changed");
        }
    }
    std::cout<<"Four software encoders, 32 "<<(gpu?"GPU":"CPU")<<" NV12 frames: ToI420 calls="
        <<(gpu?gpu->i420ConversionCount():conversions.load())<<'\n';
    Require(conversions==0,"Software encoding performed an unnecessary NV12/I420 round trip");
    if(gpu) {
        Require(gpu->readbackCount()==8 && gpu->i420ConversionCount()==0,"Shared GPU input repeated readback or converted through I420");
        std::vector<uint8_t> probePixels(640*360*3/2,128);std::fill_n(probePixels.begin(),640*360,uint8_t(91));
        auto probe=gpu->UploadNv12(640,360,probePixels);
        std::array requested{webrtc::VideoFrameBuffer::Type::kNV12};
        auto mapped=probe->GetMappedFrameBuffer(requested);
        Require(mapped && mapped->type()==webrtc::VideoFrameBuffer::Type::kNV12 && mapped->GetNV12()->DataY()[0]==91,
            "GPU NV12 mapping unavailable or changed pixels");
        LatestRoomVideoFrame latest;latest.OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(probe).build());
        auto retained=latest.Take();latest.Stop();
        Require(retained && retained->pixels().size()==probePixels.size() && retained->pixels()[0]==91 &&
            gpu->readbackCount()==9 && gpu->i420ConversionCount()==0,"Preview CPU pixels performed a redundant conversion/readback");
        std::array planarFirst{webrtc::VideoFrameBuffer::Type::kI420,webrtc::VideoFrameBuffer::Type::kNV12};
        auto planar=probe->GetMappedFrameBuffer(planarFirst);
        Require(planar && planar->type()==webrtc::VideoFrameBuffer::Type::kI420 && probe->ToI420().get()==planar.get() &&
            probe->ToNv12().get()==mapped.get() && gpu->readbackCount()==9 && gpu->i420ConversionCount()==1,
            "Explicit I420 mapping lost format preference or cache ownership");
        auto planarProbe=gpu->UploadNv12(640,360,probePixels);
        auto firstPlanar=planarProbe->ToI420();auto secondNv12=planarProbe->ToNv12();
        Require(firstPlanar && secondNv12 && secondNv12->DataY()[0]==91 && secondNv12->DataUV()[0]==128 &&
            planarProbe->ToI420().get()==firstPlanar.get() && gpu->readbackCount()==10 && gpu->i420ConversionCount()==2,
            "I420-first mapping repeated GPU readback or changed pixels");
        gpu->Retire();
        Require(!probe->ToNv12() && !probe->ToI420() && !probe->GetMappedFrameBuffer(requested) &&
            mapped->GetNV12()->DataY()[0]==91,"Retired GPU mapping reopened or invalidated owned CPU pixels");
    }
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

// Model a completed software-input/readback stall without physical capture or
// a transform test hook. The real encoder's elapsed-work limit includes mapping.
class StalledInput : public webrtc::VideoFrameBuffer {
public:
    explicit StalledInput(std::shared_ptr<std::promise<void>> entered) : entered_(std::move(entered)), pixels_(webrtc::I420Buffer::Create(640, 360)) {
        pixels_->InitializeData();
    }
    Type type() const override { return Type::kNative; }
    int width() const override { return 640; }
    int height() const override { return 360; }
    webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override {
        entered_->set_value();
        std::this_thread::sleep_for(std::chrono::milliseconds(550));
        return pixels_;
    }
private:
    std::shared_ptr<std::promise<void>> entered_;
    webrtc::scoped_refptr<webrtc::I420Buffer> pixels_;
};

void CompletedStallRecovery() {
    using namespace screenshare::media;
    auto diagnostics = std::make_shared<DiagnosticHistory>();
    MfVideoEncoderFactory factory({}, diagnostics);
    auto encoder = factory.Create(webrtc::CreateEnvironment(), factory.GetSupportedFormats().front());
    Sink sink;
    struct Cleanup { webrtc::VideoEncoder& encoder; ~Cleanup() { encoder.Release(); } } cleanup{*encoder};
    webrtc::VideoCodec codec;
    codec.codecType = webrtc::kVideoCodecH264;
    codec.width = 640; codec.height = 360; codec.maxFramerate = 30; codec.startBitrate = 1000;
    Require(encoder->InitEncode(&codec, {webrtc::VideoEncoder::Capabilities(false), 2, 1200}) == 0,
        "Completed-stall initialization failed");
    encoder->RegisterEncodeCompleteCallback(&sink);
    Require(encoder->Encode(Frame(1), nullptr) == 0, "Initial stall fixture frame rejected");
    sink.Wait(1);

    auto stall = [&](uint32_t timestamp, std::shared_ptr<std::promise<void>> entered) {
        auto frame = Frame(timestamp);
        frame.set_video_frame_buffer(webrtc::make_ref_counted<StalledInput>(entered));
        Require(encoder->Encode(frame, nullptr) == 0, "Stalled input rejected");
    };
    for (unsigned gap = 0; gap < 3; ++gap) {
        auto entered = std::make_shared<std::promise<void>>();
        auto started = entered->get_future();
        const uint32_t lateTimestamp = 100 + gap * 100;
        stall(lateTimestamp, entered);
        Require(started.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "Input stall never started");

        const uint32_t freshTimestamp = lateTimestamp + 10;
        if (!gap) {
            // Same rates/FPS: a worker barrier, not an encoder restart. A fresh
            // Encode call must be accepted after the late picture completed.
            Rate(*encoder, 1000000);
        } else {
            for (unsigned i = 1; i < 10; ++i)
                Require(encoder->Encode(Frame(lateTimestamp + i), nullptr) == 0, "Pending stall input rejected");
        }
        auto fresh = Frame(freshTimestamp);
        fresh.set_video_frame_buffer(WithMapping(fresh.video_frame_buffer(),
            {freshTimestamp, 640, 360, 0, 0, 640, 360}, StreamPreset::Gaming));
        Require(encoder->Encode(fresh, nullptr) == 0, "Completed software stall poisoned subsequent encoding");
        sink.Wait(gap + 2);
        Rate(*encoder, 1000000);
        const auto& image = sink.images.back();
        Require(sink.images.size() == gap + 2 && image.RtpTimestamp() == freshTimestamp && image.NtpTimeMs() == 123456 &&
            image._frameType == webrtc::VideoFrameType::kVideoFrameKey &&
            screenshare::input::ReadMappingSei({image.data(), image.size()}).generation == freshTimestamp,
            "Stall recovery replayed old output or lost fresh IDR/timing/mapping");
        // A new decoder must reconstruct the recovery picture without the
        // dropped picture's reference chain, including parameter sets.
        screenshare::H264StreamDecoder decoder;
        decoder.Start(4096, 4096);
        screenshare::EncodedPacket packet;
        packet.timestamp100ns = freshTimestamp * 100000;
        packet.duration100ns = 333333;
        packet.bytes.resize(image.size());
        std::memcpy(packet.bytes.data(), image.data(), image.size());
        const auto decoded = decoder.DecodePacket(packet);
        Require(decoded.size() == 1 && decoded.front().width == 640 && decoded.front().height == 360,
            "Fresh recovery keyframe depended on dropped output");
    }
    Require(sink.dropped == 21, "Late or replaced input was not accounted for exactly once");

    auto entered = std::make_shared<std::promise<void>>();
    auto started = entered->get_future();
    stall(900, entered);
    Require(started.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "Release fixture did not enter stall");
    encoder->Release();
    Require(sink.images.size() == 4 && sink.dropped == 21,
        "Cancelled late output emitted a callback or changed drop accounting");
    Require(encoder->Encode(Frame(901), nullptr) == WEBRTC_VIDEO_CODEC_UNINITIALIZED,
        "Cancelled late output revived a released encoder");
    Require(encoder->InitEncode(&codec, {webrtc::VideoEncoder::Capabilities(false), 2, 1200}) == 0,
        "Encoder could not restart after cancelling late output");
    encoder->RegisterEncodeCompleteCallback(&sink);
    Require(encoder->Encode(Frame(902), nullptr) == 0, "Restarted input rejected after cancellation");
    sink.Wait(5);
    Rate(*encoder, 1000000);
    Require(sink.images.size() == 5 && sink.images.back().RtpTimestamp() == 902 &&
        sink.images.back()._frameType == webrtc::VideoFrameType::kVideoFrameKey,
        "New configuration emitted stale output or lost its initial IDR");

    unsigned lateEvents = 0, starts = 0;
    for (const auto& record : *diagnostics->Read().records) {
        const auto event = record.labels.find("event");
        if (event == record.labels.end()) continue;
        Require(event->second != "encoder-terminal-failure" && event->second != "encoder-frame-failed",
            "Completed stall recorded a fatal encoder failure");
        if (event->second == "encoder-late-frame-dropped") {
            ++lateEvents;
            Require(record.numbers.at("code") >= 500, "Late-output diagnostics lost elapsed work time");
        }
        if (event->second == "encoder-software-ready") ++starts;
    }
    Require(lateEvents == 3 && starts == 2, "Late-output diagnostics missing or stall recovery rebuilt the codec");
    std::cout << "Completed software stalls: three late drops, fresh independent IDRs, bounded pending replacement and cancellation/restart passed.\n";
}
}
int main(int argc, char** argv) {
    try {
        bool hardware = false, gpuInput = false, gpuReadback = false;
        int cycles = 1;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--hardware") hardware = true;
            else if (option == "--gpu-input") gpuInput = true;
            else if (option == "--gpu-readback") gpuReadback = true;
            else if (option == "--cycles" && i + 1 < argc) {
                const std::string count = argv[++i]; size_t consumed = 0;
                cycles = std::stoi(count, &consumed);
                Require(consumed == count.size() && cycles >= 1 && cycles <= 100, "Invalid cycle count");
            } else throw std::runtime_error("Usage: MfEncoderAdapterTest [--gpu-readback | --hardware [--gpu-input]] [--cycles 1..100]");
        }
        Require(!gpuInput || hardware, "--gpu-input requires --hardware");
        Require(!gpuReadback || !hardware,"--gpu-readback requires software encoding");
        if (!hardware) { Nv12PlaneValidation();DirectNv12Input();if(gpuReadback)DirectNv12Input(true);UnavailableFrameRate();CompletedStallRecovery(); }
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
