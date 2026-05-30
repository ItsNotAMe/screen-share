#include "ui/RoomAccessCheck.h"

#include "transport/SignalingClient.h"

#include <QtCore/QByteArray>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <exception>
#include <string>
#include <thread>

namespace {

std::string toStdUtf8(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<size_t>(bytes.size()));
}

std::string lowercaseCopy(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

RoomAccessCheckResult resultFromException(const std::exception& error)
{
    const std::string message = error.what();
    const std::string lower = lowercaseCopy(message);

    if (lower.find("invalid_room_password") != std::string::npos ||
        lower.find("password is incorrect") != std::string::npos ||
        lower.find("wrong password") != std::string::npos) {
        return {
            RoomAccessCheckResult::Status::WrongPassword,
            QStringLiteral("Room password is wrong.")};
    }

    if (lower.find("room_password_required") != std::string::npos ||
        lower.find("requires a password") != std::string::npos ||
        lower.find("password required") != std::string::npos) {
        return {
            RoomAccessCheckResult::Status::PasswordRequired,
            QStringLiteral("Room password required.")};
    }

    return {
        RoomAccessCheckResult::Status::Failed,
        QString::fromStdString(message)};
}

void deliverResult(
    const QPointer<QObject>& receiver,
    std::function<void(RoomAccessCheckResult)> callback,
    RoomAccessCheckResult result)
{
    if (receiver.isNull()) {
        return;
    }

    QObject* target = receiver.data();
    QMetaObject::invokeMethod(
        target,
        [receiver, callback = std::move(callback), result = std::move(result)]() mutable {
            if (!receiver.isNull() && callback) {
                callback(std::move(result));
            }
        },
        Qt::QueuedConnection);
}

} // namespace

void startRoomAccessCheck(
    const QString& signalingServerUrl,
    const QString& roomId,
    const QString& roomPassword,
    QObject* receiver,
    std::function<void(RoomAccessCheckResult)> callback)
{
    const QPointer<QObject> target(receiver);
    const std::string server = toStdUtf8(signalingServerUrl);
    const std::string room = toStdUtf8(roomId);
    const std::string password = toStdUtf8(roomPassword);

    std::thread([target, server, room, password, callback = std::move(callback)]() mutable {
        RoomAccessCheckResult result;
        try {
            screenshare::SignalingClientConfig config;
            config.serverUrl = server;
            config.timeout = std::chrono::milliseconds(5000);
            screenshare::SignalingClient client(std::move(config));
            client.Peers(room, "ui-preflight", password);
            result.status = RoomAccessCheckResult::Status::Accepted;
        } catch (const std::exception& error) {
            result = resultFromException(error);
        } catch (...) {
            result = {
                RoomAccessCheckResult::Status::Failed,
                QStringLiteral("Could not verify room access.")};
        }

        deliverResult(target, std::move(callback), std::move(result));
    }).detach();
}
