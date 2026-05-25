#pragma once

#include "app/ScreenShareRunContext.h"
#include "core/ScreenShareSession.h"
#include "transport/UdpProtocol.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace screenshare_app_internal {

struct SavedReportContext {
    std::string sessionId;
    uint64_t sessionFingerprint = 0;
    bool accessCodeRequired = false;
    bool encryptionEnabled = false;
    std::optional<screenshare::udp_protocol::FeedbackSnapshot> latestReceiverFeedback;
};

int ExecuteShareSessionConfig(
    const screenshare::ShareSessionConfig& config,
    SavedReportContext& reportContext,
    const ScreenShareAppRunContext& context);
int ExecuteWatchSessionConfig(
    const screenshare::WatchSessionConfig& config,
    SavedReportContext& reportContext,
    const ScreenShareAppRunContext& context);
int RunTypedScreenShareSession(
    std::function<int(SavedReportContext&, const ScreenShareAppRunContext&)> executeSession,
    const ScreenShareAppRunContext& context,
    std::string executablePath,
    const char* syntheticCommand,
    const std::string& reportPath);
std::vector<char*> MutableArgv(std::vector<std::string>& arguments);

} // namespace screenshare_app_internal
