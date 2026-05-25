#include "app/ScreenShareSessionRunner.h"

#include "app/ScreenShareAppInternal.h"

#include <utility>

int RunShareSession(
    const screenshare::ShareSessionConfig& config,
    const ScreenShareAppRunContext& context,
    std::string executablePath)
{
    return screenshare_app_internal::RunTypedScreenShareSession(
        [config](
            screenshare_app_internal::SavedReportContext& reportContext,
            const ScreenShareAppRunContext& runContext) {
            return screenshare_app_internal::ExecuteShareSessionConfig(config, reportContext, runContext);
        },
        context,
        std::move(executablePath),
        "--typed-share-session",
        config.reportPath);
}

int RunWatchSession(
    const screenshare::WatchSessionConfig& config,
    const ScreenShareAppRunContext& context,
    std::string executablePath)
{
    return screenshare_app_internal::RunTypedScreenShareSession(
        [config](
            screenshare_app_internal::SavedReportContext& reportContext,
            const ScreenShareAppRunContext& runContext) {
            return screenshare_app_internal::ExecuteWatchSessionConfig(config, reportContext, runContext);
        },
        context,
        std::move(executablePath),
        "--typed-watch-session",
        config.reportPath);
}
