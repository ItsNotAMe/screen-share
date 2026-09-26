#pragma once
#include "RoomNetwork.h"
#include <deque>

namespace screenshare::room::qt {
// Control-thread facade over the owned networking loop. Start/Stop may replace
// a pending subscription; old handles/generations never update the current view.
class RoomDirectory final : public QObject {
public:
    enum class Phase { Stopped, Connecting, Ready, Reconnecting, Failed };
    struct Room { QString id, name, status; int viewers = 0, limit = 0; bool password = false; QString hostNickname; };
    struct Status { Phase phase = Phase::Stopped; uint64_t revision = 0; std::vector<Room> rooms; };
    explicit RoomDirectory(bool diagnosticLoopback = false, QObject* parent = nullptr);
    ~RoomDirectory() override;
    bool Start(QUrl httpsOrigin);
    void Stop();
    bool running() const { return bool(network_) || desired_.has_value(); }
    Status status() const { return status_; }
    uint64_t connectionAttempts() const { return attempts_; }
    std::function<void(const Status&)> changed;
private:
    void Tick();
    void Publish();
    void Fail();
    bool loopback_;
    QTimer timer_;
    std::optional<QUrl> desired_;
    std::unique_ptr<RoomNetwork> network_;
    std::future<bool> opening_;
    std::shared_future<void> stopping_;
    uint64_t handle_ = 0, generation_ = 0, attempts_ = 0;
    std::deque<std::chrono::steady_clock::time_point> reconnects_;
    Status status_;
};
}
