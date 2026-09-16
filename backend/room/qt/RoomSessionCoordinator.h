#pragma once
#include "RoomNetwork.h"
#include "media/SignalingExecutor.h"

namespace screenshare::room::qt {
// Private composition boundary. Construct/invoke/destroy on signaling; network
// and executor outlive it. No operation blocks. Notifications/Advance execute on
// signaling and may Send, but must not Open/Close/Stop/destroy it or throw.
// One coordinator consumes one RoomNetwork. No other consumer may call Drain.
class RoomSessionCoordinator final {
public:
    struct Stats { size_t peakSendBytes = 0, peakSendOperations = 0; };
    using Notify = std::function<void(const RoomSocket::Event&)>;
    using Advance = std::function<void()>;
    RoomSessionCoordinator(media::SignalingExecutor&, RoomNetwork&, Advance);
    ~RoomSessionCoordinator();
    RoomSessionCoordinator(const RoomSessionCoordinator&) = delete;
    RoomSessionCoordinator& operator=(const RoomSessionCoordinator&) = delete;
    std::future<bool> Open(RoomNetwork::Socket, RoomSocket::Config, Notify);
    // Acceptance queues the send; any later failure becomes an Error event.
    bool Send(RoomNetwork::Socket, QByteArray);
    std::shared_future<void> Close(RoomNetwork::Socket);
    std::shared_future<void> Stop();
    bool failed() const;
    Stats stats() const;
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
