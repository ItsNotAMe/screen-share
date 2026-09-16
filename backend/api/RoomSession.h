#pragma once
#include "media/RoomPeerSignal.h"
#include "media/StreamPreferences.h"
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace screenshare::v2 {
struct RoomOptions {
    std::string origin, roomId, nickname, name, password;
    bool host = false, publicRoom = true;
    int viewerLimit = 4;
};
enum class RoomPhase { Idle, Admitting, Connecting, Active, Reconnecting, Stopping, Stopped, Failed };
enum class RoomError { None, Busy, Cancelled, Admission, Transport, Media };
struct RoomResult { RoomError error = RoomError::None; bool outcomeUnconfirmed = false; };
enum class StreamUpdateError { None, Invalid, Busy, Unavailable, Unsupported, Rejected };
struct StreamUpdateResult { StreamUpdateError error = StreamUpdateError::None; uint64_t revision = 0; };
struct PeerStreamStatus {
    std::string peerId;
    uint64_t appliedRevision = 0, observedRevision = 0;
    bool rejected = false;
    int width = 0, height = 0;
};
struct StreamStatus {
    uint64_t requestedRevision = 0;
    media::StreamPreferences preferences;
    std::vector<PeerStreamStatus> peers;
};
struct RoomStatus {
    RoomPhase phase = RoomPhase::Idle;
    RoomError error = RoomError::None;
    uint64_t generation = 0;
    std::string roomId, peerId;
    size_t activePeers = 0, failedPeers = 0, pendingPeers = 0;
    StreamStatus stream;
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
    virtual std::vector<std::string> FailedPeers() const { return {}; }
    virtual StreamUpdateResult UpdateStreamPreferences(const media::StreamPreferences&) { return {StreamUpdateError::Unsupported}; }
    virtual StreamStatus StreamSettings() const { return {}; }
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
    // Host-only. Completion means accepted, not decoded/displayed remotely.
    // Status exposes per-peer sender application and source-frame observation.
    // At most one command is queued; callers may retry Busy with their latest value.
    std::future<StreamUpdateResult> UpdateStreamPreferences(media::StreamPreferences);
    std::shared_future<void> Stop();
    RoomStatus Status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
