#pragma once


#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtWidgets/QWidget>

#include <functional>

class QLabel;
class QPushButton;
class QVBoxLayout;
class QLineEdit;
class RoomDirectoryWidget;

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
    Actions actions_;
    RoomDirectoryWidget* rooms_ = nullptr;
};

// Shared list, search, status and row actions used by Home and Join.
class RoomDirectoryWidget final : public QWidget {
public:
    RoomDirectoryWidget(std::function<void()> refresh, std::function<void(const QString&)> join, QWidget* parent = nullptr);
    void setPushedRooms(const QVector<HomeActiveRoom>&, const QString& unavailable = {});
    void refreshRooms();
private:
    QWidget* buildRoomPanel();
    QWidget* buildRoomRow(const HomeActiveRoom&);
    void updateRooms(const QVector<HomeActiveRoom>&);
    void filterRooms();
    void showRoomStatus(const QString&);
    std::function<void()> request_;
    std::function<void(const QString&)> join_;
    QVBoxLayout* roomListLayout_ = nullptr;
    QLabel* roomStatusLabel_ = nullptr;
    QPushButton* refreshRoomsButton_ = nullptr;
    QLabel* directoryStatus_ = nullptr;
    QLineEdit* search_ = nullptr;
};
