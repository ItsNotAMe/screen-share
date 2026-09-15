#pragma once
#include "media/audio/PcmAudioEndpoint.h"
#include "api/audio/audio_device.h"
#include "api/scoped_refptr.h"
#include <atomic>

namespace screenshare::media {
struct PcmAudioDiagnostics {
    std::atomic<uint64_t> capturedBlocks{0}, playedBlocks{0}, droppedCaptureFrames{0};
    std::atomic<uint32_t> captureErrors{0}, playoutErrors{0}, playoutBufferFrames{0};
    std::atomic<uint32_t> estimatedCaptureDelayMs{0}, playoutDelayMs{0};
    std::atomic<uint32_t> playoutEnginePeriodUs{0};
};
webrtc::scoped_refptr<webrtc::AudioDeviceModule> CreatePcmAudioDeviceModule(
    PcmEndpointFactories endpoints, std::shared_ptr<PcmAudioDiagnostics> diagnostics);
}
