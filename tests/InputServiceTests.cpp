#include "input/v2/InputService.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <future>
#include <limits>
#include <mutex>
#include <source_location>
#include <thread>

using namespace screenshare::input;
using namespace std::chrono_literals;
void Check(bool value,std::source_location where=std::source_location::current()) {
    if(!value)throw std::runtime_error("Input check failed at line "+std::to_string(where.line()));
}
template<class F> void Wait(F fn,std::source_location where=std::source_location::current()) {
    const auto end=std::chrono::steady_clock::now()+3s;
    while(!fn()) { Check(std::chrono::steady_clock::now()<end,where); std::this_thread::sleep_for(2ms); }
}
class RecordingSink final : public Sink {
public:
    std::atomic<unsigned> applied{0},releases{0},grants{0},pauses{0};
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
    bool Pause(const std::string&) override {
        Check(owner==std::this_thread::get_id());
        std::lock_guard lock(mutex);last={};++pauses;return !fail;
    }
    bool Restrict(const std::string& peer,uint8_t) override {return Pause(peer);}
};
Status Read(Service& service,const std::string& id="viewer") {
    for(const auto& p:service.Read())if(p.peer==id)return p;
    return {};
}
void Protocol() {
    Message golden{"x",0x0102030405060708ULL,0x1112131415161718ULL,{}};
    const std::vector<uint8_t> expected{'S','I','N',2,3,1,1,2,3,4,5,6,7,8,17,18,19,20,21,22,23,24,'x'};
    Check(Encode(golden)==expected);
    for(unsigned k=0;k<=uint8_t(Kind::Withdraw);++k) {
        Message m{"connection_restart_7",9,12,{}}; auto& e=m.event;
        e.kind=Kind(k); e.capabilities=7; e.x=.25f; e.y=.75f; e.button=4; e.down=true;
        e.key=65; e.scan=0x11e; e.wheelX=-1200; e.wheelY=1200;
        e.buttons=0xf3ff; e.axes={-32768,32767,-1,0}; e.leftTrigger=255;
        const auto bytes=Encode(m); Check(!bytes.empty());
        const auto result=Decode(bytes); Check(result && Encode(*result)==bytes);
        for(size_t n=0;n<bytes.size();++n)Check(!Decode(std::span(bytes).first(n)));
        auto bad=bytes; bad.push_back(0); Check(!Decode(bad));
        bad=bytes; bad[3]=1; Check(!Decode(bad));
        bad=bytes; bad[4]=255; Check(!Decode(bad));
    }
    Message bad{"x",1,1,{}}; bad.event.kind=Kind::Pointer;
    bad.event.x=std::numeric_limits<float>::quiet_NaN(); Check(Encode(bad).empty());
    bad.event.x=1.01f; Check(Encode(bad).empty());
    bad.event.x=0; bad.event.kind=Kind::Key; bad.event.key=256; Check(Encode(bad).empty());
    bad.event.kind=Kind::Pad; bad.event.buttons=0x400; Check(Encode(bad).empty());
    bad.event.kind=Kind::RelativePointer;bad.event.x=-27;bad.event.y=50;
    Check(Decode(Encode(bad))->event.x==-27);
    bad.event.x=32768;Check(Encode(bad).empty());
    bad.event.x=std::numeric_limits<float>::infinity();Check(Encode(bad).empty());
    bad.event.kind=Kind::RelativeButton;bad.event.button=5;Check(Encode(bad).empty());
}
void WatchdogKeepsGrant() {
    auto sink=std::make_shared<RecordingSink>(); Service host(true,sink),viewer(false);
    host.Bind("viewer","lag",true);viewer.Bind("host","lag",true);
    auto pump=[&] {
        for(const auto& packet:host.Drain("viewer",true,true))viewer.Receive("host",packet.reliable,packet.bytes);
        for(const auto& packet:viewer.Drain("host",true,true))host.Receive("viewer",packet.reliable,packet.bytes);
    };
    Check(host.Grant("viewer",Gamepad));Wait([&]{pump();return Read(viewer,"host").granted==Gamepad;});
    Event pad;pad.kind=Kind::Pad;pad.buttons=1;
    for(unsigned gap=0;gap<3;++gap) {
        Check(viewer.Submit("host",pad));const auto before=sink->applied.load();
        Wait([&]{pump();return sink->applied>before;});
        const auto epoch=Read(host).permission;
        // Stop transport advancement while the independent watchdog runs.
        Wait([&]{return Read(host).reason==Reason::Watchdog;});
        Check(Read(host).granted==Gamepad && Read(host).paused && Read(host).permission>epoch &&
            sink->grants==1 && !sink->releases && sink->pauses==gap+1);
        const auto pausedCount=sink->applied.load();
        host.Receive("viewer",false,Encode({"lag",epoch,10000,pad}));
        Wait([&]{pump();return Read(viewer,"host").permission==Read(host).permission;});
        Check(Read(viewer,"host").granted==Gamepad && sink->applied==pausedCount);
        // Heartbeats and an old cached report cannot restore held state.
        Check(Read(host).reason==Reason::Watchdog);
        pad.buttons=uint16_t(gap+2);Check(viewer.Submit("host",pad));
        Wait([&]{pump();return Read(host).reason==Reason::None && sink->applied>pausedCount;});
    }
    host.Bind("viewer","lag",false);Check(!Read(host).granted);
    Wait([&]{return sink->releases==1;});
}
void WatchdogRevalidation() {
    class DelayedPause final : public Sink {
    public:
        std::promise<void> entered,unblock;
        std::shared_future<void> ready=unblock.get_future().share();
        std::atomic<unsigned> releases{0};
        bool Grant(const std::string&,uint8_t,int) override {return true;}
        bool Apply(const std::string&,const Event&) override {return true;}
        bool Pause(const std::string&) override {entered.set_value();ready.wait();return true;}
        void Release(const std::string&) noexcept override {++releases;}
    };
    auto sink=std::make_shared<DelayedPause>();Service host(true,sink);host.Bind("viewer","pause",true);
    Check(host.Grant("viewer",Gamepad));Wait([&]{return Read(host).granted==Gamepad;});
    const auto epoch=Read(host).permission;auto entered=sink->entered.get_future();
    const bool began=entered.wait_for(2s)==std::future_status::ready;
    if(!began){sink->unblock.set_value();Check(false);}
    auto revoke=std::async(std::launch::async,[&]{host.Revoke();return Read(host);});
    const bool responsive=revoke.wait_for(200ms)==std::future_status::ready;
    sink->unblock.set_value();const auto status=revoke.get();
    Check(responsive && !status.granted && status.permission>epoch);
    Wait([&]{return sink->releases>0;});Check(!Read(host).granted && !Read(host).paused);
    auto failed=std::make_shared<RecordingSink>();Service broken(true,failed);broken.Bind("viewer","failed-pause",true);
    Check(broken.Grant("viewer",Gamepad));Wait([&]{return Read(broken).granted==Gamepad;});
    failed->fail=true;Wait([&]{return !Read(broken).granted;});
    Wait([&]{return failed->releases>0;});Check(failed->pauses==1);
}
void WithdrawalDuringRenewal() {
    auto sink=std::make_shared<RecordingSink>();Service host(true,sink),viewer(false);
    host.Bind("viewer","withdraw",true);viewer.Bind("host","withdraw",true);
    auto pump=[&] {
        for(const auto& packet:host.Drain("viewer",true,true))viewer.Receive("host",packet.reliable,packet.bytes);
        for(const auto& packet:viewer.Drain("host",true,true))host.Receive("viewer",packet.reliable,packet.bytes);
    };
    Check(host.Grant("viewer",Gamepad));Wait([&]{pump();return Read(viewer,"host").granted==Gamepad;});
    Wait([&]{return Read(host).paused;});
    viewer.Revoke();Check(Read(viewer,"host").revokePending);
    Wait([&]{pump();return !Read(host).granted && !Read(viewer,"host").granted && !Read(viewer,"host").revokePending;});
    Wait([&]{return sink->releases==1;});Check(sink->grants==1);
}
void DelayedOwnerKeepsFreshInput() {
    class Delayed final : public Sink {
    public:
        std::promise<void> entered,unblock;
        std::shared_future<void> ready=unblock.get_future().share();
        std::atomic<unsigned> applied{0},pauses{0};
        bool Grant(const std::string&,uint8_t,int) override {return true;}
        bool Apply(const std::string&,const Event&) override {
            if(applied++==0){entered.set_value();ready.wait();}return true;
        }
        bool Pause(const std::string&) override {++pauses;return true;}
        void Release(const std::string&) noexcept override {}
    };
    auto sink=std::make_shared<Delayed>();Service host(true,sink);host.Bind("viewer","owner-gap",true);
    Check(host.Grant("viewer",Gamepad));Wait([&]{return Read(host).granted==Gamepad;});
    const auto epoch=Read(host).permission;
    Event pad;pad.kind=Kind::Pad;pad.buttons=1;
    auto entered=sink->entered.get_future();host.Receive("viewer",false,Encode({"owner-gap",epoch,1,pad}));
    if(entered.wait_for(2s)!=std::future_status::ready){sink->unblock.set_value();Check(false);}
    std::this_thread::sleep_for(350ms);
    pad.buttons=2;host.Receive("viewer",false,Encode({"owner-gap",epoch,2,pad}));
    sink->unblock.set_value();Wait([&]{return sink->applied>=2;});
    Check(Read(host).granted==Gamepad && Read(host).permission==epoch && !Read(host).paused && !sink->pauses);
}
void RelativeMotion() {
    auto sink=std::make_shared<RecordingSink>();Service host(true,sink),viewer(false);
    host.Bind("viewer","relative",true);viewer.Bind("host","relative",true);
    auto pump=[&] {
        for(const auto& packet:host.Drain("viewer",true,true))viewer.Receive("host",packet.reliable,packet.bytes);
        for(const auto& packet:viewer.Drain("host",true,true))host.Receive("viewer",packet.reliable,packet.bytes);
    };
    Check(host.Grant("viewer",Mouse));Wait([&]{pump();return Read(viewer,"host").granted==Mouse;});
    Event motion;motion.kind=Kind::RelativePointer;motion.sourceGeneration=9;motion.x=3;motion.y=-2;
    for(int i=0;i<100;++i)Check(viewer.Submit("host",motion));
    std::optional<Event> sent;
    Wait([&]{
        for(const auto& packet:viewer.Drain("host",true,true)) {
            auto message=Decode(packet.bytes);Check(message.has_value());
            if(message->event.kind==Kind::RelativePointer) {Check(!packet.reliable);sent=message->event;}
            host.Receive("viewer",packet.reliable,packet.bytes);
        }
        return sent.has_value();
    });
    Check(sent->x==300 && sent->y==-200 && sent->sourceGeneration==9);
    Wait([&]{return sink->applied>0;});
    {std::lock_guard lock(sink->mutex);Check(sink->last.kind==Kind::RelativePointer && sink->last.x==300 && sink->last.y==-200);}
    // Do not combine motion from different captured sources.
    Check(viewer.Submit("host",motion));motion.sourceGeneration=10;motion.x=-4;motion.y=7;
    Check(viewer.Submit("host",motion));sent.reset();
    Wait([&]{for(const auto& packet:viewer.Drain("host",true,true))if(auto m=Decode(packet.bytes);m && m->event.kind==Kind::RelativePointer)sent=m->event;return sent.has_value();});
    Check(sent->sourceGeneration==10 && sent->x==-4 && sent->y==7);
    Event button;button.kind=Kind::RelativeButton;button.sourceGeneration=10;button.down=true;
    Check(viewer.Submit("host",button));
    auto packets=viewer.Drain("host",true,true);Check(packets.size()==1 && packets.front().reliable);
    host.Receive("viewer",true,packets.front().bytes);
    Wait([&]{return sink->applied>=2;});
    {std::lock_guard lock(sink->mutex);Check(sink->last.kind==Kind::RelativeButton && sink->last.down);}
    host.Revoke();Wait([&]{pump();return !Read(viewer,"host").granted;});
    Check(!viewer.Submit("host",motion));
}
void RepeatedRevoke() {
    auto sink=std::make_shared<RecordingSink>(); Service host(true,sink),viewer(false);
    host.Bind("viewer","connection",true); viewer.Bind("host","connection",true);
    auto pump=[&] {
        for(auto& packet:host.Drain("viewer",true,true))viewer.Receive("host",packet.reliable,packet.bytes);
        for(auto& packet:viewer.Drain("host",true,true))host.Receive("viewer",packet.reliable,packet.bytes);
    };
    Check(host.Grant("viewer",Gamepad));
    Wait([&] {pump();return Read(viewer,"host").granted==Gamepad;});
    viewer.Revoke(); viewer.Revoke(); viewer.Revoke();
    Check(Read(viewer,"host").revokePending && !viewer.Request("host",Gamepad));
    pump(); // Repeated local cleanup must not erase the unsent release.
    Check(!Read(host).granted && Read(host).reason==Reason::Revoked);
    pump(); Check(!Read(viewer,"host").revokePending);
    Check(viewer.Request("host",Gamepad)); pump(); Check(Read(host).requested==Gamepad);
    Check(host.Grant("viewer",Gamepad));
    Wait([&] {pump();return Read(viewer,"host").granted==Gamepad;});
}
void AdditiveRequests() {
    auto sink=std::make_shared<RecordingSink>(); Service host(true,sink),viewer(false);
    host.Bind("viewer","connection",true);viewer.Bind("host","connection",true);
    auto pump=[&] {
        for(const auto& p:host.Drain("viewer",true,true))viewer.Receive("host",p.reliable,p.bytes);
        for(const auto& p:viewer.Drain("host",true,true))host.Receive("viewer",p.reliable,p.bytes);
    };
    Check(host.Grant("viewer",Keyboard));
    Wait([&]{pump();return Read(viewer,"host").granted==Keyboard;});
    const auto permission=Read(viewer,"host").permission;
    const auto releases=sink->releases.load();
    Check(viewer.Request("host",Mouse));Check(viewer.Request("host",Gamepad));pump();
    Check(Read(host).requested==(Mouse|Gamepad) && Read(host).granted==Keyboard);
    Check(Read(viewer,"host").requested==(Mouse|Gamepad) && Read(viewer,"host").granted==Keyboard);
    Check(Read(host).permission==permission && sink->releases==releases);
    Event key;key.kind=Kind::Key;key.key=65;
    Check(viewer.SubmitIfCurrent("host",permission,key));
    Wait([&]{pump();return sink->applied>0;});
    host.Deny("viewer",Mouse);pump();
    Check(Read(host).requested==Gamepad && Read(viewer,"host").requested==Gamepad);
    Check(Read(viewer,"host").granted==Keyboard && Read(host).permission==permission && sink->releases==releases);
    // Stale/lane-forged denials cannot dismiss another pending control.
    viewer.Receive("host",false,Encode({"connection",permission,9999,{Kind::RequestDenied,Gamepad}}));
    viewer.Receive("host",true,Encode({"connection",permission+1,9999,{Kind::RequestDenied,Gamepad}}));
    Check(Read(viewer,"host").requested==Gamepad);
    host.Receive("viewer",true,Encode({"connection",permission,9999,{Kind::RequestDenied,Gamepad}}));
    Check(Read(host).requested==Gamepad);
    host.Deny("viewer",Gamepad);pump();
    Check(!Read(host).requested && !Read(viewer,"host").requested);
    Check(Read(viewer,"host").granted==Keyboard && Read(host).permission==permission && sink->releases==releases);
    // Same-epoch acknowledgements cannot authorize a changed set of controls.
    viewer.Receive("host",true,Encode({"connection",permission,10000,{Kind::Permission,7}}));
    Check(Read(viewer,"host").granted==Keyboard);
    Check(viewer.Request("host",Mouse));pump();
    Check(viewer.Request("host",Gamepad));pump();
    // A quick deny followed by grant must retain the unsent denial response.
    host.Deny("viewer",Gamepad);
    Check(host.Grant("viewer",Read(host).granted|Read(host).requested));
    Wait([&]{pump();return Read(viewer,"host").granted==(Keyboard|Mouse);});
    Check(!Read(host).requested && !Read(viewer,"host").requested);
    Check(viewer.Request("host",Gamepad));pump();
    Check(Read(host).requested==Gamepad && Read(host).granted==(Keyboard|Mouse));
    viewer.Revoke();pump();pump();
    Check(!Read(host).granted && !Read(host).requested && !Read(viewer,"host").revokePending);
    // Release also cancels a request before any control has been granted.
    Check(viewer.Request("host",Mouse));pump();Check(Read(host).requested==Mouse);
    viewer.Revoke();pump();pump();Check(!Read(host).requested && !Read(viewer,"host").revokePending);
    // Initial permission must preserve all independent unsent requests.
    host.Bind("viewer","new_connection",true);viewer.Bind("host","new_connection",true);
    Check(viewer.Request("host",Mouse));Check(viewer.Request("host",Keyboard));pump();
    Check(Read(host).requested==(Mouse|Keyboard));
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
    const auto count=sink->applied.load(); Check(Read(host).granted==7 && Read(host).paused && Read(host).permission>grant);
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
void RequestDuringGrant() {
    class Delayed final : public Sink {
    public:
        std::promise<void> entered, unblock;
        std::shared_future<void> released=unblock.get_future().share();
        bool Grant(const std::string&,uint8_t,int) override {entered.set_value();released.wait();return true;}
        bool Apply(const std::string&,const Event&) override {return true;}
        void Release(const std::string&) noexcept override {}
    };
    auto sink=std::make_shared<Delayed>();Service host(true,sink),viewer(false);
    host.Bind("viewer","connection",true);viewer.Bind("host","connection",true);
    auto pump=[&] {
        for(const auto& p:host.Drain("viewer",true,true))viewer.Receive("host",p.reliable,p.bytes);
        for(const auto& p:viewer.Drain("host",true,true))host.Receive("viewer",p.reliable,p.bytes);
    };
    pump();Check(viewer.Request("host",Keyboard));pump();Check(host.Grant("viewer",Keyboard));
    sink->entered.get_future().wait();
    const bool accepted=viewer.Request("host",Mouse);pump();
    const auto pending=Read(host).requested;
    sink->unblock.set_value(); // Always release the input owner before assertions.
    Check(accepted && (pending&Mouse)); // Keyboard may still be applying.
    Wait([&]{pump();return Read(viewer,"host").granted==Keyboard;});
    Check(Read(host).requested==Mouse && Read(viewer,"host").requested==Mouse);
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
void DelayedDriverGrant() {
    class Delayed final : public Sink {
    public:
        std::promise<void> entered, unblock;
        std::shared_future<void> released = unblock.get_future().share();
        std::atomic<unsigned> neutral{0};
        bool Grant(const std::string&,uint8_t,int) override { entered.set_value(); released.wait(); return true; }
        bool Apply(const std::string&,const Event&) override { return true; }
        void Release(const std::string&) noexcept override { ++neutral; }
    };
    auto sink = std::make_shared<Delayed>(); Service host(true,sink);
    host.Bind("viewer","connection",true);
    Check(host.Grant("viewer",Gamepad));
    sink->entered.get_future().wait();
    auto revoke = std::async(std::launch::async,[&] { host.Revoke(); return Read(host); });
    const bool responsive = revoke.wait_for(200ms)==std::future_status::ready;
    sink->unblock.set_value(); // Always drain even if the responsiveness assertion fails.
    const auto status = revoke.get(); Check(responsive && !status.granted);
    Wait([&] {return sink->neutral>0;}); Check(!Read(host).granted);
}
void Observations() {
    auto sink = std::make_shared<RecordingSink>(); Service host(true, sink), viewer(false);
    host.Bind("viewer", "first", true); viewer.Bind("host", "first", true);
    host.Bind("other", "isolated", true);
    auto pump = [&] {
        for (const auto& packet : host.Drain("viewer", true, true)) viewer.Receive("host", packet.reliable, packet.bytes);
        for (const auto& packet : viewer.Drain("host", true, true)) host.Receive("viewer", packet.reliable, packet.bytes);
    };
    Check(!Read(host).queueWaitUs && !Read(host).backendApplyUs);
    Check(host.Grant("viewer", Mouse | Keyboard));
    Wait([&] { pump(); return Read(viewer, "host").granted == (Mouse | Keyboard); });
    Event key; key.kind = Kind::Key; key.key = 65;
    Check(viewer.Submit("host", key));
    Check(Read(viewer, "host").reliableQueued == 1);
    Wait([&] { pump(); return Read(host).applied == 1; });
    Check(Read(host).queueWaitUs && Read(host).backendApplyUs && !Read(host, "other").queueWaitUs);
    // Heartbeats maintain the grant but must not refresh measurements of actual input.
    Wait([&] { pump(); return !Read(host).queueWaitUs; });
    Check(Read(host).granted && !Read(host).backendApplyUs);
    Event pointer; pointer.kind = Kind::Pointer; pointer.x = pointer.y = .5f;
    for (int i = 0; i < 10000; ++i) Check(viewer.Submit("host", pointer));
    Check(Read(viewer, "host").stateQueued <= 3 && Read(viewer, "host").coalesced >= 9999);
    viewer.Drain("host", false, false);
    Check(Read(viewer, "host").transportBlocked && !Read(viewer, "host").stateQueued);
    viewer.Drain("host", true, true); Check(!Read(viewer, "host").transportBlocked);
    Check(viewer.Submit("host", key));
    Wait([&] { pump(); return Read(host).queueWaitUs.has_value(); });
    host.Revoke(); Check(!Read(host).queueWaitUs && !Read(host).backendApplyUs && !Read(host).stateQueued);
    host.Bind("viewer", "replacement", true);
    Check(!Read(host).applied && !Read(host).rejected && !Read(host).coalesced && !Read(host).queueWaitUs);
}
void StalePollingOwner() {
    auto sink = std::make_shared<RecordingSink>(); Service host(true, sink), viewer(false);
    host.Bind("viewer", "connection", true); viewer.Bind("host", "connection", true);
    auto pump = [&] {
        for (const auto& p : host.Drain("viewer", true, true)) viewer.Receive("host", p.reliable, p.bytes);
        for (const auto& p : viewer.Drain("host", true, true)) host.Receive("viewer", p.reliable, p.bytes);
    };
    Check(host.Grant("viewer", Gamepad));
    Wait([&] { pump(); return Read(viewer,"host").granted == Gamepad; });
    const auto old = Read(viewer,"host").permission;
    host.Revoke();
    Wait([&] { pump(); return !Read(viewer,"host").granted; });
    Check(host.Grant("viewer", Gamepad));
    Wait([&] { pump(); return Read(viewer,"host").granted == Gamepad; });
    const auto current = Read(viewer,"host").permission;
    Event e; e.kind = Kind::Pad;
    Check(current > old && !viewer.SubmitIfCurrent("host", old, e));
    Check(!viewer.SubmitIfCurrent("host", 0, e));
    viewer.RevokeIfCurrent("host", old);
    Check(Read(viewer,"host").granted == Gamepad);
    Check(viewer.SubmitIfCurrent("host", current, e));
    viewer.RevokeIfCurrent("host", current);
    Check(!Read(viewer,"host").granted && Read(viewer,"host").revokePending);
}
void SelectiveRelease() {
    auto sink=std::make_shared<RecordingSink>(); Service host(true,sink),viewer(false);
    host.Bind("viewer","selective",true,true);viewer.Bind("host","selective",true,true);
    auto pump=[&] {
        for(const auto& p:host.Drain("viewer",true,true))viewer.Receive("host",p.reliable,p.bytes);
        for(const auto& p:viewer.Drain("host",true,true))host.Receive("viewer",p.reliable,p.bytes);
    };
    Wait([&]{pump();return Read(viewer,"host").permission>0;});
    Check(host.Grant("viewer",7));Wait([&]{pump();return Read(viewer,"host").granted==7;});
    Event key;key.kind=Kind::Key;key.key=65;key.down=true;
    Event mouse;mouse.kind=Kind::Button;mouse.down=true;
    Event pad;pad.kind=Kind::Pad;pad.buttons=1;
    Check(viewer.Submit("host",key) && viewer.Submit("host",mouse) && viewer.Submit("host",pad));
    Wait([&]{pump();return sink->applied>=3;});
    const auto old=Read(viewer,"host").permission;
    const auto originalGrants=sink->grants.load();
    Check(viewer.Release("host",Mouse));
    Check(Read(viewer,"host").granted==(Keyboard|Gamepad) && Read(viewer,"host").releasePending==Mouse);
    Check(!viewer.Submit("host",mouse) && !viewer.Request("host",Mouse));
    Check(viewer.Release("host",Mouse));
    Wait([&]{pump();return !Read(viewer,"host").releasePending && Read(host).granted==(Keyboard|Gamepad);});
    Check(Read(viewer,"host").granted==(Keyboard|Gamepad) && Read(viewer,"host").permission>old);
    Check(sink->grants==originalGrants && sink->pauses>0);
    Check(viewer.Submit("host",key) && viewer.Submit("host",pad));
    // Poller teardown must not revoke keyboard after controller withdrawal.
    const auto beforePad=Read(viewer,"host").permission;
    Check(viewer.Release("host",Gamepad));viewer.RevokeIfCurrent("host",beforePad,Gamepad);
    Check(Read(viewer,"host").granted==Keyboard && !Read(viewer,"host").revokePending);
    Wait([&]{pump();return !Read(viewer,"host").releasePending && Read(host).granted==Keyboard;});
    // Cancel one pending request without losing other requests or grants.
    for(const bool sent:{false,true}) {
        Check(viewer.Request("host",Mouse) && viewer.Request("host",Gamepad));
        if(sent){pump();Check(Read(host).requested==(Mouse|Gamepad));}
        Check(viewer.Release("host",Mouse));
        Wait([&]{pump();return !Read(viewer,"host").releasePending;});
        Check(Read(host).requested==Gamepad && Read(viewer,"host").requested==Gamepad && Read(host).granted==Keyboard);
        Check(viewer.Release("host",Gamepad));Wait([&]{pump();return !Read(viewer,"host").releasePending;});
        Check(!Read(host).requested && Read(host).granted==Keyboard);
    }
    // A grant already in flight cannot restore withdrawn consent.
    Check(host.Grant("viewer",7));Wait([&]{pump();return Read(viewer,"host").granted==7;});
    Check(viewer.Release("host",Mouse));Check(host.Grant("viewer",7));
    Wait([&]{pump();Check(!(Read(viewer,"host").granted&Mouse));return !Read(viewer,"host").releasePending && Read(host).granted==(Keyboard|Gamepad);});
    const auto stale=Read(host).permission;
    Check(host.Grant("viewer",7));Wait([&]{pump();return Read(viewer,"host").granted==7;});
    host.Receive("viewer",true,Encode({"selective",stale,10000,{Kind::Withdraw,Keyboard}}));
    host.Receive("viewer",false,Encode({"selective",Read(host).permission,10001,{Kind::Withdraw,Keyboard}}));
    Check(Read(host).granted==7);
    Check(viewer.Release("host",Mouse) && viewer.Release("host",Keyboard) && viewer.Release("host",Gamepad));
    Wait([&]{pump();return !Read(viewer,"host").releasePending && !Read(host).granted;});
    Check(!Read(viewer,"host").granted && !Read(viewer,"host").revokePending);
    // Legacy hosts never receive a new packet. Last/all release still works.
    host.Bind("viewer","old",true);viewer.Bind("host","old",true);pump();
    Check(!Read(viewer,"host").selectiveRelease && !Read(viewer,"host").releasePending);
    Check(host.Grant("viewer",Mouse|Keyboard));Wait([&]{pump();return Read(viewer,"host").granted==(Mouse|Keyboard);});
    Check(!viewer.Release("host",Mouse) && Read(viewer,"host").granted==(Mouse|Keyboard));
    Check(viewer.Release("host",Mouse|Keyboard));pump();pump();Check(!Read(host).granted);
    Check(!host.Release("viewer",Mouse) && !viewer.Release("host",0) && !viewer.Release("host",8));
}
void WithdrawalCrossesNewGrant() {
    struct State {std::promise<void> entered,resume;std::atomic<uint8_t> active{0};};
    auto state=std::make_shared<State>();auto resumed=state->resume.get_future();
    class Device final:public Sink {
        std::shared_ptr<State> state_;std::future<void>& resumed_;
    public:
        Device(std::shared_ptr<State> state,std::future<void>& resumed):state_(std::move(state)),resumed_(resumed){}
        bool Grant(const std::string&,uint8_t caps,int) override {state_->active=caps;return true;}
        bool Apply(const std::string&,const Event&) override {return true;}
        void Release(const std::string&) noexcept override {state_->active=0;}
        bool Healthy(const std::string&) override {return state_->active!=0;}
        bool Restrict(const std::string&,uint8_t caps) override {
            state_->entered.set_value();resumed_.wait();state_->active=caps;return true;
        }
    };
    Service host(true,std::make_shared<Device>(state,resumed)),viewer(false);
    struct Resume {std::promise<void>& value;~Resume(){try{value.set_value();}catch(const std::future_error&){}}} resume{state->resume};
    host.Bind("viewer","crossed",true,true);viewer.Bind("host","crossed",true,true);
    auto pump=[&]{
        for(const auto& packet:host.Drain("viewer",true,true))viewer.Receive("host",packet.reliable,packet.bytes);
        for(const auto& packet:viewer.Drain("host",true,true))host.Receive("viewer",packet.reliable,packet.bytes);
    };
    Check(host.Grant("viewer",7));Wait([&]{pump();return Read(viewer,"host").granted==7;});
    auto entered=state->entered.get_future();Check(viewer.Release("host",Mouse));
    Wait([&]{pump();return entered.wait_for(0ms)==std::future_status::ready;});
    const bool granted=host.Grant("viewer",Keyboard);state->resume.set_value();Check(granted);
    Wait([&]{pump();return Read(viewer,"host").granted==Keyboard && !Read(viewer,"host").releasePending;});
    const auto until=std::chrono::steady_clock::now()+50ms;
    Wait([&]{pump();return std::chrono::steady_clock::now()>=until;});
    Check(state->active==Keyboard && Read(host).granted==Keyboard);
}
int main() {
    try {Protocol();SelectiveRelease();WithdrawalCrossesNewGrant();WatchdogKeepsGrant();WatchdogRevalidation();WithdrawalDuringRenewal();DelayedOwnerKeepsFreshInput();RelativeMotion();Safety();Allocation();Congestion();DelayedDriverGrant();RepeatedRevoke();AdditiveRequests();RequestDuringGrant();Observations();StalePollingOwner();std::cout<<"{\"passed\":true,\"physical_input\":false}\n";}
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
