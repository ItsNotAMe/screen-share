#pragma once
#include <cstdint>
#include <optional>

namespace screenshare::media {
// Receiver-reported decoder statistics. Never a presentation acknowledgement.
struct ReceiverVideoObservation {
    uint32_t width = 0, height = 0, framesDecoded = 0;
    std::optional<uint32_t> fpsMilli;
};
struct ReceiverVideoStatus {
    std::optional<ReceiverVideoObservation> observation;
    bool stale = false;
    std::optional<uint32_t> ageSeconds;
};
}
