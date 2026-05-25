#pragma once

#include "core/SessionRuntimeControl.h"

#include <functional>
#include <string_view>

struct ScreenShareAppRunContext {
    screenshare::ISessionRuntimeControl* runtimeControl = nullptr;
    std::function<void(std::string_view)> outputHandler;
};
