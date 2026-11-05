#pragma once
#include "media/audio/PcmAudioEndpoint.h"
#include "api/audio/audio_device.h"
#include "api/scoped_refptr.h"
#include <atomic>
#include <chrono>

namespace screenshare::media {
struct PcmAudioDiagnostics {
    std::atomic<uint64_t> capturedBlocks{0}, playedBlocks{0}, droppedCaptureFrames{0};
    std::atomic<uint32_t> captureErrors{0}, playoutErrors{0}, playoutBufferFrames{0};
    std::atomic<uint32_t> estimatedCaptureDelayMs{0}, playoutDelayMs{0};
    std::atomic<uint32_t> playoutEnginePeriodUs{0};
    std::atomic<uint64_t> maxPlayoutGapMs{0};
    // Remote media time from the most recently pulled block; -1 is unavailable.
    std::atomic<int64_t> lastPlayoutNtpMs{-1};
};
webrtc::scoped_refptr<webrtc::AudioDeviceModule> CreatePcmAudioDeviceModule(
    PcmEndpointFactories endpoints, std::shared_ptr<PcmAudioDiagnostics> diagnostics);
}
