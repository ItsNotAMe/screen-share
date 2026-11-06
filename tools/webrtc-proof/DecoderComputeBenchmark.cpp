#include "codec/H264StreamDecoder.h"
#include "libyuv/compare.h"
#include <windows.h>
#include <array>
#include <chrono>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <latch>
#include <thread>

namespace {
constexpr int width=1920,height=1080,poolSize=64,warmup=64;
void Check(bool ok) {if(!ok)throw std::runtime_error("Decoder benchmark invariant failed");}
double CpuMs() {
    FILETIME created{},exited{},kernel{},user{};
    Check(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user));
    auto ticks=[](FILETIME v){return (uint64_t(v.dwHighDateTime)<<32)|v.dwLowDateTime;};
    return double(ticks(kernel)+ticks(user))/10000.;
}
void Generate(const char* path) {
    screenshare::H264StreamEncoder encoder;screenshare::H264StreamEncoderConfig config;
    config.width=width;config.height=height;config.fps=60;config.bitrate=12000000;config.keyframeIntervalFrames=poolSize;
    encoder.Start(config);std::ofstream out(path,std::ios::binary);Check(bool(out));
    const std::array<uint32_t,4> header{0x31444353,width,height,poolSize};
    out.write(reinterpret_cast<const char*>(header.data()),sizeof(header));
    screenshare::CapturedFrame frame;frame.width=frame.sourceWidth=width;frame.height=frame.sourceHeight=height;
    frame.nv12Pixels.resize(size_t(width)*height*3/2);
    for(int n=0;n<poolSize;++n) {
        auto* p=reinterpret_cast<uint8_t*>(frame.nv12Pixels.data());
        for(int y=0;y<height;++y)for(int x=0;x<width;++x)p[y*width+x]=uint8_t(16+(x/8+y/4+n*7)%220);
        for(int y=0;y<height/2;++y)for(int x=0;x<width;x+=2){p[width*height+y*width+x]=uint8_t(80+(y/8+n)%50);p[width*height+y*width+x+1]=uint8_t(160+(x/16+n)%50);}
        const auto packets=encoder.EncodeFrame(frame);Check(packets.size()==1 && (n || packets[0].isKeyframe));
        const std::array<uint32_t,2> info{uint32_t(packets[0].bytes.size()),packets[0].isKeyframe?1u:0u};
        out.write(reinterpret_cast<const char*>(info.data()),sizeof(info));
        out.write(reinterpret_cast<const char*>(packets[0].bytes.data()),packets[0].bytes.size());Check(bool(out));
    }
}
std::vector<screenshare::EncodedPacket> Load(const char* path) {
    std::ifstream in(path,std::ios::binary);std::array<uint32_t,4> header{};
    in.read(reinterpret_cast<char*>(header.data()),sizeof(header));Check(in && header==std::array<uint32_t,4>{0x31444353,width,height,poolSize});
    std::vector<screenshare::EncodedPacket> pool(poolSize);
    for(auto& packet:pool) {
        std::array<uint32_t,2> info{};in.read(reinterpret_cast<char*>(info.data()),sizeof(info));
        Check(in && info[0]>0 && info[0]<=16*1024*1024 && info[1]<=1);
        packet.isKeyframe=info[1];packet.bytes.resize(info[0]);packet.duration100ns=166667;
        in.read(reinterpret_cast<char*>(packet.bytes.data()),packet.bytes.size());Check(bool(in));
    }
    Check(pool[0].isKeyframe && in.peek()==std::char_traits<char>::eof());return pool;
}
struct Result {std::exception_ptr error;uint64_t frames=0,bytes=0,hash=14695981039346656037ull,allocations=0,warmAllocations=0;};
}
int main(int argc,char** argv) try {
    if(argc==3 && std::string(argv[1])=="--generate"){Generate(argv[2]);return 0;}
    Check(argc==4);const auto pool=Load(argv[1]);const int viewers=std::stoi(argv[2]),measured=std::stoi(argv[3]);
    Check((viewers==1 || viewers==4) && measured>=30 && measured<=3600);
    std::latch ready(viewers),start(1),done(viewers),release(1);
    std::array<Result,4> results;std::vector<std::thread> workers;
    for(int v=0;v<viewers;++v)workers.emplace_back([&,v] {
        auto& result=results[v];bool announced=false;
        try {
            screenshare::H264StreamDecoder decoder;decoder.Start(4096,4096);
            auto packets=pool;unsigned delivered=0;
            auto consume=[&](auto output) {
                for(const auto& frame:output) {
                    Check(frame.width==width && frame.height==height && frame.timestamp100ns==int64_t(delivered+1)*166667 &&
                        !frame.texture && frame.data.size()==size_t(width)*height*3/2);
                    if(delivered++<warmup)continue;
                    ++result.frames;result.bytes+=frame.data.size();
                    result.hash^=libyuv::HashDjb2(reinterpret_cast<const uint8_t*>(frame.data.data()),frame.data.size(),5381);
                    result.hash*=1099511628211ull;result.hash^=uint64_t(frame.timestamp100ns);result.hash*=1099511628211ull;
                }
            };
            auto step=[&](int i){auto& packet=packets[i%poolSize];packet.timestamp100ns=int64_t(i+1)*166667;consume(decoder.DecodePacket(packet));};
            for(int i=0;i<warmup;++i)step(i);
            result.warmAllocations=decoder.outputBufferAllocations();ready.count_down();announced=true;start.wait();
            for(int i=warmup;i<warmup+measured;++i)step(i);
            consume(decoder.Drain());Check(delivered==unsigned(warmup+measured) && result.frames==unsigned(measured));
            result.allocations=decoder.outputBufferAllocations();done.count_down();release.wait();
        } catch(...) {result.error=std::current_exception();if(!announced)ready.count_down();done.count_down();}
    });
    ready.wait();const auto cpu=CpuMs();const auto began=std::chrono::steady_clock::now();start.count_down();done.wait();
    const auto wall=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count(),elapsedCpu=CpuMs()-cpu;
    release.count_down();for(auto& worker:workers)worker.join();
    uint64_t decoded=0,bytes=0,hash=14695981039346656037ull,allocations=0,measuredAllocations=0;
    for(int v=0;v<viewers;++v){const auto& r=results[v];if(r.error)std::rethrow_exception(r.error);
        decoded+=r.frames;bytes+=r.bytes;hash^=r.hash;hash*=1099511628211ull;allocations+=r.allocations;measuredAllocations+=r.allocations-r.warmAllocations;}
    std::cout<<std::fixed<<std::setprecision(6)<<"{\"viewers\":"<<viewers<<",\"inputFrames\":"<<measured
        <<",\"decodedFrames\":"<<decoded<<",\"cpuMs\":"<<elapsedCpu<<",\"wallMs\":"<<wall
        <<",\"cpuMsPerDecodedFrame\":"<<elapsedCpu/decoded<<",\"decodedBytes\":"<<bytes<<",\"outputAllocations\":"<<allocations
        <<",\"measuredOutputAllocations\":"<<measuredAllocations<<",\"outputHash\":\""<<std::hex<<hash<<"\"}\n";
    return 0;
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
