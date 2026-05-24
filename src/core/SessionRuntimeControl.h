#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace screenshare {

enum class RuntimeResolutionMode {
    Auto,
    Native,
    Fixed,
};

struct RuntimeResolutionRequest {
    RuntimeResolutionMode mode = RuntimeResolutionMode::Auto;
    int width = 0;
    int height = 0;
};

class ISessionRuntimeControl {
public:
    virtual ~ISessionRuntimeControl() = default;

    virtual bool StopRequested() = 0;
    virtual std::optional<RuntimeResolutionRequest> TakeResolutionRequest() = 0;
};

class NullSessionRuntimeControl final : public ISessionRuntimeControl {
public:
    bool StopRequested() override;
    std::optional<RuntimeResolutionRequest> TakeResolutionRequest() override;
};

class MemorySessionRuntimeControl final : public ISessionRuntimeControl {
public:
    bool StopRequested() override;
    std::optional<RuntimeResolutionRequest> TakeResolutionRequest() override;

    void RequestStop();
    void ResetStop();
    void RequestResolution(RuntimeResolutionRequest request);
    void ClearResolutionRequest();
    void Reset();

private:
    std::mutex mutex_;
    bool stopRequested_ = false;
    std::optional<RuntimeResolutionRequest> resolutionRequest_;
};

class FileSessionRuntimeControl final : public ISessionRuntimeControl {
public:
    FileSessionRuntimeControl(std::string stopFilePath, std::string controlFilePath);

    bool StopRequested() override;
    std::optional<RuntimeResolutionRequest> TakeResolutionRequest() override;

private:
    std::filesystem::path stopFilePath_;
    std::filesystem::path controlFilePath_;
    std::optional<std::filesystem::file_time_type> lastControlWriteTime_;
    std::string lastControlContent_;
};

std::optional<RuntimeResolutionRequest> ParseRuntimeResolutionRequest(std::string_view content);

} // namespace screenshare
