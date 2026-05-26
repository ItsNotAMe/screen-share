#include "core/SessionRuntimeControl.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

namespace screenshare {
namespace {

std::string TrimAscii(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool ParsePositiveInt(std::string_view value, int& out)
{
    const std::string text = TrimAscii(value);
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || parsed <= 0 || parsed > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

} // namespace

bool NullSessionRuntimeControl::StopRequested()
{
    return false;
}

std::optional<RuntimeStreamSettingsRequest> NullSessionRuntimeControl::TakeStreamSettingsRequest()
{
    return std::nullopt;
}

std::optional<RuntimeAudioPlaybackSettingsRequest> NullSessionRuntimeControl::TakeAudioPlaybackSettingsRequest()
{
    return std::nullopt;
}

bool MemorySessionRuntimeControl::StopRequested()
{
    std::scoped_lock lock(mutex_);
    return stopRequested_;
}

std::optional<RuntimeStreamSettingsRequest> MemorySessionRuntimeControl::TakeStreamSettingsRequest()
{
    std::scoped_lock lock(mutex_);
    auto request = streamSettingsRequest_;
    streamSettingsRequest_.reset();
    return request;
}

std::optional<RuntimeAudioPlaybackSettingsRequest> MemorySessionRuntimeControl::TakeAudioPlaybackSettingsRequest()
{
    std::scoped_lock lock(mutex_);
    auto request = audioPlaybackSettingsRequest_;
    audioPlaybackSettingsRequest_.reset();
    return request;
}

void MemorySessionRuntimeControl::RequestStop()
{
    std::scoped_lock lock(mutex_);
    stopRequested_ = true;
}

void MemorySessionRuntimeControl::ResetStop()
{
    std::scoped_lock lock(mutex_);
    stopRequested_ = false;
}

void MemorySessionRuntimeControl::RequestStreamSettings(RuntimeStreamSettingsRequest request)
{
    std::scoped_lock lock(mutex_);
    streamSettingsRequest_ = std::move(request);
}

void MemorySessionRuntimeControl::RequestAudioPlaybackSettings(RuntimeAudioPlaybackSettingsRequest request)
{
    std::scoped_lock lock(mutex_);
    audioPlaybackSettingsRequest_ = std::move(request);
}

void MemorySessionRuntimeControl::ClearStreamSettingsRequest()
{
    std::scoped_lock lock(mutex_);
    streamSettingsRequest_.reset();
}

void MemorySessionRuntimeControl::Reset()
{
    std::scoped_lock lock(mutex_);
    stopRequested_ = false;
    streamSettingsRequest_.reset();
    audioPlaybackSettingsRequest_.reset();
}

FileSessionRuntimeControl::FileSessionRuntimeControl(std::string stopFilePath, std::string controlFilePath)
    : stopFilePath_(std::move(stopFilePath)),
      controlFilePath_(std::move(controlFilePath))
{
}

bool FileSessionRuntimeControl::StopRequested()
{
    if (stopFilePath_.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::exists(stopFilePath_, error);
}

std::optional<RuntimeStreamSettingsRequest> FileSessionRuntimeControl::TakeStreamSettingsRequest()
{
    if (controlFilePath_.empty()) {
        return std::nullopt;
    }

    std::error_code error;
    const auto writeTime = std::filesystem::last_write_time(controlFilePath_, error);
    if (error) {
        return std::nullopt;
    }
    if (lastControlWriteTime_ && *lastControlWriteTime_ == writeTime) {
        return std::nullopt;
    }

    std::ifstream file(controlFilePath_, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream content;
    content << file.rdbuf();
    lastControlWriteTime_ = writeTime;
    const std::string text = content.str();
    if (text == lastControlContent_) {
        return std::nullopt;
    }
    lastControlContent_ = text;
    return ParseRuntimeStreamSettingsRequest(text);
}

std::optional<RuntimeAudioPlaybackSettingsRequest> FileSessionRuntimeControl::TakeAudioPlaybackSettingsRequest()
{
    return std::nullopt;
}

std::optional<RuntimeStreamSettingsRequest> ParseRuntimeStreamSettingsRequest(std::string_view content)
{
    std::optional<RuntimeStreamSettingsRequest> request;
    std::istringstream input{std::string(content)};
    std::string line;
    while (std::getline(input, line)) {
        line = TrimAscii(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const std::string lowerLine = LowerAscii(line);
        if (lowerLine.rfind("resolution", 0) != 0) {
            continue;
        }
        const size_t separator = line.find_first_of("= \t");
        if (separator == std::string::npos) {
            continue;
        }
        std::string value = TrimAscii(std::string_view(line).substr(separator + 1));
        if (!value.empty() && value.front() == '=') {
            value = TrimAscii(std::string_view(value).substr(1));
        }

        const std::string lowerValue = LowerAscii(value);
        RuntimeResolutionRequest parsed;
        if (lowerValue == "auto") {
            parsed.mode = RuntimeResolutionMode::Auto;
            if (!request) {
                request.emplace();
            }
            request->resolution = parsed;
            continue;
        }
        if (lowerValue == "native") {
            parsed.mode = RuntimeResolutionMode::Native;
            if (!request) {
                request.emplace();
            }
            request->resolution = parsed;
            continue;
        }

        const size_t x = lowerValue.find('x');
        if (x == std::string::npos) {
            continue;
        }
        int width = 0;
        int height = 0;
        if (!ParsePositiveInt(std::string_view(lowerValue).substr(0, x), width) ||
            !ParsePositiveInt(std::string_view(lowerValue).substr(x + 1), height) ||
            (width % 2) != 0 ||
            (height % 2) != 0) {
            continue;
        }

        parsed.mode = RuntimeResolutionMode::Fixed;
        parsed.width = width;
        parsed.height = height;
        if (!request) {
            request.emplace();
        }
        request->resolution = parsed;
    }
    return request;
}

} // namespace screenshare
