#pragma once

#include "app/ScreenShareRunContext.h"
#include "core/ScreenShareSession.h"

#include <string>

int RunShareSession(
    const screenshare::ShareSessionConfig& config,
    const ScreenShareAppRunContext& context = {},
    std::string executablePath = "ScreenShare");
int RunWatchSession(
    const screenshare::WatchSessionConfig& config,
    const ScreenShareAppRunContext& context = {},
    std::string executablePath = "ScreenShare");
