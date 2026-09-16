#pragma once
#include "RoomSocket.h"
#include "media/RoomPeerRoster.h"
#include "media/RoomPeerSignal.h"
#include "media/SignalingExecutor.h"
#include <deque>

namespace screenshare::room::qt {
// Signaling-side room/media composition. Feed only authenticated RoomSocket
// events. The coordinator calls Advance automatically, after capture retirement
// has been serviced. Hooks must not reenter this object or throw from Remove.
// Native ownership stays behind Add/Remove/Receive; failed peers remain isolated
// until authoritative leave/rejoin, rather than retrying on profile revisions.
class RoomMediaSession final {
public:
    using Receive = std::function<bool(const std::string&, media::RoomPeerSignal)>;
    enum class State { Waiting, Active, Suspended, Failed, Stopped };
    struct Status {
        State state;
        uint64_t generation;
        size_t activePeers, failedPeers, pendingPeers, queuedEvents, queuedBytes;
    };
    RoomMediaSession(media::SignalingExecutor&, bool host, std::string self,
        media::RoomPeerRoster::Add, media::RoomPeerRoster::Remove, Receive,
        media::RoomPeerRoster::Ready ready = {});
    ~RoomMediaSession();
    void OnEvent(const RoomSocket::Event&);
    void Advance();
    void Stop();
    Status status() const;
    size_t activeCount() const { return status().activePeers; }
    size_t failedCount() const { return status().failedPeers; }
private:
    void CheckThread() const;
    void Fail();
    media::SignalingExecutor& executor_;
    bool host_;
    std::string self_;
    media::RoomPeerRoster roster_;
    Receive receive_;
    struct Pending { RoomSocket::Event event; size_t bytes; };
    std::deque<Pending> pending_;
    size_t bytes_ = 0;
    uint64_t generation_ = 0;
    State state_ = State::Waiting;
};
}
