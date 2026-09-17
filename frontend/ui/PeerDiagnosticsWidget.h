#pragma once
#include "api/RoomSession.h"
#include <QWidget>
#include <functional>
class QTableWidget;
// Presentation of immutable snapshots only. No timers, network or media ownership.
class PeerDiagnosticsWidget final : public QWidget {
public:
    explicit PeerDiagnosticsWidget(QWidget* parent = nullptr);
    void Update(const screenshare::v2::RoomStatus&);
private:
    QTableWidget* diagnostics_;
    std::function<void()> refreshDetails_;
};
