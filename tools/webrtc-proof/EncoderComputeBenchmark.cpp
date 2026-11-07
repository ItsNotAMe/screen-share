#include "media/webrtc/MfVideoEncoderFactory.h"
#include "media/webrtc/D3dVideoFrameBuffer.h"
#include "media/webrtc/MappedVideoBuffer.h"
#include "api/environment/environment_factory.h"
#include "api/video/nv12_buffer.h"
#include "codec/H264StreamDecoder.h"
#include "libyuv/compare.h"
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr int width=1920,height=1080,warmup=30,poolSize=16,qualityFrames=16;
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
        if(frames<=warmup+qualityFrames)qualityImages.push_back(image);
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
    std::vector<webrtc::EncodedImage> qualityImages;
};
struct Quality { double yError=0,uvError=0,minY=100;unsigned frames=0; };
double Psnr(double error,double count) { return error?10*std::log10(255.*255.*count/error):100.; }
Quality CheckQuality(const Sink& sink,const std::vector<webrtc::scoped_refptr<webrtc::NV12Buffer>>& references) {
    Check(sink.qualityImages.size()==warmup+qualityFrames);
    screenshare::H264StreamDecoder decoder;decoder.Start(4096,4096);Quality quality;
    for(unsigned i=0;i<sink.qualityImages.size();++i) {
        const auto& image=sink.qualityImages[i];screenshare::EncodedPacket packet;
        packet.timestamp100ns=int64_t(i+1)*166667;packet.duration100ns=166667;
        packet.bytes.assign(reinterpret_cast<const std::byte*>(image.data()),reinterpret_cast<const std::byte*>(image.data()+image.size()));
        const auto output=decoder.DecodePacket(packet);
        Check(output.size()==1 && output[0].width==width && output[0].height==height && output[0].timestamp100ns==packet.timestamp100ns);
        if(i<warmup)continue;
        const auto& reference=references[i%poolSize];const auto* decoded=reinterpret_cast<const uint8_t*>(output[0].data.data());
        Check(output[0].data.size()==size_t(width)*height*3/2);
        const auto y=libyuv::ComputeSumSquareErrorPlane(reference->DataY(),reference->StrideY(),decoded,width,width,height);
        const auto uv=libyuv::ComputeSumSquareErrorPlane(reference->DataUV(),reference->StrideUV(),decoded+width*height,width,width,height/2);
        quality.yError+=double(y);quality.uvError+=double(uv);quality.minY=std::min(quality.minY,Psnr(double(y),width*height));++quality.frames;
    }
    Check(decoder.Drain().empty());return quality;
}
unsigned ThreadCount() {
    const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);Check(snapshot!=INVALID_HANDLE_VALUE);
    THREADENTRY32 entry{};entry.dwSize=sizeof(entry);unsigned count=0;
    if(Thread32First(snapshot,&entry))do { if(entry.th32OwnerProcessID==GetCurrentProcessId())++count; }while(Thread32Next(snapshot,&entry));
    CloseHandle(snapshot);return count;
}
}
int main(int argc,char** argv) try {
    Check(argc==4 || (argc==5 && std::string(argv[4])=="detailed"));
    const bool detailed=argc==5;
    const std::string mode=argv[1];const int viewers=std::stoi(argv[2]), measured=std::stoi(argv[3]);
    Check((mode=="cpu" || mode=="gpu") && (viewers==1 || viewers==4) && measured>=30 && measured<=3600);
    std::vector<webrtc::scoped_refptr<webrtc::VideoFrameBuffer>> pool;
    std::vector<webrtc::scoped_refptr<webrtc::NV12Buffer>> references;
    auto gpu=mode=="gpu"?std::make_shared<screenshare::media::D3dVideoDevice>():nullptr;
    for(int n=0;n<poolSize;++n) {
        auto pixels=webrtc::NV12Buffer::Create(width,height);
        for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
            uint32_t pattern=uint32_t((x+n*5)/4)*0x9e3779b9u ^ uint32_t((y+n*3)/4)*0x85ebca6bu;
            pattern^=pattern>>16;pattern*=0x7feb352du;pattern^=pattern>>15;
            pixels->MutableDataY()[y*pixels->StrideY()+x]=detailed?uint8_t(16+(pattern>>24)*220/256):uint8_t(16+(x/8+y/4+n*7)%220);
        }
        for(int y=0;y<height/2;++y)for(int x=0;x<width;x+=2) {
            pixels->MutableDataUV()[y*pixels->StrideUV()+x]=uint8_t(80+(y/8+n)%50);
            pixels->MutableDataUV()[y*pixels->StrideUV()+x+1]=uint8_t(160+(x/16+n)%50);
        }
        references.push_back(pixels);
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
        Check(encoders[i]->InitEncode(&codec,{webrtc::VideoEncoder::Capabilities(false),16,1200})==0);
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
    std::vector<double> batches;batches.reserve(measured);
    const auto cpuStart=CpuMilliseconds();const auto started=Clock::now();
    for(unsigned i=warmup;i<unsigned(warmup+measured);++i) {
        const auto began=Clock::now();batch(i);
        batches.push_back(std::chrono::duration<double,std::milli>(Clock::now()-began).count());
    }
    barrier();
    const auto wallMs=std::chrono::duration<double,std::milli>(Clock::now()-started).count();
    const auto cpuMs=CpuMilliseconds()-cpuStart;
    std::sort(batches.begin(),batches.end());
    PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
    Check(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)));
    const auto threads=ThreadCount();
    uint64_t bytes=0,hash=14695981039346656037ull;
    for(int i=0;i<viewers;++i){Check(!sinks[i].dropped && sinks[i].frames==unsigned(warmup+measured));bytes+=sinks[i].bytes;hash^=sinks[i].hash;hash*=1099511628211ull;}
    Check(!gpu || gpu->readbackCount()-readbacks==uint64_t(measured));
    // Decode/quality validation is outside the CPU/elapsed measurement. Retain
    // only warmup plus one changing input cycle, never the entire timed run.
    Quality quality;
    for(int i=0;i<viewers;++i) {
        const auto q=CheckQuality(sinks[i],references);quality.yError+=q.yError;quality.uvError+=q.uvError;
        quality.minY=std::min(quality.minY,q.minY);quality.frames+=q.frames;
    }
    std::cout<<std::fixed<<std::setprecision(6)<<"{\"mode\":\""<<mode<<"\",\"viewers\":"<<viewers
        <<",\"detailed\":"<<(detailed?"true":"false")<<",\"inputFrames\":"<<measured<<",\"encodedFrames\":"<<measured*viewers<<",\"cpuMs\":"<<cpuMs
        <<",\"wallMs\":"<<wallMs<<",\"cpuMsPerEncodedFrame\":"<<cpuMs/(measured*viewers)
        <<",\"encodedBytes\":"<<bytes<<",\"privateBytes\":"<<memory.PrivateUsage<<",\"processThreads\":"<<threads
        <<",\"batchP95Ms\":"<<batches[size_t((batches.size()-1)*.95)]<<",\"batchP99Ms\":"<<batches[size_t((batches.size()-1)*.99)]
        <<",\"qualityFrames\":"<<quality.frames<<",\"lumaPsnrDb\":"<<Psnr(quality.yError,double(width)*height*quality.frames)
        <<",\"chromaPsnrDb\":"<<Psnr(quality.uvError,double(width)*height/2*quality.frames)<<",\"minimumLumaPsnrDb\":"<<quality.minY
        <<",\"outputHash\":\""<<std::hex<<hash<<"\"}\n";
    return 0;
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
