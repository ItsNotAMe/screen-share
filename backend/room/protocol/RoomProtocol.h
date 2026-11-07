#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace screenshare::room::wire {
// Private Qt wire boundary; this does not authorize the decoded command.
struct Validation {
    bool ok = false;
    QString error;
    QJsonObject message;
};
Validation ValidateClientCommand(const QByteArray& bytes, bool directory = false);
Validation ValidateServerEvent(const QByteArray& bytes, bool directory = false);
// Shared entry-form/admission rules. Empty room-name result means invalid.
QString NormalizeRoomName(const QString&);
bool ValidRoomPassword(const QString&);
}
