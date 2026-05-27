#pragma once

#include "core/ScreenShareSession.h"
#include "core/SessionRuntimeControl.h"

#include <functional>
#include <string_view>

struct ScreenShareRunContext {
    screenshare::ISessionRuntimeControl* runtimeControl = nullptr;
    std::function<void(std::string_view)> outputHandler;
    std::function<void(screenshare::SessionEvent::VideoFrame)> videoFrameHandler;
};
