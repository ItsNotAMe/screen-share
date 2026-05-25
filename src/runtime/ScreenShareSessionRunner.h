#pragma once

#include "runtime/ScreenShareRunContext.h"
#include "core/ScreenShareSession.h"

#include <string>

int RunShareSession(
    const screenshare::ShareSessionConfig& config,
    const ScreenShareRunContext& context = {},
    std::string executablePath = "ScreenShare");
int RunWatchSession(
    const screenshare::WatchSessionConfig& config,
    const ScreenShareRunContext& context = {},
    std::string executablePath = "ScreenShare");
