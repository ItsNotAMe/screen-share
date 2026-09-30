#pragma once
#include "api/RoomSession.h"
#include <QWidget>
#include <QHash>
#include <QSize>
#include <QStringList>
#include <functional>
class QComboBox;
class QLabel;
class QVBoxLayout;

// A readable view of the existing session snapshot; no network or media work.
class SessionDetailsWidget final : public QWidget {
public:
    explicit SessionDetailsWidget(bool host, QWidget* parent = nullptr);
    void Update(const screenshare::v2::RoomStatus&);
    void SetVideo(QSize size, double framesPerSecond);
    void SelectPeer(const QString&);
    QString SelectedPeer() const;
    std::function<void(const QString&)> selectedPeerChanged;
private:
    void Refresh();
    void Section(QVBoxLayout*, const QString&, const QStringList&, const QStringList&);
    void Value(const QString&, const QString&);
    bool host_;
    QComboBox* peer_ = nullptr;
    QLabel* note_;
    QHash<QString,QLabel*> values_;
    screenshare::v2::RoomStatus status_;
    QSize videoSize_;
    double videoFps_ = 0;
};
