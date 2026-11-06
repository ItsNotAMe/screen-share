#pragma once
#include "CaptureVideoSource.h"
#include "api/rtp_sender_interface.h"
#include <chrono>

namespace screenshare::media {
// Coordinator thread only; one instance per viewer generation. Revision success
// means RTP parameters accepted and source settings queued. Source statistics
// separately identify the revision observed while processing frames.
class ViewerStreamSettings final {
public:
    using Clock = std::chrono::steady_clock;
    // Poll on the coordinator tick. Refresh RTP parameters on each attempt;
    // a newer request supersedes any backoff immediately. Three attempts cap
    // recovery work and keep permanent rejection visible to the caller.
    std::optional<SettingsApplyError> Update(webrtc::RtpSenderInterface& sender, CaptureVideoSource& source,
        const StreamPreferences& preferences, uint64_t revision, Clock::time_point now,
        std::optional<int> videoAllowance = {}) {
        if (revision != attemptedRevision_ || attempts_ == 0) {
            attemptedRevision_ = revision;
            attempts_ = 0;
            pending_ = true;
            retryAt_ = now;
        }
        if (!pending_ || now < retryAt_) return {};
        const auto error = Apply(sender, source, preferences, revision, videoAllowance);
        ++attempts_;
        pending_ = error == SettingsApplyError::SenderRejected && attempts_ < 3;
        retryAt_ = now + std::chrono::milliseconds(250);
        return error;
    }
    bool pending() const noexcept { return pending_; }
    SettingsApplyError Apply(webrtc::RtpSenderInterface& sender, CaptureVideoSource& source,
                             const StreamPreferences& preferences, uint64_t revision, std::optional<int> videoAllowance = {}) {
        StreamLimits limits{};
        try { limits = ValidateStreamPreferences(preferences); }
        catch (const std::invalid_argument&) { return SettingsApplyError::Invalid; }
        if (videoAllowance && (*videoAllowance < 0 || *videoAllowance > limits.maxVideoBitrateBps)) return SettingsApplyError::Invalid;
        if (!revision || revision <= revision_ || revision <= source.requestedRevision())
            return SettingsApplyError::StaleRevision;
        auto parameters = sender.GetParameters();
        if (parameters.encodings.size() != 1) return SettingsApplyError::UnsupportedTopology;
        auto& encoding = parameters.encodings[0];
        const auto allowance = videoAllowance.value_or(limits.maxVideoBitrateBps);
        encoding.active = allowance != 0;
        encoding.max_bitrate_bps = std::max(1000, allowance);
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
        appliedVideoBitrateBps_ = allowance;
        preferences_ = preferences;
        return SettingsApplyError::None;
    }
    uint64_t revision() const noexcept { return revision_; }
    int appliedVideoBitrateBps() const noexcept { return appliedVideoBitrateBps_; }
    const std::optional<StreamPreferences>& preferences() const noexcept { return preferences_; }
private:
    uint64_t attemptedRevision_ = 0;
    unsigned attempts_ = 0;
    bool pending_ = false;
    Clock::time_point retryAt_{};
    uint64_t revision_ = 0;
    int appliedVideoBitrateBps_ = 0;
    std::optional<StreamPreferences> preferences_;
};
}
