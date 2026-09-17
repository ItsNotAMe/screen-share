#pragma once
#include "media/audio/PcmAudioEndpoint.h"

namespace screenshare::media {
// One speech processor per microphone endpoint, with no extra queue/thread.
std::unique_ptr<PcmCaptureEndpoint> ProcessMicrophone(std::unique_ptr<PcmCaptureEndpoint>);
}
