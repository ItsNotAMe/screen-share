#pragma once
#include "api/RoomSession.h"
#include "input/ViewerGamepad.h"
#include "input/v2/GamepadPoller.h"
#include <QWidget>
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class VideoFrameWidget;

class RoomGamepadControl final : public QWidget {
public:
    using Devices = std::function<std::vector<screenshare::ViewerGamepadDevice>()>;
    using Read = std::function<std::optional<screenshare::RemoteGamepadState>(std::string_view)>;
    RoomGamepadControl(bool host, std::function<std::shared_ptr<screenshare::input::Port>()>,
        std::function<screenshare::v2::RoomStatus()>, QWidget* parent,
        Devices = screenshare::ViewerGamepad::ConnectedDevices,
        Read = screenshare::ViewerGamepad::ReadState);
    ~RoomGamepadControl() override;
    void Revoke();
    void SetVideo(VideoFrameWidget*);
    std::function<bool(uint8_t)> prepareGrant;
protected:
    bool eventFilter(QObject*, QEvent*) override;
private:
    void Tick();
    bool host_, armed_ = false;
    std::string requestedPeer_;
    uint64_t requestPermission_ = 0;
    QString actionError_;
    std::function<std::shared_ptr<screenshare::input::Port>()> port_;
    std::function<screenshare::v2::RoomStatus()> room_;
    Read read_;
    QComboBox* peers_;
    QComboBox* devices_;
    QComboBox* capabilities_;
    VideoFrameWidget* video_ = nullptr;
    QCheckBox* consent_;
    QPushButton* action_;
    QLabel* status_;
    std::unique_ptr<screenshare::input::GamepadPoller> poller_;
};
