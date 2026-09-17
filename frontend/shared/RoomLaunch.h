#pragma once
#include "shared/RoomSessionConfig.h"
#include "shared/RoomProfile.h"
#include <QStringList>
#include <QUrl>

// Arguments exclude argv[0]. Explicit opt-in; legacy commands are never guessed
// or partially translated. No network access, persistence or physical devices.
QUrl ParseRoomHomeLaunch(const QStringList& arguments);
RoomSessionConfig ParseRoomCommand(const QStringList& arguments, const RoomProfile* defaults = nullptr,
                                  bool diagnosticLoopback = false);
