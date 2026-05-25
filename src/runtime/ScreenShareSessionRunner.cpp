#include "runtime/ScreenShareSessionRunner.h"

#include "runtime/ScreenShareRuntimeInternal.h"

#include <utility>

int RunShareSession(
    const screenshare::ShareSessionConfig& config,
    const ScreenShareRunContext& context,
    std::string executablePath)
{
    return screenshare_runtime_internal::RunTypedScreenShareSession(
        [config](
            screenshare_runtime_internal::SavedReportContext& reportContext,
            const ScreenShareRunContext& runContext) {
            return screenshare_runtime_internal::ExecuteShareSessionConfig(config, reportContext, runContext);
        },
        context,
        std::move(executablePath),
        "--typed-share-session",
        config.reportPath);
}

int RunWatchSession(
    const screenshare::WatchSessionConfig& config,
    const ScreenShareRunContext& context,
    std::string executablePath)
{
    return screenshare_runtime_internal::RunTypedScreenShareSession(
        [config](
            screenshare_runtime_internal::SavedReportContext& reportContext,
            const ScreenShareRunContext& runContext) {
            return screenshare_runtime_internal::ExecuteWatchSessionConfig(config, reportContext, runContext);
        },
        context,
        std::move(executablePath),
        "--typed-watch-session",
        config.reportPath);
}
