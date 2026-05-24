#pragma once

#include "core/SessionRuntimeControl.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct ScreenShareAppRunContext {
    screenshare::ISessionRuntimeControl* runtimeControl = nullptr;
    std::function<void(std::string_view)> outputHandler;
};

int RunScreenShareApp(int argc, char** argv);
int RunScreenShareApp(int argc, char** argv, const ScreenShareAppRunContext& context);
int RunScreenShareApp(
    const std::vector<std::string>& arguments,
    const ScreenShareAppRunContext& context = {});
