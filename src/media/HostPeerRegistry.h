#pragma once
#include "PeerConnectionLifecycle.h"
#include <map>
#include <memory>
#include <optional>

namespace screenshare::media {
// Implementations retain the lifecycle object through Close(). All calls occur
// on one signaling/control executor. Restart queues asynchronous negotiation;
// neither method may reenter its owning registry. Close cancels queued work.
class IMediaPeer {
public:
    virtual ~IMediaPeer() = default;
    virtual PeerConnectionLifecycle& lifecycle() noexcept = 0;
    virtual bool RequestIceRestart(uint64_t revision) = 0;
    virtual void Close() noexcept = 0;
};
struct ManagedPeerSnapshot {
    uint64_t viewer = 0, generation = 0, restartRevision = 0;
    PeerLifecycleState state = PeerLifecycleState::Connecting;
    PeerLifecycleFailure failure = PeerLifecycleFailure::None;
    bool restartDispatchFailed = false;
};
// One host session, one executor. Owns peers until Remove/Stop, including failed
// rows so callers can show their terminal reason. No Qt or WebRTC types escape.
class HostPeerRegistry final {
public:
    ~HostPeerRegistry() { Stop(); }
    HostPeerRegistry() = default;
    HostPeerRegistry(const HostPeerRegistry&) = delete;
    HostPeerRegistry& operator=(const HostPeerRegistry&) = delete;
    bool Add(uint64_t viewer, std::unique_ptr<IMediaPeer> peer) {
        if (!peer) return false;
        const auto generation = peer->lifecycle().generation();
        if (stopped_ || !viewer || peers_.contains(viewer) || peers_.size() >= 63 || generation <= lastGeneration_) {
            peer->Close(); return false;
        }
        lastGeneration_ = generation;
        peers_.emplace(viewer, Entry{std::move(peer), {viewer, generation}});
        return true;
    }
    bool RequestRestart(uint64_t viewer, uint64_t generation, PeerConnectionLifecycle::Time now) {
        auto it = peers_.find(viewer);
        return it != peers_.end() && !it->second.closed && it->second.status.generation == generation &&
               it->second.peer->lifecycle().RequestRestart(generation, now);
    }
    void Tick(PeerConnectionLifecycle::Time now) {
        for (auto& [viewer, entry] : peers_) {
            if (entry.closed) continue;
            auto& policy = entry.peer->lifecycle();
            const auto action = policy.Tick(now);
            if (action == PeerLifecycleAction::Close) { CloseEntry(entry); continue; }
            if (action == PeerLifecycleAction::RestartIce) {
                bool accepted = false;
                try { accepted = entry.peer->RequestIceRestart(policy.restartRevision()); } catch (...) {}
                if (!accepted) { entry.status.restartDispatchFailed = true; CloseEntry(entry); }
            }
        }
    }
    bool Remove(uint64_t viewer, uint64_t generation) {
        auto it = peers_.find(viewer);
        if (it == peers_.end() || it->second.status.generation != generation) return false;
        CloseEntry(it->second); peers_.erase(it); return true;
    }
    std::optional<ManagedPeerSnapshot> snapshot(uint64_t viewer) const {
        auto it = peers_.find(viewer);
        if (it == peers_.end()) return std::nullopt;
        return it->second.closed ? it->second.status : Read(it->second);
    }
    void Stop() noexcept {
        stopped_ = true;
        for (auto& [viewer, entry] : peers_) CloseEntry(entry);
        peers_.clear();
    }
private:
    struct Entry {
        std::unique_ptr<IMediaPeer> peer;
        ManagedPeerSnapshot status;
        bool closed = false;
    };
    static ManagedPeerSnapshot Read(const Entry& entry) {
        auto result = entry.status;
        const auto& policy = entry.peer->lifecycle();
        result.state = policy.state(); result.failure = policy.failure();
        result.restartRevision = policy.restartRevision();
        return result;
    }
    static void CloseEntry(Entry& entry) noexcept {
        if (entry.closed) return;
        entry.status = Read(entry);
        if (entry.status.restartDispatchFailed) entry.status.state = PeerLifecycleState::Failed;
        else if (entry.status.state != PeerLifecycleState::Failed) entry.status.state = PeerLifecycleState::Closed;
        entry.closed = true;
        entry.peer->lifecycle().Close();
        entry.peer->Close();
    }
    std::map<uint64_t, Entry> peers_;
    uint64_t lastGeneration_ = 0;
    bool stopped_ = false;
};
}
