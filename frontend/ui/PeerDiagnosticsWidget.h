#pragma once
#include "api/RoomSession.h"
#include <QWidget>
#include <functional>
#include <chrono>
#include <QByteArray>
class QTableWidget;
// Presentation of immutable snapshots only. No timers, network or media ownership.
class PeerDiagnosticsWidget final : public QWidget {
public:
    explicit PeerDiagnosticsWidget(QWidget* parent = nullptr);
    void Update(const screenshare::v2::RoomStatus&);
private:
    QTableWidget* diagnostics_;
    std::function<void()> refreshDetails_;
    QByteArray controlSnapshot_;
    std::chrono::steady_clock::time_point nextMeasurementRefresh_{};
};
