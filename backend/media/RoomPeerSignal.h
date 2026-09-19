#pragma once
#include "IceCandidateHandoff.h"
#include <string>

namespace screenshare::media {
struct RoomPeerSignal {
    enum class Kind { Offer, Answer, Candidate, RestartRequest } kind;
    std::string connectionId, sdp;
    IceCandidateMessage ice;
};
}
