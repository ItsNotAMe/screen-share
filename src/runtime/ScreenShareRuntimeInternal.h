#pragma once

#include "runtime/ScreenShareRunContext.h"
#include "runtime/ScreenShareRuntimeOptions.h"
#include "runtime/ScreenShareSessionOptions.h"
#include "runtime/ScreenShareRuntimeSupport.h"
#include "core/ScreenShareSession.h"

namespace screenshare_runtime_internal {

int ExecuteShareSessionConfig(
    const screenshare::ShareSessionConfig& config,
    SavedReportContext& reportContext,
    const ScreenShareRunContext& context);
int ExecuteWatchSessionConfig(
    const screenshare::WatchSessionConfig& config,
    SavedReportContext& reportContext,
    const ScreenShareRunContext& context);

} // namespace screenshare_runtime_internal
