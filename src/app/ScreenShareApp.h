#pragma once

#include "core/SessionRuntimeControl.h"

#include <string>
#include <vector>

struct ScreenShareAppRunContext {
    screenshare::ISessionRuntimeControl* runtimeControl = nullptr;
};

int RunScreenShareApp(int argc, char** argv);
int RunScreenShareApp(int argc, char** argv, const ScreenShareAppRunContext& context);
int RunScreenShareApp(
    const std::vector<std::string>& arguments,
    const ScreenShareAppRunContext& context = {});
