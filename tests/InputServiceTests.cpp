#include "input/v2/InputService.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>
#include <mutex>
#include <source_location>
#include <thread>

using namespace screenshare::input;
using namespace std::chrono_literals;
void Check(bool value,std::source_location where=std::source_location::current()) {
    if(!value)throw std::runtime_error("Input check failed at line "+std::to_string(where.line()));
}
template<class F> void Wait(F fn) {
    const auto end=std::chrono::steady_clock::now()+3s;
    while(!fn()) { Check(std::chrono::steady_clock::now()<end); std::this_thread::sleep_for(2ms); }
}
class RecordingSink final : public Sink {
public:
    std::atomic<unsigned> applied{0},releases{0},grants{0};
    std::atomic<bool> fail{false},healthy{true};
    std::mutex mutex;
    Event last;
    std::thread::id owner;
    bool Grant(const std::string&,uint8_t,int) override { ++grants; owner=std::this_thread::get_id(); return !fail; }
    bool Apply(const std::string&,const Event& e) override {
        Check(owner==std::this_thread::get_id());
        std::lock_guard lock(mutex); last=e; ++applied; return !fail;
    }
    void Release(const std::string&) noexcept override { ++releases; }
    bool Healthy(const std::string&) override {return healthy;}
};
Status Read(Service& service,const std::string& id="viewer") {
    for(const auto& p:service.Read())if(p.peer==id)return p;
    return {};
}
void Protocol() {
    Message golden{"x",0x0102030405060708ULL,0x1112131415161718ULL,{}};
    const std::vector<uint8_t> expected{'S','I','N',1,3,1,1,2,3,4,5,6,7,8,17,18,19,20,21,22,23,24,'x'};
    Check(Encode(golden)==expected);
    for(unsigned k=0;k<=8;++k) {
        Message m{"connection_restart_7",9,12,{}}; auto& e=m.event;
        e.kind=Kind(k); e.capabilities=7; e.x=.25f; e.y=.75f; e.button=4; e.down=true;
        e.key=65; e.scan=0x11e; e.wheelX=-1200; e.wheelY=1200;
        e.buttons=0xf3ff; e.axes={-32768,32767,-1,0}; e.leftTrigger=255;
        const auto bytes=Encode(m); Check(!bytes.empty());
        const auto result=Decode(bytes); Check(result && Encode(*result)==bytes);
        for(size_t n=0;n<bytes.size();++n)Check(!Decode(std::span(bytes).first(n)));
        auto bad=bytes; bad.push_back(0); Check(!Decode(bad));
        bad=bytes; bad[3]=2; Check(!Decode(bad));
        bad=bytes; bad[4]=255; Check(!Decode(bad));
    }
    Message bad{"x",1,1,{}}; bad.event.kind=Kind::Pointer;
    bad.event.x=std::numeric_limits<float>::quiet_NaN(); Check(Encode(bad).empty());
    bad.event.x=1.01f; Check(Encode(bad).empty());
    bad.event.x=0; bad.event.kind=Kind::Key; bad.event.key=256; Check(Encode(bad).empty());
    bad.event.kind=Kind::Pad; bad.event.buttons=0x400; Check(Encode(bad).empty());
}
void Safety() {
    auto sink=std::make_shared<RecordingSink>(); Service host(true,sink),viewer(false);
    host.Bind("viewer","connection",true); viewer.Bind("host","connection",true);
    auto pump=[&] {
        for(auto& p:host.Drain("viewer",true,true))viewer.Receive("host",p.reliable,p.bytes);
        for(auto& p:viewer.Drain("host",true,true))host.Receive("viewer",p.reliable,p.bytes);
    };
    Event key; key.kind=Kind::Key; key.key=65; key.down=true;
    Check(!viewer.Submit("host",key)); Check(!viewer.Grant("host",7));
    Check(viewer.Request("host",7)); pump(); Check(Read(host).requested==7 && !Read(host).granted);
    Check(host.Grant("viewer",7));
    Wait([&] {pump();return Read(viewer,"host").granted==7;});
    Check(viewer.Submit("host",key)); pump(); Wait([&] {return sink->applied==1;});
    const auto grant=Read(host).permission;
    Message stale{"connection",grant,10000,key};
    host.Receive("viewer",false,Encode(stale)); // Key on unreliable lane rejected.
    std::this_thread::sleep_for(10ms); Check(sink->applied==1);
    auto wrong=stale; wrong.connection="other"; host.Receive("viewer",true,Encode(wrong));
    wrong=stale; wrong.permission++; host.Receive("viewer",true,Encode(wrong));
    Check(sink->applied==1);
    Event move; move.kind=Kind::Pointer; move.x=.75f; move.y=.25f;
    for(unsigned i=0;i<10000;++i)Check(viewer.Submit("host",move));
    const auto packets=viewer.Drain("host",true,true); Check(packets.size()<=3);
    for(auto& p:packets)host.Receive("viewer",p.reliable,p.bytes);
    Wait([&] {pump();return sink->applied>=2;}); Check(Read(viewer,"host").coalesced>=9999);
    // Per-state sequence spaces prevent a newer heartbeat suppressing a pad.
    Event pad; pad.kind=Kind::Pad; pad.axes={-32768,32767,1,-1};
    host.Receive("viewer",false,Encode({"connection",grant,20000,{}}));
    const auto before=sink->applied.load();
    host.Receive("viewer",false,Encode({"connection",grant,500,pad}));
    Wait([&] {return sink->applied>before;});
    // A complete gamepad keepalive repairs an unreliable dropped change.
    Check(viewer.Submit("host",pad));
    viewer.Drain("host",true,false);
    bool padKeepalive=false;
    Wait([&] {
        for(const auto& packet:viewer.Drain("host",true,true)) {
            const auto m=Decode(packet.bytes);
            if(m && m->event.kind==Kind::Pad)padKeepalive=m->event.axes==pad.axes;
        }
        return padKeepalive;
    });
    // No network pumping: owner watchdog must fire independently of signaling.
    Wait([&] {return Read(host).reason==Reason::Watchdog;});
    const auto count=sink->applied.load(); Check(!Read(host).granted && Read(host).permission>grant);
    host.Receive("viewer",true,Encode(stale)); std::this_thread::sleep_for(15ms); Check(sink->applied==count);
    Check(host.Grant("viewer",Keyboard)); Wait([&] {pump();return Read(viewer,"host").granted==Keyboard;});
    Check(Read(host).permission>grant);
    // Host revoke is local and cannot be delayed behind a congested transport.
    host.Revoke(); Check(!Read(host).granted); Wait([&] {return sink->releases>=2;});
    pump(); Check(!Read(viewer,"host").granted);
    Check(host.Grant("viewer",Keyboard)); Wait([&] {pump();return Read(viewer,"host").granted==Keyboard;});
    // Bounded outgoing transitions revoke instead of dropping key-up only.
    bool rejected=false;
    for(unsigned i=0;i<65;++i)rejected|=!viewer.Submit("host",key);
    Check(rejected && !Read(viewer,"host").granted && Read(viewer,"host").reason==Reason::Backpressure);
    pump(); Wait([&] {return !Read(host).granted;});
    Check(host.Grant("viewer",Mouse)); Wait([&] {pump();return Read(viewer,"host").granted==Mouse;});
    host.Configure(Mouse|Gamepad,0); Check(!Read(host).granted && !host.Grant("viewer",Keyboard));
    Check(host.Grant("viewer",Mouse)); Wait([&] {return Read(host).granted==Mouse;});
    sink->healthy=false; Wait([&] {return Read(host).reason==Reason::Backend;}); sink->healthy=true;
    Check(host.Grant("viewer",Mouse)); Wait([&] {return Read(host).granted==Mouse;});
    host.Bind("viewer","connection_restart_2",true); Check(!Read(host).granted);
    host.Receive("viewer",true,Encode(stale)); Check(sink->applied==count);
    host.Close(); viewer.Close(); Check(!host.Grant("viewer",Mouse) && !viewer.Submit("host",move));
}
void Congestion() {
    auto sink=std::make_shared<RecordingSink>(); Service host(true,sink);
    host.Bind("viewer","connection",true);
    Check(host.Grant("viewer",Keyboard)); Wait([&] {return Read(host).granted==Keyboard;});
    const auto grant=Read(host).permission;
    host.Drain("viewer",true,true); // All application data accepted into SCTP.
    // Even an empty application queue must notice stuck SCTP data.
    Wait([&] {host.Drain("viewer",false,true);return Read(host).reason==Reason::Backpressure;});
    Check(Read(host).permission>grant && !Read(host).granted);
    Check(host.Grant("viewer",Keyboard)); Wait([&] {return Read(host).granted==Keyboard;});
    sink->fail=true;
    Event key; key.kind=Kind::Key; key.key=65; key.down=true;
    host.Receive("viewer",true,Encode({"connection",Read(host).permission,1,key}));
    Wait([&] {return Read(host).reason==Reason::Backend;});
    Check(!Read(host).granted);
}
void Allocation() {
    auto sink=std::make_shared<RecordingSink>(); Service host(true,sink);
    for(unsigned i=0;i<4;++i)host.Bind(std::to_string(i),"connection",true);
    Check(host.Grant("0",Mouse|Keyboard|Gamepad)); Wait([&] {return Read(host,"0").granted==7;});
    Check(host.Grant("1",Mouse)); Wait([&] {return Read(host,"1").reason==Reason::Ownership;});
    Check(host.Grant("1",Gamepad)); Check(host.Grant("2",Gamepad));
    Wait([&] {return Read(host,"1").granted==Gamepad && Read(host,"2").granted==Gamepad;});
    Check(host.Grant("3",Gamepad)); Wait([&] {return Read(host,"3").reason==Reason::Ownership;});
    host.Configure(7,3); Check(host.Grant("0",Gamepad)); Wait([&] {return Read(host,"0").granted==Gamepad;});
    Check(host.Grant("1",Gamepad)); Wait([&] {return Read(host,"1").reason==Reason::Ownership;});
    host.Remove("0"); Wait([&] {return !Read(host,"0").ready;});
    Check(host.Grant("1",Gamepad)); Wait([&] {return Read(host,"1").granted==Gamepad;});
    // Removal must not exhaust the lifetime peer bound after many rejoins.
    for(unsigned i=0;i<100;++i) {
        const auto id="join"+std::to_string(i); host.Bind(id,"connection",true); Check(Read(host,id).ready);
        host.Remove(id); Wait([&] {return Read(host,id).peer.empty();});
    }
    Service disabled(true); disabled.Bind("viewer","connection",true); Check(!disabled.Grant("viewer",7));
}
int main() {
    try {Protocol();Safety();Allocation();Congestion();std::cout<<"{\"passed\":true,\"physical_input\":false}\n";}
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
