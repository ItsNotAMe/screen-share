#pragma once

#include <string>
#include <vector>

struct ScreenShareRunContext;

int RunScreenShareCli(int argc, char** argv);
int RunScreenShareCli(int argc, char** argv, const ScreenShareRunContext& context);
int RunScreenShareCli(const std::vector<std::string>& arguments);
int RunScreenShareCli(
    const std::vector<std::string>& arguments,
    const ScreenShareRunContext& context);
