#pragma once
#include "media/audio/RoutedPcmCapture.h"
#include "media/audio/SourcePcmCapture.h"
#include "media/audio/DefaultAudioDeviceNotification.h"
#include "media/audio/WasapiPcmEndpoint.h"
#include <algorithm>
#include <source_location>
#include <thread>

namespace routed_capture_test {
using namespace screenshare::media;
inline void Check(bool ok,std::source_location at=std::source_location::current()) {
    if(!ok)throw std::runtime_error("Default capture routing failed at "+std::to_string(at.line()));
}
struct State {
    uint64_t route=0;
    unsigned starts=0,live=0;
    bool failStart=false,failRead=false,pinned=false,wrongThread=false;
    std::function<void()> duringStart,duringRead;
};
class Endpoint final:public PcmCaptureEndpoint {
    std::shared_ptr<State> state_;
    std::thread::id owner_=std::this_thread::get_id();
    uint64_t route_=0;
public:
    explicit Endpoint(std::shared_ptr<State> state):state_(std::move(state)){++state_->live;}
    ~Endpoint() override {state_->wrongThread|=owner_!=std::this_thread::get_id();--state_->live;}
    void Start() override {
        Check(owner_==std::this_thread::get_id());route_=state_->route;++state_->starts;
        auto action=state_->duringStart;if(action)action();
        if(state_->failStart)throw std::runtime_error("Injected capture activation failure");
    }
    bool Read(PcmBlock& block,std::stop_token) override {
        Check(owner_==std::this_thread::get_id());block.fill(int16_t(100+route_));
        auto action=state_->duringRead;if(action)action();
        if(state_->failRead)throw std::runtime_error("Injected capture read failure");
        return true;
    }
    bool DeviceChanged() const override {return !state_->pinned && state_->route!=route_;}
    uint32_t DelayMs() const override {return 17;}
    uint64_t DroppedFrames() const override {return 3;}
};
inline void Run() {
    using namespace std::chrono_literals;
    auto state=std::make_shared<State>();
    auto factory=[&]{return std::make_unique<Endpoint>(state);};
    PcmBlock block;
    auto read=[&](PcmCaptureEndpoint& capture,int16_t sample){Check(capture.Read(block,{}));Check(std::all_of(block.begin(),block.end(),[&](auto v){return v==sample;}));};
    {
        RoutedPcmCapture capture(factory);capture.Start();read(capture,100);
        state->route+=3;read(capture,0);Check(state->starts==2 && state->live==1 && capture.DroppedFrames()==6);
        read(capture,103);
        state->duringRead=[&]{++state->route;state->duringRead={};};
        read(capture,0);read(capture,104);Check(state->starts==3);
        state->duringRead=[&]{++state->route;state->duringRead={};throw std::runtime_error("Changed during read");};
        read(capture,0);read(capture,105);Check(state->starts==4 && capture.DelayMs()==17);
        state->failRead=true;bool failed=false;try{capture.Read(block,{});}catch(const std::runtime_error&){failed=true;}
        Check(failed && state->starts==4);state->failRead=false;
    }
    Check(!state->live && !state->wrongThread);
    state=std::make_shared<State>();
    {
        state->duringStart=[&]{if(state->starts==1)++state->route;};
        RoutedPcmCapture capture(factory);capture.Start();read(capture,0);read(capture,101);
        Check(state->starts==2);
        state->pinned=true;++state->route;read(capture,101);Check(state->starts==2);
    }
    state=std::make_shared<State>();
    {
        // A route changes again while its replacement opens: one open per read.
        RoutedPcmCapture capture(factory);capture.Start();++state->route;
        state->duringStart=[&]{if(state->starts==2)++state->route;};
        read(capture,0);Check(state->starts==2);read(capture,0);Check(state->starts==3);read(capture,102);
    }
    state=std::make_shared<State>();
    {
        std::stop_source stop;stop.request_stop();RoutedPcmCapture capture(factory);
        bool cancelled=false;try{capture.Start(stop.get_token());}catch(const std::runtime_error&){cancelled=true;}
        Check(cancelled && !state->starts && !state->live);
    }
    state=std::make_shared<State>();
    {
        std::stop_source stop;state->duringStart=[&]{stop.request_stop();};
        RoutedPcmCapture capture(factory);bool cancelled=false;
        try{capture.Start(stop.get_token());}catch(const std::runtime_error&){cancelled=true;}
        Check(cancelled && state->starts==1 && !state->live && !capture.Read(block,stop.get_token()));
    }
    state=std::make_shared<State>();
    {
        std::stop_source stop;RoutedPcmCapture capture(factory);capture.Start();++state->route;
        state->duringStart=[&]{stop.request_stop();};
        Check(!capture.Read(block,stop.get_token()) && state->starts==2 && !state->live);
        Check(!capture.Read(block,stop.get_token()) && state->starts==2);
    }
    state=std::make_shared<State>();
    {
        std::stop_source stop;RoutedPcmCapture capture(factory);capture.Start();
        state->duringRead=[&]{++state->route;stop.request_stop();};
        Check(!capture.Read(block,stop.get_token()) && state->starts==1);
    }
    state=std::make_shared<State>();
    {
        auto now=std::chrono::steady_clock::time_point{};bool healthy=false;
        SourcePcmCapture capture([]{return AudioSelection{};},[&](AudioSelection){return std::make_unique<RoutedPcmCapture>(factory);},
            [&](bool ready){healthy=ready;},[&]{return now;});
        capture.Start();read(capture,100);Check(healthy);
        ++state->route;state->failStart=true;read(capture,0);Check(!healthy && state->starts==2 && !state->live);
        now+=249ms;read(capture,0);Check(state->starts==2);
        now+=1ms;state->failStart=false;read(capture,101);Check(healthy && state->starts==3 && capture.DroppedFrames()==6);
    }
    Check(!state->live && !state->wrongThread);
    auto microphone=Microsoft::WRL::Make<DefaultAudioDeviceNotification>(eCapture);
    microphone->OnDefaultDeviceChanged(eRender,eConsole,L"speakers");
    microphone->OnDefaultDeviceChanged(eCapture,eCommunications,L"communications");
    Check(microphone->revision()==0);
    microphone->OnDefaultDeviceChanged(eCapture,eConsole,nullptr);
    microphone->OnDefaultDeviceChanged(eCapture,eConsole,L"new-microphone");Check(microphone->revision()==2);
    screenshare::AudioCaptureConfig config;
    Check(dynamic_cast<RoutedPcmCapture*>(WasapiPcmEndpoints(config).capture().get())!=nullptr);
    config.source=screenshare::AudioCaptureSource::Microphone;
    Check(dynamic_cast<RoutedPcmCapture*>(WasapiPcmEndpoints(config).capture().get())!=nullptr);
    config.deviceId=L"pinned-device";Check(dynamic_cast<RoutedPcmCapture*>(WasapiPcmEndpoints(config).capture().get())==nullptr);
    config.source=screenshare::AudioCaptureSource::SystemOutput;
    Check(dynamic_cast<RoutedPcmCapture*>(WasapiPcmEndpoints(config).capture().get())==nullptr);
    config.deviceId.clear();config.source=screenshare::AudioCaptureSource::ProcessOutput;config.processId=11;
    Check(dynamic_cast<RoutedPcmCapture*>(WasapiPcmEndpoints(config).capture().get())==nullptr);
    config.source=screenshare::AudioCaptureSource::None;config.processId=0;
    Check(dynamic_cast<RoutedPcmCapture*>(WasapiPcmEndpoints(config).capture().get())==nullptr);
}
}
