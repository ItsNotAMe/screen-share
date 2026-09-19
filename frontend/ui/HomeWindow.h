#pragma once


#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtWidgets/QWidget>

#include <functional>

class QLabel;
class QPushButton;
class QVBoxLayout;
class QLineEdit;

struct HomeActiveRoom {
    QString roomId;
    QString name;
    int peerCount = 0;
    bool passwordProtected = false;
    qint64 updatedAt = 0;
    bool joinable = true;
};

class HomeWindow final : public QWidget {
public:
    struct Actions {
        std::function<void()> createRoom;
        std::function<void()> joinRoom;
        std::function<void()> requestRooms;
        std::function<void(const QString&)> openRoom;
    };

    explicit HomeWindow(Actions actions, QWidget* parent = nullptr);
    void refreshRooms();
    void setPushedRooms(const QVector<HomeActiveRoom>&, const QString& unavailable = {});

private:
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
    void updateRooms(const QVector<HomeActiveRoom>& rooms);
    void filterRooms();
    void showRoomStatus(const QString& message);
    QWidget* buildMetric(const QString& value, const QString& label);

    Actions actions_;
    QVBoxLayout* roomListLayout_ = nullptr;
    QLabel* roomStatusLabel_ = nullptr;
    QLabel* roomCountValue_ = nullptr;
    QLabel* peerCountValue_ = nullptr;
    QPushButton* refreshRoomsButton_ = nullptr;
    QLabel* directoryStatus_ = nullptr;
    QLineEdit* search_ = nullptr;
};
