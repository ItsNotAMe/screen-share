#pragma once
#include "api/field_trials_view.h"

namespace screenshare::media {
// Private policy for the pinned SDK. Its actual parser is covered by the
// engine test; packet tests verify recovery, freshness and viewer isolation.
class MediaNetworkPolicy final : public webrtc::FieldTrialsView {
public:
    std::string Lookup(absl::string_view key) const override {
        // The upstream 350 ms in-flight allowance lets a sudden capacity drop
        // build a large backlog before encoder pushback applies. Retain the
        // upstream RTT-aware window with only 50 ms additional data. Feed its
        // reduced rate to the encoder: drop-only mode can remove at most half
        // the source frames, insufficient for a sharp capacity collapse. This
        // is not a network packet-age deadline or a fixed user bitrate override.
        if (key == "WebRTC-CongestionWindow") return "QueueSize:50,MinBitrate:30000,DropFrame:false";
        // Screen-content defaults pace at 1x with a 2875 ms drain horizon.
        // Permit short bursts at 2.5x the congestion-controlled rate and drain
        // sooner. Allocation/encoder ceilings and congestion feedback remain.
        return key == "WebRTC-ProbingScreenshareBwe" ? "2.5,200,80,40,-60,3" : "";
    }
};
}
