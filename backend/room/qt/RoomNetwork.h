#pragma once
#include "RoomAdmission.h"
#include <memory>
#include <vector>

namespace screenshare::room::qt {
// Private Qt adapter shared by session coordinators. Owns a dedicated event loop;
// no widgets or caller event pumping. Commands are asynchronous and bounded.
// Requires a process QCoreApplication. Lifetime is externally serialized;
// command producers must finish before destruction begins.
// Drain on the control executor; events carry socket and connection generations.
// Destroy from the external owner after media teardown, never from this loop.
class RoomNetwork final {
public:
    using Socket = uint64_t;
    struct Event { Socket socket; RoomSocket::Event value; };
    explicit RoomNetwork(bool allowPlainLoopback = false);
    ~RoomNetwork();
    RoomNetwork(const RoomNetwork&) = delete;
    RoomNetwork& operator=(const RoomNetwork&) = delete;
    std::future<RoomAdmission::Result> Admit(RoomAdmission::Request);
    // Positive handles increase monotonically across opens, including rejoin;
    // this prevents queued events from referring to a replacement connection.
    std::future<bool> Open(Socket, RoomSocket::Config);
    std::future<RoomSocket::SendResult> Send(Socket, QByteArray);
    // Repeated stops coalesce, even when ordinary commands are at capacity.
    // More than 64 distinct pending stop IDs retires all sockets fail-closed.
    std::shared_future<void> Stop(Socket);
    // Cancels pending admission and closes every socket without joining the
    // event loop. The coordinator can observe completion asynchronously.
    std::shared_future<void> StopAll() { return Stop(0); }
    std::vector<Event> Drain();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
