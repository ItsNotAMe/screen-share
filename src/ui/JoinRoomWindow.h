#pragma once

#include "core/ScreenShareSession.h"
#include "ui/QtSessionBackend.h"

#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtWidgets/QWidget>

#include <functional>

class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QPushButton;
class QSpinBox;
class QVBoxLayout;

struct JoinRoomInfo {
    QString roomId;
    QString name;
    int peerCount = 0;
    bool passwordProtected = false;
    qint64 updatedAt = 0;
};

class JoinRoomWindow final : public QWidget {
public:
    struct Actions {
        std::function<void()> back;
    };

    explicit JoinRoomWindow(QtSessionBackend* backend, Actions actions, QWidget* parent = nullptr);

    void refreshRooms();

private:
    QWidget* buildShell();
    QWidget* buildHeader();
    QWidget* buildRoomsSection();
    QWidget* buildRoomRow(const JoinRoomInfo& room);
    QWidget* buildLinkSection();
    QWidget* buildAdvancedSection();
    QPushButton* iconButton(const QString& text, const QString& objectName, const char* iconName);
    QLabel* textLabel(const QString& text, const char* objectName);
    QLabel* iconLabel(const char* iconName, int size, const QString& color = QStringLiteral("#eaf5f2"));

    void updateRooms(const QVector<JoinRoomInfo>& rooms);
    void showRoomStatus(const QString& message);
    void pasteRoomLink();
    void joinRoom(const JoinRoomInfo& room);
    void joinRoomLink();
    void startWatch(const QString& roomId, const QString& password);
    QString promptPassword(const QString& roomName, bool* accepted);
    void installBackendHandlers();
    screenshare::WatchSessionConfig currentConfig(const QString& roomId, const QString& password) const;
    void setStatus(const QString& text, const QString& objectName);

    QtSessionBackend* backend_ = nullptr;
    Actions actions_;
    QNetworkAccessManager* roomNetwork_ = nullptr;
    QVBoxLayout* roomListLayout_ = nullptr;
    QLabel* roomStatusLabel_ = nullptr;
    QLineEdit* roomLinkEdit_ = nullptr;
    QSpinBox* listenPortSpin_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* linkJoinButton_ = nullptr;
};
