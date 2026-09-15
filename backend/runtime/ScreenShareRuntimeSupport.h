#pragma once

#include "transport/UdpProtocol.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct ScreenShareRunContext;

namespace screenshare_runtime_internal {

struct SavedReportContext {
    std::string sessionId;
    uint64_t sessionFingerprint = 0;
    bool accessCodeRequired = false;
    bool encryptionEnabled = false;
    std::optional<screenshare::udp_protocol::FeedbackSnapshot> latestReceiverFeedback;
};

std::string FormatSessionFingerprint(uint64_t fingerprint);
uint64_t SessionFingerprint(std::string_view sessionId);
std::string GenerateSessionId();

class ScopedCallbackLogRedirect {
public:
    explicit ScopedCallbackLogRedirect(std::function<void(std::string_view)> handler);
    ~ScopedCallbackLogRedirect();

    ScopedCallbackLogRedirect(const ScopedCallbackLogRedirect&) = delete;
    ScopedCallbackLogRedirect& operator=(const ScopedCallbackLogRedirect&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class ScopedLogRedirect {
public:
    explicit ScopedLogRedirect(const std::filesystem::path& path, bool announce = true);
    ~ScopedLogRedirect();

    ScopedLogRedirect(const ScopedLogRedirect&) = delete;
    ScopedLogRedirect& operator=(const ScopedLogRedirect&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

void RemoveFileIfExists(const std::filesystem::path& path);
std::filesystem::path TemporaryReportLogPath(const std::filesystem::path& reportPath);
void WriteSavedReport(
    const std::filesystem::path& outputPath,
    const std::optional<std::filesystem::path>& consoleLogPath,
    const char* argv0,
    int argc,
    char** argv,
    const SavedReportContext& reportContext,
    int exitCode);
std::vector<char*> MutableArgv(std::vector<std::string>& arguments);

} // namespace screenshare_runtime_internal
