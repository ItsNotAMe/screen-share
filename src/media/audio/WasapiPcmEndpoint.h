#pragma once
#include "media/audio/PcmAudioEndpoint.h"
#include "audio/WasapiCapture.h"

namespace screenshare::media {
PcmEndpointFactories WasapiPcmEndpoints(AudioCaptureConfig capture, std::wstring playbackDeviceId = {});
}
