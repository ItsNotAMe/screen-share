#pragma once

#include "core/ScreenShareSession.h"

#include <string>
#include <vector>

namespace screenshare {

std::vector<std::string> BuildShareArguments(const ShareSessionConfig& config);
std::vector<std::string> BuildWatchArguments(const WatchSessionConfig& config);

} // namespace screenshare
