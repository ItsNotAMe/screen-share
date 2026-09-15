#pragma once
#include "capture/CaptureSession.h"
#include "capture/CaptureDistributor.h"
#include <future>
#include <memory>
#include <vector>

namespace screenshare::media {
enum class HostMediaState { Idle, Starting, WaitingForViewers, Running, Recovering, Stopping, Stopped, Failed };
enum class HostOperationError { None, InvalidState, StaleGeneration, InvalidViewer, Capacity, Cancelled, Internal };
struct HostOperationResult {
    uint64_t operation = 0, generation = 0;
    HostOperationError error = HostOperationError::None;
};
struct HostViewerSnapshot {
    uint64_t viewer = 0;
    CaptureDeliveryStats delivery;
    uint64_t connectionGeneration = 0;
};
struct HostMediaSnapshot {
    HostMediaState state = HostMediaState::Idle;
    uint64_t generation = 0, sourceGeneration = 0, viewerCount = 0;
    CaptureFailure captureFailure = CaptureFailure::None;
    uint64_t lastFailedViewer = 0;
    std::vector<HostViewerSnapshot> viewers;
    uint64_t activeOperation = 0;
    uint64_t lastFailedConnectionGeneration = 0;
};
// Host capture/membership lifecycle, independent of Qt and WebRTC. Peer creation
// and signaling remain the next integration layer. Lifecycle commands execute
// on one control worker; media stays on capture/per-viewer delivery workers.
// Callers may read snapshots concurrently. Callbacks must not synchronously wait
// on commands that join them (Remove/Stop), or destroy their owning session.
class HostMediaSession final {
public:
    HostMediaSession();
    ~HostMediaSession();
    HostMediaSession(const HostMediaSession&) = delete;
    HostMediaSession& operator=(const HostMediaSession&) = delete;
    std::future<HostOperationResult> Start(CaptureSession::Factory);
    // Admit connections in increasing generation order, matching HostPeerRegistry.
    // Removal is scoped to both host session and viewer connection incarnation.
    std::future<HostOperationResult> AddViewer(uint64_t generation, uint64_t viewer,
                                             uint64_t connectionGeneration, CaptureSession::Deliver);
    std::future<HostOperationResult> RemoveViewer(uint64_t generation, uint64_t viewer,
                                                uint64_t connectionGeneration);
    std::future<HostOperationResult> Stop(uint64_t generation);
    HostMediaSnapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
