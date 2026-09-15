#pragma once
#include "HostPeerRegistry.h"
#include "SignalingExecutor.h"

namespace screenshare::media {
// Construct, invoke and destroy on the supplied signaling executor. The
// executor and capture must outlive this owner. Clients enqueue calls through
// SignalingExecutor; native headers and timers remain inside the implementation.
// Automatically advances deadlines, async peer completions and capture cleanup.
class HostPeerOwner final {
public:
    HostPeerOwner(SignalingExecutor&, HostMediaSession&, uint64_t sessionGeneration);
    ~HostPeerOwner();
    HostPeerOwner(const HostPeerOwner&) = delete;
    HostPeerOwner& operator=(const HostPeerOwner&) = delete;
    bool Add(uint64_t viewer, std::unique_ptr<IMediaPeer>);
    bool RequestRestart(uint64_t viewer, uint64_t generation);
    bool Remove(uint64_t viewer, uint64_t generation);
    std::optional<ManagedPeerSnapshot> snapshot(uint64_t viewer) const;
    void Stop();
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
