#include "media/HostPeerOwner.h"
#include "rtc_base/thread.h"
#include "api/units/time_delta.h"

namespace screenshare::media {
struct HostPeerOwner::State : std::enable_shared_from_this<State> {
    SignalingExecutor& executor;
    HostPeerRegistry peers;
    bool stopped = false;
    State(SignalingExecutor& thread, HostMediaSession& capture, uint64_t generation)
        : executor(thread), peers(capture, generation) {}
    void CheckThread() const {
        if (!executor.IsCurrent()) throw std::logic_error("Peer owner requires its signaling executor");
    }
    void Schedule() {
        webrtc::Thread::Current()->PostDelayedTask([weak = weak_from_this()] {
            if (auto state = weak.lock(); state && !state->stopped) {
                state->peers.Tick(PeerConnectionLifecycle::Clock::now());
                state->Schedule();
            }
        }, webrtc::TimeDelta::Millis(20));
    }
};
HostPeerOwner::HostPeerOwner(SignalingExecutor& executor, HostMediaSession& capture, uint64_t generation) {
    if (!executor.IsCurrent() || !generation)
        throw std::logic_error("Invalid peer owner executor or generation");
    state_ = std::make_shared<State>(executor, capture, generation);
    state_->Schedule();
}
HostPeerOwner::~HostPeerOwner() { Stop(); }
bool HostPeerOwner::Add(uint64_t viewer, std::unique_ptr<IMediaPeer> peer) {
    state_->CheckThread();
    return state_->peers.Add(viewer, std::move(peer));
}
bool HostPeerOwner::RequestRestart(uint64_t viewer, uint64_t generation) {
    state_->CheckThread();
    return state_->peers.RequestRestart(viewer, generation, PeerConnectionLifecycle::Clock::now());
}
bool HostPeerOwner::Remove(uint64_t viewer, uint64_t generation) {
    state_->CheckThread();
    return state_->peers.Remove(viewer, generation);
}
std::optional<ManagedPeerSnapshot> HostPeerOwner::snapshot(uint64_t viewer) const {
    state_->CheckThread();
    return state_->peers.snapshot(viewer);
}
void HostPeerOwner::Stop() {
    state_->CheckThread();
    if (state_->stopped) return;
    // Stop capture and close peers before retiring the scheduler. Pending
    // delayed callbacks hold weak state and cannot revive this owner.
    state_->peers.Stop();
    state_->stopped = true;
}
}
