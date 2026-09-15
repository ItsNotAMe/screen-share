#pragma once
#include "CaptureVideoSource.h"
#include "api/rtp_sender_interface.h"

namespace screenshare::media {
enum class SettingsApplyError { None, Invalid, StaleRevision, UnsupportedTopology, SenderRejected };
// Coordinator thread only; one instance per viewer generation. Revision success
// means RTP parameters accepted and source settings queued. Source statistics
// separately identify the revision observed while processing frames.
class ViewerStreamSettings final {
public:
    SettingsApplyError Apply(webrtc::RtpSenderInterface& sender, CaptureVideoSource& source,
                             const StreamPreferences& preferences, uint64_t revision) {
        StreamLimits limits{};
        try { limits = ValidateStreamPreferences(preferences); }
        catch (const std::invalid_argument&) { return SettingsApplyError::Invalid; }
        if (!revision || revision <= revision_ || revision <= source.requestedRevision())
            return SettingsApplyError::StaleRevision;
        auto parameters = sender.GetParameters();
        if (parameters.encodings.size() != 1) return SettingsApplyError::UnsupportedTopology;
        auto& encoding = parameters.encodings[0];
        encoding.max_bitrate_bps = limits.maxVideoBitrateBps;
        encoding.min_bitrate_bps.reset(); // Never turn a desired rate into a floor.
        encoding.max_framerate = preferences.fps;
        encoding.scale_resolution_down_by.reset();
        encoding.scale_resolution_down_to.reset();
        switch (limits.degradation) {
        case StreamDegradation::Disabled:
            parameters.degradation_preference = webrtc::DegradationPreference::MAINTAIN_FRAMERATE_AND_RESOLUTION; break;
        case StreamDegradation::MaintainFps:
            parameters.degradation_preference = webrtc::DegradationPreference::MAINTAIN_FRAMERATE; break;
        case StreamDegradation::MaintainResolution:
            parameters.degradation_preference = webrtc::DegradationPreference::MAINTAIN_RESOLUTION; break;
        case StreamDegradation::Balanced:
            parameters.degradation_preference = webrtc::DegradationPreference::BALANCED; break;
        }
        if (!sender.SetParameters(parameters).ok()) return SettingsApplyError::SenderRejected;
        source.Configure(preferences, revision);
        revision_ = revision;
        return SettingsApplyError::None;
    }
    uint64_t revision() const noexcept { return revision_; }
private:
    uint64_t revision_ = 0;
};
}
