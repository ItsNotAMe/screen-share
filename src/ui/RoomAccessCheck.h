#pragma once

#include <QtCore/QString>

#include <functional>

class QObject;

struct RoomAccessCheckResult {
    enum class Status {
        Accepted,
        PasswordRequired,
        WrongPassword,
        Failed,
    };

    Status status = Status::Failed;
    QString message;
};

void startRoomAccessCheck(
    const QString& signalingServerUrl,
    const QString& roomId,
    const QString& roomPassword,
    QObject* receiver,
    std::function<void(RoomAccessCheckResult)> callback);
