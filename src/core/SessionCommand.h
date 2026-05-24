#pragma once

#include "core/SessionBackend.h"

#include <string>
#include <vector>

namespace screenshare {

std::vector<std::string> BuildShareRoomArguments(const ShareSessionConfig& config);
std::vector<std::string> BuildWatchRoomArguments(const WatchSessionConfig& config);

} // namespace screenshare
