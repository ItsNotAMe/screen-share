#pragma once

#include "runtime/ScreenShareRunContext.h"
#include "core/ScreenShareSession.h"

int RunShareSession(
    const screenshare::ShareSessionConfig& config,
    const ScreenShareRunContext& context = {});
int RunWatchSession(
    const screenshare::WatchSessionConfig& config,
    const ScreenShareRunContext& context = {});
