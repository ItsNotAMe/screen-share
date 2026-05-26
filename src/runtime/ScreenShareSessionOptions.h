#pragma once

#include "core/ScreenShareSession.h"
#include "runtime/ScreenShareRuntimeOptions.h"
#include "transport/NatTraversal.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace screenshare_runtime_internal {

std::string ParseSessionId(const char* value);
std::string ParseAccessCode(const char* value);

bool HasUdpSession(const Options& options);
bool HasSignalingCommand(const Options& options);
bool HasSignalingOptions(const Options& options);
bool HasNatShareTarget(const Options& options);
bool LooksLikeNatInvite(std::string_view text);
std::string FormatNatEndpoint(const screenshare::NatInviteEndpoint& endpoint);
uint64_t NatProbeSessionFingerprint(const Options& options);

struct SelectedNatInviteEndpoint {
    screenshare::NatInviteEndpoint endpoint;
    const char* name = "public";
};

SelectedNatInviteEndpoint SelectNatInviteEndpoint(
    const screenshare::NatInvite& invite,
    InviteEndpointPreference preference);
screenshare::NatInvite ParseValidatedLocalInviteForShare(
    const Options& options,
    const std::string& inviteText,
    const char* optionName);
screenshare::NatInvite ParseValidatedPeerInvite(const Options& options);

Options BuildShareSessionOptions(
    const screenshare::ShareSessionConfig& config,
    std::string defaultSessionId);
Options BuildWatchSessionOptions(
    const screenshare::WatchSessionConfig& config,
    std::string defaultSessionId);

} // namespace screenshare_runtime_internal
