#pragma once

#include <string>
#include <vector>

struct ScreenShareAppRunContext;

int RunScreenShareApp(int argc, char** argv);
int RunScreenShareApp(int argc, char** argv, const ScreenShareAppRunContext& context);
int RunScreenShareApp(const std::vector<std::string>& arguments);
int RunScreenShareApp(
    const std::vector<std::string>& arguments,
    const ScreenShareAppRunContext& context);
