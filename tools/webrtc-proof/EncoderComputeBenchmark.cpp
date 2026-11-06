#include "media/webrtc/MfVideoEncoderFactory.h"
#include "media/webrtc/D3dVideoFrameBuffer.h"
#include "media/webrtc/MappedVideoBuffer.h"
#include "api/environment/environment_factory.h"
#include "api/video/nv12_buffer.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
void Check(bool ok) { if (!ok) throw std::runtime_error("Encoder benchmark invariant failed"); }
double CpuMilliseconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    Check(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user));
    auto ticks=[](FILETIME value) { return (uint64_t(value.dwHighDateTime)<<32)|value.dwLowDateTime; };
    return double(ticks(kernel)+ticks(user))/10000.0;
}
class Sink final : public webrtc::EncodedImageCallback {
public:
    Result OnEncodedImage(const webrtc::EncodedImage& image, const webrtc::CodecSpecificInfo*) override {
        std::lock_guard lock(mutex);
        Check(image.RtpTimestamp()==frames+1);
        ++frames;bytes+=image.size();
        for(size_t i=0;i<image.size();++i){hash^=image.data()[i];hash*=1099511628211ull;}
        changed.notify_one();return Result(Result::OK);
    }
    void OnFrameDropped(uint32_t,int,bool) override { std::lock_guard lock(mutex);++dropped; }
    void Wait(unsigned count) {
        std::unique_lock lock(mutex);
        Check(changed.wait_for(lock,std::chrono::seconds(10),[&]{return frames>=count;}));
    }
    void Begin() { std::lock_guard lock(mutex);bytes=0;hash=14695981039346656037ull; }
    std::mutex mutex;std::condition_variable changed;
    unsigned frames=0,dropped=0;uint64_t bytes=0,hash=14695981039346656037ull;
};
}
int main(int argc,char** argv) try {
    Check(argc==4);
    const std::string mode=argv[1];const int viewers=std::stoi(argv[2]), measured=std::stoi(argv[3]);
    Check((mode=="cpu" || mode=="gpu") && (viewers==1 || viewers==4) && measured>=30 && measured<=3600);
    constexpr int width=1920,height=1080,warmup=30,poolSize=16;
    std::vector<webrtc::scoped_refptr<webrtc::VideoFrameBuffer>> pool;
    auto gpu=mode=="gpu"?std::make_shared<screenshare::media::D3dVideoDevice>():nullptr;
    for(int n=0;n<poolSize;++n) {
        auto pixels=webrtc::NV12Buffer::Create(width,height);
        for(int y=0;y<height;++y)for(int x=0;x<width;++x)
            pixels->MutableDataY()[y*pixels->StrideY()+x]=uint8_t(16+(x/8+y/4+n*7)%220);
        for(int y=0;y<height/2;++y)for(int x=0;x<width;x+=2) {
            pixels->MutableDataUV()[y*pixels->StrideUV()+x]=uint8_t(80+(y/8+n)%50);
            pixels->MutableDataUV()[y*pixels->StrideUV()+x+1]=uint8_t(160+(x/16+n)%50);
        }
        if(gpu) {
            std::vector<uint8_t> packed(size_t(width)*height*3/2);
            for(int y=0;y<height;++y)std::copy_n(pixels->DataY()+y*pixels->StrideY(),width,packed.data()+y*width);
            for(int y=0;y<height/2;++y)std::copy_n(pixels->DataUV()+y*pixels->StrideUV(),width,packed.data()+width*height+y*width);
            pool.push_back(gpu->UploadNv12(width,height,packed));
        } else pool.push_back(pixels);
    }
    std::array<Sink,4> sinks;
    std::array<std::unique_ptr<webrtc::VideoEncoder>,4> encoders;
    screenshare::media::MfVideoEncoderFactory factory;
    webrtc::VideoCodec codec;codec.codecType=webrtc::kVideoCodecH264;
    codec.width=width;codec.height=height;codec.maxFramerate=60;codec.startBitrate=12000;
    for(int i=0;i<viewers;++i) {
        encoders[i]=factory.Create(webrtc::CreateEnvironment(),factory.GetSupportedFormats().front());
        Check(encoders[i]->InitEncode(&codec,{webrtc::VideoEncoder::Capabilities(false),2,1200})==0);
        encoders[i]->RegisterEncodeCompleteCallback(&sinks[i]);
        Check(!encoders[i]->GetEncoderInfo().is_hardware_accelerated);
    }
    auto barrier=[&] {
        webrtc::VideoBitrateAllocation bitrate;bitrate.SetBitrate(0,0,12000000);
        for(int i=0;i<viewers;++i)encoders[i]->SetRates({bitrate,60.0});
    };
    auto batch=[&](unsigned index) {
        auto input=pool[index%poolSize];
        if(gpu) {
            // Fresh wrapper per acquired frame: never benchmark a readback
            // cache accidentally warmed for the entire texture pool.
            auto retained=static_cast<screenshare::media::D3dVideoFrameBuffer*>(input.get())->RetainedNv12();
            input=webrtc::make_ref_counted<screenshare::media::D3dVideoFrameBuffer>(gpu,retained.nv12Texture,width,height);
        }
        auto frame=webrtc::VideoFrame::Builder().set_video_frame_buffer(screenshare::media::WithMapping(input,
            {17,width,height,0,0,width,height},screenshare::media::StreamPreset::Gaming))
            .set_rtp_timestamp(index+1).set_timestamp_us(int64_t(index+1)*16667).set_ntp_time_ms(123456).build();
        for(int i=0;i<viewers;++i)Check(encoders[i]->Encode(frame,nullptr)==0);
        for(int i=0;i<viewers;++i)sinks[i].Wait(index+1);
    };
    for(unsigned i=0;i<warmup;++i)batch(i);
    barrier();for(int i=0;i<viewers;++i)sinks[i].Begin();
    const auto readbacks=gpu?gpu->readbackCount():0;
    const auto cpuStart=CpuMilliseconds();const auto started=Clock::now();
    for(unsigned i=warmup;i<unsigned(warmup+measured);++i)batch(i);
    barrier();
    const auto wallMs=std::chrono::duration<double,std::milli>(Clock::now()-started).count();
    const auto cpuMs=CpuMilliseconds()-cpuStart;
    uint64_t bytes=0,hash=14695981039346656037ull;
    for(int i=0;i<viewers;++i){Check(!sinks[i].dropped && sinks[i].frames==unsigned(warmup+measured));bytes+=sinks[i].bytes;hash^=sinks[i].hash;hash*=1099511628211ull;}
    Check(!gpu || gpu->readbackCount()-readbacks==uint64_t(measured));
    std::cout<<std::fixed<<std::setprecision(6)<<"{\"mode\":\""<<mode<<"\",\"viewers\":"<<viewers
        <<",\"inputFrames\":"<<measured<<",\"encodedFrames\":"<<measured*viewers<<",\"cpuMs\":"<<cpuMs
        <<",\"wallMs\":"<<wallMs<<",\"cpuMsPerEncodedFrame\":"<<cpuMs/(measured*viewers)
        <<",\"encodedBytes\":"<<bytes<<",\"outputHash\":\""<<std::hex<<hash<<"\"}\n";
    return 0;
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
