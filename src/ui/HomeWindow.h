#pragma once

#include "ui/WatchSessionUiState.h"

#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtWidgets/QWidget>

#include <functional>

class QLabel;
class QNetworkAccessManager;
class QPushButton;
class QVBoxLayout;

struct HomeActiveRoom {
    QString roomId;
    QString name;
    int peerCount = 0;
    bool passwordProtected = false;
    qint64 updatedAt = 0;
};

class HomeWindow final : public QWidget {
public:
    struct Actions {
        std::function<void()> createRoom;
        std::function<void()> joinRoom;
        std::function<void(const WatchSessionUiState&)> quickJoinRoom;
    };

    explicit HomeWindow(Actions actions, QWidget* parent = nullptr);

private:
    QWidget* buildTopBar();
    QWidget* buildMainMenu();
    QWidget* buildActionPanel(
        const char* iconName,
        const QString& title,
        const QString& detail,
        const QString& buttonObjectName,
        std::function<void()> action);
    QWidget* buildRoomPanel();
    QWidget* buildRoomRow(
        const HomeActiveRoom& room);
    void refreshRooms();
    void updateRooms(const QVector<HomeActiveRoom>& rooms);
    void showRoomStatus(const QString& message);
    QWidget* buildMetric(const QString& value, const QString& label);

    Actions actions_;
    QNetworkAccessManager* roomNetwork_ = nullptr;
    QVBoxLayout* roomListLayout_ = nullptr;
    QLabel* roomStatusLabel_ = nullptr;
    QLabel* roomCountValue_ = nullptr;
    QLabel* peerCountValue_ = nullptr;
    QPushButton* refreshRoomsButton_ = nullptr;
};
