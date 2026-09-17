#pragma once
#include "input/v2/InputService.h"
#include <QJsonObject>
#include <QString>
#include <chrono>

// One explicitly selected local command file; never network-controlled. Existing
// contents are ignored at construction so saved commands cannot grant on startup.
class RoomInputCommands final {
public:
    explicit RoomInputCommands(QString path);
    QJsonObject Poll(const std::shared_ptr<screenshare::input::Port>&, bool host);
private:
    QString path_;
    QByteArray previous_;
    uint64_t sequence_ = 0;
    std::chrono::steady_clock::time_point next_{};
};
