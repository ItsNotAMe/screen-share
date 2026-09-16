#pragma once
#include "media/RoomPeerSignal.h"
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <optional>

namespace screenshare::room::qt {
QByteArray EncodeRoomSignal(const media::RoomPeerSignal&, const QString& room, const QString& target);
// Converts a validated event. Authentication and role authorization must already
// have occurred in RoomSocket; successful structural conversion is not authority.
std::optional<media::RoomPeerSignal> DecodeRoomSignal(const QJsonObject&);
}
