#pragma once
#include "media/RoomPeerSignal.h"
#include <functional>
#include <future>
#include <memory>
#include <string>

namespace screenshare::v2 {
struct RoomOptions {
    std::string origin, roomId, nickname, name, password;
    bool host = false, publicRoom = true;
    int viewerLimit = 4;
};
enum class RoomPhase { Idle, Admitting, Connecting, Active, Reconnecting, Stopping, Stopped, Failed };
enum class RoomError { None, Busy, Cancelled, Admission, Transport, Media };
struct RoomResult { RoomError error = RoomError::None; bool outcomeUnconfirmed = false; };
struct RoomStatus {
    RoomPhase phase = RoomPhase::Idle;
    RoomError error = RoomError::None;
    uint64_t generation = 0;
    std::string roomId, peerId;
    size_t activePeers = 0, failedPeers = 0, pendingPeers = 0;
};
// Private media implementations are injected without leaking Qt/WebRTC types
// into the public control API. All runtime methods execute on owned signaling.
// BeginStop must drain capture/delivery asynchronously before destruction.
// It must return a valid shared future. Advance continues during that drain;
// Ready and Remove must not throw. RoomSend is signaling-only and reports queue
// acceptance; subsequent transport failure is handled by the session owner.
class RoomRuntime {
public:
    virtual ~RoomRuntime() = default;
    virtual void Advance() = 0;
    virtual bool Ready(const std::string&) = 0;
    virtual bool Add(const std::string&) = 0;
    virtual void Remove(const std::string&) noexcept = 0;
    virtual bool Receive(const std::string&, media::RoomPeerSignal) = 0;
    virtual std::shared_future<void> BeginStop() = 0;
};
struct RoomIdentity { bool host; std::string roomId, peerId; };
using RoomSend = std::function<bool(const std::string&, media::RoomPeerSignal)>;
using RoomRuntimeFactory = std::function<std::unique_ptr<RoomRuntime>(const RoomIdentity&, RoomSend)>;
// One session incarnation. Requires process QCoreApplication and application
// media/SSL initialization. Start/Stop/Status may be called from external threads.
// Stop is coalesced and completes after networking and media drain. Destroy on
// an external owner thread after command producers stop; never in runtime hooks.
// Credentials remain private. Create a new object for a new admission; ambiguous
// admission is never retried automatically.
// Start completes on the first authenticated room snapshot, not first media.
class RoomSession final {
public:
    explicit RoomSession(RoomRuntimeFactory, bool diagnosticLoopback = false);
    ~RoomSession();
    RoomSession(const RoomSession&) = delete;
    RoomSession& operator=(const RoomSession&) = delete;
    std::future<RoomResult> Start(RoomOptions);
    std::shared_future<void> Stop();
    RoomStatus Status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
