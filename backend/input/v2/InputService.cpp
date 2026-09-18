#include "InputService.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace screenshare::input {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace {
template<class F> auto External(std::unique_lock<std::mutex>& lock, F&& fn) {
    struct Unlock {
        std::unique_lock<std::mutex>& lock;
        explicit Unlock(std::unique_lock<std::mutex>& value) : lock(value) { lock.unlock(); }
        ~Unlock() { lock.lock(); }
    } unlocked(lock);
    return fn();
}
unsigned StateSlot(Kind kind) { return kind == Kind::Pointer ? 0 : kind == Kind::Pad ? 1 : 2; }
uint8_t Required(Kind kind) {
    if(kind==Kind::Pointer || kind==Kind::Button || kind==Kind::Wheel) return Mouse;
    if(kind==Kind::Key) return Keyboard;
    if(kind==Kind::Pad) return Gamepad;
    return 0;
}
}
struct Service::Impl {
    struct Pending { Message message; Clock::time_point received; };
    struct Peer {
        Status status;
        std::string connection;
        uint64_t sent = 0, controlSeen = 0;
        std::array<uint64_t,3> stateSeen{};
        std::deque<Pending> incoming;
        std::deque<Message> outgoing;
        std::array<std::optional<Pending>,3> incomingState;
        std::array<std::optional<Message>,3> outgoingState;
        std::optional<Event> lastPad;
        std::optional<uint8_t> grant;
        bool release = false, removed = false;
        int padSlot = -1;
        Clock::time_point lastInput{}, lastSend{}, lastStateDrain{}, blocked{};
        Clock::time_point measured{};
    };
    bool host, closed = false;
    uint8_t allowed = 7;
    unsigned localPads = 0;
    uint64_t nextPermission = 0;
    std::shared_ptr<Sink> sink;
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool work = false;
    std::map<std::string, Peer> peers;
    std::jthread worker;
    Impl(bool h, std::shared_ptr<Sink> s) : host(h), sink(std::move(s)) {}
    void Clear(Peer& p) {
        p.incoming.clear(); p.outgoing.clear();
        p.incomingState = {}; p.outgoingState = {}; p.lastPad.reset(); p.blocked = {};
        p.status.queueWaitUs.reset(); p.status.backendApplyUs.reset(); p.measured = {};
    }
    void Queue(Peer& p, Event e) {
        Message m{p.connection,p.status.permission,++p.sent,e};
        if(Replaceable(e.kind)) {
            auto& slot=p.outgoingState[StateSlot(e.kind)];
            if(slot) ++p.status.coalesced;
            slot=std::move(m);
        } else p.outgoing.push_back(std::move(m));
    }
    void Revoke(Peer& p, Reason reason, bool notify = true) {
        work=true; wake.notify_one();
        const bool had=p.status.granted!=0;
        const bool releasing = !host && (had || p.status.revokePending) && notify && p.status.ready;
        Clear(p); p.grant.reset(); p.status.granted=0; p.status.requested=0; p.status.grantPending=false;
        p.status.reason=reason; p.padSlot=-1;
        p.release=p.release || had;
        if(host) {
            p.status.permission=++nextPermission;
            if(notify && p.status.ready) { Event e; e.kind=Kind::Permission; Queue(p,e); }
        } else {
            p.status.revokePending = releasing;
            if(releasing) { Event e; e.kind=Kind::Release; Queue(p,e); }
        }
    }
    bool Capacity(Peer& p) {
        // Every message <=170 bytes, so 64 total queued transitions <16 KiB.
        if(p.incoming.size()+p.outgoing.size()<64)return true;
        Revoke(p,Reason::Backpressure); return false;
    }
    void Apply(Peer& p, const Pending& pending, Clock::time_point now, std::unique_lock<std::mutex>& lock) {
        const auto& m=pending.message; const auto& e=m.event;
        if(!p.status.granted || m.permission!=p.status.permission || now-pending.received>=300ms ||
            (Required(e.kind) & p.status.granted)!=Required(e.kind)) { ++p.status.rejected; return; }
        p.lastInput=std::max(p.lastInput,pending.received);
        if(e.kind==Kind::Heartbeat)return;
        const auto permission = p.status.permission;
        const auto peer = p.status.peer;
        try {
            Clock::time_point began{}, completed{};
            const bool applied = sink && External(lock, [&] {
                began = Clock::now();
                const bool result = sink->Apply(peer,e);
                completed = Clock::now();
                return result;
            });
            if (p.status.permission != permission || closed) return;
            if(applied) {
                p.status.queueWaitUs = std::chrono::duration_cast<std::chrono::microseconds>(began - pending.received).count();
                p.status.backendApplyUs = std::chrono::duration_cast<std::chrono::microseconds>(completed - began).count();
                p.measured = completed; ++p.status.applied; return;
            }
        } catch(...) {}
        if (p.status.permission != permission || closed) return;
        Revoke(p,Reason::Backend);
    }
    void Tick(std::unique_lock<std::mutex>& lock) {
        const auto now=Clock::now();
        for(auto& [id,p]:peers) {
            if(p.release) { if(sink)External(lock,[&] {sink->Release(id);}); p.release=false; }
            if(closed)continue;
            if(host && p.grant) {
                const auto caps=*p.grant; p.grant.reset();
                bool busy=false; unsigned pads=0;
                std::array<bool,3> occupied{};
                for(const auto& [other,q]:peers) if(other!=id) {
                    busy|=(caps & q.status.granted & (Mouse|Keyboard))!=0;
                    if(q.status.granted & Gamepad) { ++pads; if(q.padSlot>=0) occupied[q.padSlot]=true; }
                }
                if((caps&Gamepad) && pads>=std::min(3u,4-std::min(4u,localPads)))busy=true;
                int slot=-1;
                if(caps&Gamepad) for(int i=0;i<3;++i)if(!occupied[i]) {slot=i;break;}
                bool granted=false;
                const auto permission = p.status.permission;
                if(!busy && sink && p.status.ready && (caps & allowed)==caps) {
                    try { granted=External(lock,[&] {return sink->Grant(id,caps,slot);}); } catch(...) {}
                }
                if (p.status.permission != permission || closed || !p.status.ready) {
                    if (sink) External(lock,[&] {sink->Release(id);});
                    continue; // A late driver completion cannot revive a revoke.
                }
                if(!granted) {
                    // A partially successful backend grant must also be released.
                    if(sink)External(lock,[&] {sink->Release(id);});
                    if (p.status.permission != permission || closed) continue;
                    Revoke(p,busy?Reason::Ownership:Reason::Backend); continue;
                }
                Clear(p); p.status.granted=caps; p.status.requested=0; p.status.grantPending=false;
                p.status.permission=++nextPermission; p.status.reason=Reason::None;
                p.padSlot=slot; p.lastInput=Clock::now();
                Event e; e.kind=Kind::Permission; e.capabilities=caps; Queue(p,e);
            }
            if(host && p.status.granted) {
                const auto permission = p.status.permission;
                bool healthy=false; try { healthy=sink && External(lock,[&] {return sink->Healthy(id);}); } catch(...) {}
                if (p.status.permission == permission && p.status.granted &&
                    (!healthy || Clock::now()-p.lastInput>=300ms)) Revoke(p,healthy?Reason::Watchdog:Reason::Backend);
            }
            // Bounded independent owner; no rendering, signaling HTTP or stats work.
            while(host && !p.incoming.empty()) {
                auto pending=std::move(p.incoming.front()); p.incoming.pop_front(); Apply(p,pending,Clock::now(),lock);
            }
            for(auto& state:p.incomingState) if(state) {
                auto pending=std::move(*state); state.reset(); if(host)Apply(p,pending,Clock::now(),lock);
            }
            if(!host && p.status.ready && p.status.granted && now-p.lastSend>=100ms) {
                if((p.status.granted & Gamepad) && p.lastPad) Queue(p,*p.lastPad);
                Event e; e.kind=Kind::Heartbeat; Queue(p,e); p.lastSend=now;
            }
            if(p.release) { if(sink)External(lock,[&] {sink->Release(id);}); p.release=false; }
        }
        std::erase_if(peers,[](const auto& entry) { return entry.second.removed && !entry.second.release; });
    }
};
Service::Service(bool host,std::shared_ptr<Sink> sink) : impl_(std::make_unique<Impl>(host,std::move(sink))) {
    auto* p=impl_.get();
    p->worker=std::jthread([p](std::stop_token stop) {
        std::unique_lock lock(p->mutex);
        while(!stop.stop_requested()) {
            p->work=false; p->Tick(lock);
            const bool active=std::any_of(p->peers.begin(),p->peers.end(),[](const auto& entry) {return entry.second.status.granted!=0;});
            if(active)p->wake.wait_for(lock,4ms,[&] {return stop.stop_requested();});
            else p->wake.wait(lock,[&] {return stop.stop_requested() || p->work;});
        }
        p->Tick(lock);
    });
}
Service::~Service() { Close(); }
void Service::Close() {
    { std::lock_guard lock(impl_->mutex);
      if(!impl_->closed) {
          impl_->closed=true;
          for(auto& [id,p]:impl_->peers) { impl_->Revoke(p,Reason::Disconnected,false); p.status.ready=false; }
      }
    }
    impl_->worker.request_stop();
    impl_->wake.notify_one();
    if(impl_->worker.joinable())impl_->worker.join();
}
void Service::Bind(const std::string& id,const std::string& connection,bool ready) {
    std::lock_guard lock(impl_->mutex);
    if(impl_->closed || id.empty() || id.size()>128 || connection.size()>128)return;
    auto found=impl_->peers.find(id);
    if(found==impl_->peers.end() && impl_->peers.size()>=63)return;
    auto& p=impl_->peers[id]; p.status.peer=id; p.removed=false;
    ready=ready && !connection.empty();
    if(p.connection!=connection || p.status.ready!=ready) {
        impl_->Revoke(p,Reason::Disconnected,false);
        p.connection=connection; p.status.ready=ready; p.sent=p.controlSeen=0; p.stateSeen={};
        p.status.applied = p.status.rejected = p.status.coalesced = 0;
        if(!impl_->host)p.status.permission=0;
        else if(ready) { Event e; e.kind=Kind::Permission; impl_->Queue(p,e); }
    }
}
void Service::Remove(const std::string& id) {
    std::lock_guard lock(impl_->mutex);
    auto it=impl_->peers.find(id); if(it==impl_->peers.end())return;
    // Keep the entry until its owner releases the sink; Bind can reuse it safely.
    impl_->Revoke(it->second,Reason::Disconnected,false); it->second.status.ready=false; it->second.removed=true;
}
void Service::Configure(uint8_t allowed,unsigned localPads) {
    std::lock_guard lock(impl_->mutex);
    impl_->allowed=allowed&7; impl_->localPads=std::min(4u,localPads);
    for(auto& [id,p]:impl_->peers)impl_->Revoke(p,Reason::SourceChanged);
}
bool Service::Request(const std::string& id,uint8_t caps) {
    std::lock_guard lock(impl_->mutex); auto it=impl_->peers.find(id);
    if(impl_->closed || impl_->host || it==impl_->peers.end() || !it->second.status.ready || it->second.status.revokePending || !caps || (caps&~7))return false;
    auto& p=it->second; if(!impl_->Capacity(p))return false;
    Event e; e.kind=Kind::Request; e.capabilities=caps; impl_->Queue(p,e); return true;
}
bool Service::Grant(const std::string& id,uint8_t caps) {
    std::lock_guard lock(impl_->mutex); auto it=impl_->peers.find(id);
    if(impl_->closed || !impl_->host || !impl_->sink || it==impl_->peers.end() || !it->second.status.ready || !caps || (caps&~impl_->allowed))return false;
    auto& p=it->second; impl_->Revoke(p,Reason::Revoked,false); p.grant=caps; p.status.grantPending=true; return true;
}
void Service::Revoke(const std::string& id) {
    std::lock_guard lock(impl_->mutex);
    for(auto& [peer,p]:impl_->peers)if(id.empty() || id==peer)impl_->Revoke(p,Reason::Revoked);
}
bool Service::Submit(const std::string& id,Event e) {
    if(!Valid(e) || !Required(e.kind))return false;
    std::lock_guard lock(impl_->mutex); auto it=impl_->peers.find(id);
    if(impl_->closed || impl_->host || it==impl_->peers.end())return false;
    auto& p=it->second;
    if(!p.status.ready || !(p.status.granted&Required(e.kind)) || (!Replaceable(e.kind) && !impl_->Capacity(p)))return false;
    if(e.kind==Kind::Pad)p.lastPad=e;
    impl_->Queue(p,e); return true;
}
void Service::Receive(const std::string& id,bool reliable,std::span<const uint8_t> bytes) {
    auto m=Decode(bytes); if(!m)return;
    std::lock_guard lock(impl_->mutex); auto it=impl_->peers.find(id);
    if(impl_->closed || it==impl_->peers.end())return;
    auto& p=it->second; const auto kind=m->event.kind;
    if(!p.status.ready || m->connection!=p.connection || reliable==Replaceable(kind)) {++p.status.rejected;return;}
    auto& seen=reliable?p.controlSeen:p.stateSeen[StateSlot(kind)];
    if(m->sequence<=seen) {++p.status.rejected;return;}
    if(!impl_->host) {
        if(kind!=Kind::Permission || m->permission<=p.status.permission) {++p.status.rejected;return;}
        std::optional<Event> request;
        for(const auto& queued:p.outgoing)if(queued.event.kind==Kind::Request)request=queued.event;
        seen=m->sequence; impl_->Clear(p); p.status.permission=m->permission; p.status.granted=m->event.capabilities;
        p.status.revokePending=false;
        if(request)impl_->Queue(p,*request);
        p.status.reason=p.status.granted?Reason::None:Reason::Revoked; p.lastSend={};
        impl_->work=true; impl_->wake.notify_one(); return;
    }
    if(kind==Kind::Permission) {++p.status.rejected;return;}
    if(kind==Kind::Request) { seen=m->sequence; p.status.requested=m->event.capabilities; return; }
    if(!p.status.granted || m->permission!=p.status.permission) {++p.status.rejected;return;}
    seen=m->sequence;
    if(kind==Kind::Release) {impl_->Revoke(p,Reason::Revoked);return;}
    Impl::Pending pending{std::move(*m),Clock::now()};
    if(reliable) {if(impl_->Capacity(p))p.incoming.push_back(std::move(pending));}
    else {
        auto& slot=p.incomingState[StateSlot(kind)]; if(slot)++p.status.coalesced; slot=std::move(pending);
    }
}
std::vector<Packet> Service::Drain(const std::string& id,bool reliableWritable,bool stateWritable) {
    std::vector<Packet> result;
    std::lock_guard lock(impl_->mutex); auto it=impl_->peers.find(id);
    if(impl_->closed || it==impl_->peers.end() || !it->second.status.ready)return result;
    auto& p=it->second; const auto now=Clock::now();
    if(!reliableWritable && (p.status.granted || !p.outgoing.empty())) {
        if(p.blocked==Clock::time_point{})p.blocked=now;
        if(now-p.blocked>=100ms)impl_->Revoke(p,Reason::Backpressure);
    } else p.blocked={};
    if(reliableWritable)while(!p.outgoing.empty()) {
        result.push_back({true,Encode(p.outgoing.front())}); p.outgoing.pop_front();
    }
    for(auto& state:p.outgoingState)if(state) {
        if(stateWritable && now-p.lastStateDrain<4ms)continue;
        if(stateWritable)result.push_back({false,Encode(*state)});
        else ++p.status.coalesced;
        state.reset(); // Drop under SCTP pressure; never accumulate state backlog.
    }
    if(std::any_of(result.begin(),result.end(),[](const auto& packet) {return !packet.reliable;}))p.lastStateDrain=now;
    return result;
}
void Service::TransportFailed(const std::string& id) {
    std::lock_guard lock(impl_->mutex); auto it=impl_->peers.find(id);
    if(it!=impl_->peers.end())impl_->Revoke(it->second,Reason::Backpressure);
}
std::vector<Status> Service::Read() const {
    std::lock_guard lock(impl_->mutex); std::vector<Status> result;
    const auto now = Clock::now();
    for(const auto& [id,p]:impl_->peers) {
        auto status = p.status;
        status.reliableQueued = unsigned(p.incoming.size() + p.outgoing.size());
        status.stateQueued = unsigned(std::count_if(p.incomingState.begin(), p.incomingState.end(), [](const auto& value) { return bool(value); }) +
            std::count_if(p.outgoingState.begin(), p.outgoingState.end(), [](const auto& value) { return bool(value); }));
        status.transportBlocked = p.blocked != Clock::time_point{};
        if (p.measured == Clock::time_point{} || now - p.measured >= 1s) { status.queueWaitUs.reset(); status.backendApplyUs.reset(); }
        result.push_back(std::move(status));
    }
    return result;
}
}
