#pragma once
#include "shared/RoomSessionConfig.h"
#include "shared/LatestRoomVideoFrame.h"
#include <QObject>
#include <QTimer>

// UI-thread owner. Native callbacks only publish into the latest-frame sink.
// Normal Stop is asynchronous; callers keep this owner alive until finished.
class QtRoomSession final : public QObject {
public:
    using Factory = std::function<screenshare::v2::RoomRuntimeFactory(screenshare::media::WindowsRoomRuntimeOptions)>;
    explicit QtRoomSession(QObject* parent = nullptr, Factory factory = screenshare::media::WindowsRoomRuntimeFactory,
                           bool diagnosticLoopback = false);
    ~QtRoomSession() override;
    bool start(RoomSessionConfig);
    void stop();
    void apply(screenshare::media::StreamPreferences);
    bool running() const { return bool(session_); }
    bool settingsPending() const { return pending_.has_value() || applying_.valid(); }
    screenshare::v2::RoomStatus status() const;
    std::function<void(const screenshare::v2::RoomStatus&)> statusChanged;
    std::function<void(screenshare::DecodedFrameInfo)> frameReady;
    std::function<void(const screenshare::v2::StreamUpdateResult&)> settingsAccepted;
    std::function<void(const QString&)> error;
    std::function<void(const screenshare::v2::RoomStatus&)> finished;
private:
    void tick();
    Factory factory_;
    bool loopback_;
    QTimer timer_;
    RoomSessionConfig config_;
    std::unique_ptr<screenshare::v2::RoomSession> session_;
    std::shared_ptr<LatestRoomVideoFrame> frames_;
    std::future<screenshare::v2::RoomResult> admission_;
    std::future<screenshare::v2::StreamUpdateResult> applying_;
    std::shared_future<void> stopping_;
    std::optional<screenshare::media::StreamPreferences> pending_, submitted_;
    std::chrono::steady_clock::time_point started_, nextStatus_;
    size_t nextChange_ = 0;
    screenshare::v2::RoomStatus last_;
};
