#pragma once
#include "PeerConnectionLifecycle.h"
#include "HostMediaSession.h"
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>

namespace screenshare::media {
// Implementations retain the lifecycle object through Close(). All calls occur
// on one signaling/control executor. Restart queues asynchronous negotiation;
// neither method may reenter its owning registry. Close cancels queued work.
class IMediaPeer {
public:
    virtual ~IMediaPeer() = default;
    virtual PeerConnectionLifecycle& lifecycle() noexcept = 0;
    virtual bool RequestIceRestart(uint64_t revision) = 0;
    // Consume ready async completions only; never block or reenter the owner.
    // False/throw isolates a failed completion to this peer.
    virtual bool Poll() { return true; }
    virtual void Close() noexcept = 0;
};
struct ManagedPeerSnapshot {
    uint64_t viewer = 0, generation = 0, restartRevision = 0;
    PeerLifecycleState state = PeerLifecycleState::Connecting;
    PeerLifecycleFailure failure = PeerLifecycleFailure::None;
    bool restartDispatchFailed = false;
    bool operationFailed = false;
    // Lifecycle failure can be observed before the next scheduled Tick closes
    // native peers and starts cleanup. This distinguishes those two phases.
    bool peerClosed = false;
    bool captureCleanupPending = false;
    HostOperationError captureCleanupError = HostOperationError::None;
};
// One host session, one executor. Owns peers until Remove/Stop, including failed
// rows so callers can show their terminal reason. No Qt or WebRTC types escape.
class HostPeerRegistry final {
public:
    ~HostPeerRegistry() { Stop(); }
    HostPeerRegistry() = default;
    // The bound capture owner must outlive this registry. Normal cleanup is
    // asynchronous; Stop joins capture before releasing the remaining peers.
    HostPeerRegistry(HostMediaSession& capture, uint64_t sessionGeneration)
        : capture_(&capture), sessionGeneration_(sessionGeneration) {}
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
            if (entry.closed) { PollCleanup(entry); continue; }
            auto& policy = entry.peer->lifecycle();
            const auto action = policy.Tick(now);
            if (action == PeerLifecycleAction::Close) { CloseEntry(entry); continue; }
            bool progressed = false;
            try { progressed = entry.peer->Poll(); } catch (...) {}
            if (!progressed) { entry.status.operationFailed = true; CloseEntry(entry); continue; }
            if (action == PeerLifecycleAction::RestartIce) {
                bool accepted = false;
                try { accepted = entry.peer->RequestIceRestart(policy.restartRevision()); } catch (...) {}
                if (!accepted) { entry.status.restartDispatchFailed = true; CloseEntry(entry); }
            }
        }
        for (auto it = peers_.begin(); it != peers_.end();) {
            if (it->second.removeRequested && !it->second.status.captureCleanupPending) it = peers_.erase(it);
            else ++it;
        }
    }
    // Success accepts removal. With capture bound, Tick completes it after
    // delivery is joined; wait for snapshot(viewer) to disappear before reuse.
    bool Remove(uint64_t viewer, uint64_t generation) {
        auto it = peers_.find(viewer);
        if (it == peers_.end() || it->second.status.generation != generation) return false;
        it->second.removeRequested = true;
        CloseEntry(it->second);
        if (!it->second.status.captureCleanupPending) peers_.erase(it);
        return true;
    }
    std::optional<ManagedPeerSnapshot> snapshot(uint64_t viewer) const {
        auto it = peers_.find(viewer);
        if (it == peers_.end()) return std::nullopt;
        return it->second.closed ? it->second.status : Read(it->second);
    }
    void Stop() {
        if (stopped_) return;
        if (capture_) {
            // A superseded host generation has already joined its old workers.
            // Stop is a priority command, so queue pressure cannot reject it.
            const auto result = capture_->Stop(sessionGeneration_).get();
            if (result.error != HostOperationError::None && result.error != HostOperationError::StaleGeneration)
                throw std::runtime_error("Capture shutdown did not complete");
        }
        stopped_ = true;
        for (auto& [viewer, entry] : peers_) CloseEntry(entry);
        peers_.clear();
    }
private:
    struct Entry {
        std::unique_ptr<IMediaPeer> peer;
        ManagedPeerSnapshot status;
        bool closed = false;
        bool removeRequested = false;
        std::optional<std::future<HostOperationResult>> cleanup;
    };
    static ManagedPeerSnapshot Read(const Entry& entry) {
        auto result = entry.status;
        const auto& policy = entry.peer->lifecycle();
        result.state = policy.state(); result.failure = policy.failure();
        result.restartRevision = policy.restartRevision();
        return result;
    }
    void PollCleanup(Entry& entry) {
        if (!entry.status.captureCleanupPending) return;
        if (entry.cleanup) {
            if (entry.cleanup->wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            const auto result = entry.cleanup->get();
            entry.cleanup.reset();
            entry.status.captureCleanupError = result.error;
            if (result.error == HostOperationError::None || result.error == HostOperationError::StaleGeneration) {
                entry.status.captureCleanupPending = false;
                return;
            }
            // Capacity/cancellation are retryable. Retain the row and its
            // incarnation until a later Tick confirms cleanup; never block here.
            return;
        }
        entry.cleanup.emplace(capture_->RemoveViewer(sessionGeneration_, entry.status.viewer, entry.status.generation));
    }
    void CloseEntry(Entry& entry) {
        if (entry.closed) return;
        entry.status = Read(entry);
        if (entry.status.restartDispatchFailed || entry.status.operationFailed) entry.status.state = PeerLifecycleState::Failed;
        else if (entry.status.state != PeerLifecycleState::Failed) entry.status.state = PeerLifecycleState::Closed;
        entry.closed = true;
        entry.peer->lifecycle().Close();
        entry.peer->Close();
        entry.status.peerClosed = true;
        entry.status.captureCleanupPending = capture_ && !stopped_;
        PollCleanup(entry);
    }
    std::map<uint64_t, Entry> peers_;
    uint64_t lastGeneration_ = 0;
    bool stopped_ = false;
    HostMediaSession* capture_ = nullptr;
    uint64_t sessionGeneration_ = 0;
};
}
