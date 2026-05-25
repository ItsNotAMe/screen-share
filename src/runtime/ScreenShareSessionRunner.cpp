#include "runtime/ScreenShareSessionRunner.h"

#include "runtime/ScreenShareRuntimeInternal.h"
#include "runtime/ScreenShareRuntimeSupport.h"

#include <filesystem>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

int RunTypedScreenShareSession(
    std::function<int(
        screenshare_runtime_internal::SavedReportContext&,
        const ScreenShareRunContext&)> executeSession,
    const ScreenShareRunContext& context,
    std::string executablePath,
    const char* syntheticCommand,
    const std::string& reportPath)
{
    using namespace screenshare_runtime_internal;

    std::unique_ptr<ScopedCallbackLogRedirect> callbackLogRedirect;
    if (context.outputHandler) {
        callbackLogRedirect = std::make_unique<ScopedCallbackLogRedirect>(context.outputHandler);
    }
    std::unique_ptr<ScopedLogRedirect> logRedirect;
    const std::optional<std::filesystem::path> saveReportPath =
        reportPath.empty() ? std::optional<std::filesystem::path>{} : std::filesystem::path(reportPath);
    std::optional<std::filesystem::path> capturedLogPath;
    bool capturedLogIsTemporary = false;
    SavedReportContext reportContext;
    reportContext.sessionId = GenerateSessionId();
    reportContext.sessionFingerprint = SessionFingerprint(reportContext.sessionId);
    int exitCode = 0;

    try {
        if (saveReportPath) {
            capturedLogPath = TemporaryReportLogPath(*saveReportPath);
            capturedLogIsTemporary = true;
            RemoveFileIfExists(*capturedLogPath);
            logRedirect = std::make_unique<ScopedLogRedirect>(*capturedLogPath, false);
            std::cout << "Saving run report to " << saveReportPath->string() << "\n";
        }

        exitCode = executeSession(reportContext, context);
    } catch (const std::invalid_argument& error) {
        std::cerr << "Error: " << error.what() << "\n";
        exitCode = 1;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        exitCode = 1;
    }

    logRedirect.reset();

    if (saveReportPath) {
        try {
            std::vector<std::string> reportArguments{std::move(executablePath), syntheticCommand};
            std::vector<char*> reportArgv = MutableArgv(reportArguments);
            WriteSavedReport(
                *saveReportPath,
                capturedLogPath,
                reportArgv.front(),
                static_cast<int>(reportArgv.size()),
                reportArgv.data(),
                reportContext,
                exitCode);
            std::cout << "Run report saved to " << saveReportPath->string() << "\n";
        } catch (const std::exception& error) {
            std::cerr << "Failed to save run report: " << error.what() << "\n";
            exitCode = 1;
        }
    }

    if (capturedLogIsTemporary && capturedLogPath) {
        RemoveFileIfExists(*capturedLogPath);
    }

    return exitCode;
}

} // namespace

int RunShareSession(
    const screenshare::ShareSessionConfig& config,
    const ScreenShareRunContext& context,
    std::string executablePath)
{
    return RunTypedScreenShareSession(
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
    return RunTypedScreenShareSession(
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
