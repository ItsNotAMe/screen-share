#pragma once
#include "media/audio/PcmAudioEndpoint.h"
#include "media/DiagnosticHistory.h"
#include "api/audio/audio_device.h"
#include "api/scoped_refptr.h"
#include <atomic>
#include <chrono>

namespace screenshare::media {
enum class AudioPlayoutStage : uint8_t { Stopped, BetweenBlocks, CallbackLock, Callback, DeviceWrite };
class AudioPlayoutTiming final {
public:
    struct Snapshot { AudioPlayoutStage stage; uint64_t ageUs; };
    void Enter(AudioPlayoutStage stage) {
        // Publish phase and start time together so a reader cannot combine two
        // different phases. This also exposes a write that has not returned.
        value_ = stage == AudioPlayoutStage::Stopped ? 0 : (NowUs() << 3) | uint64_t(stage);
    }
    Snapshot Read() const {
        const auto value = value_.load(), now = NowUs(), started = value >> 3;
        return {AudioPlayoutStage(value & 7), value && now > started ? now - started : 0};
    }
private:
    static uint64_t NowUs() {
        return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    std::atomic<uint64_t> value_{0};
};
struct PcmAudioDiagnostics {
    std::shared_ptr<DiagnosticHistory> events;
    // Last startup result: 0 not started, 1 MMCSS registered, 2 unavailable.
    std::atomic<unsigned> captureScheduling{0}, playoutScheduling{0};
    std::atomic<uint64_t> capturedBlocks{0}, playedBlocks{0}, droppedCaptureFrames{0};
    std::atomic<uint32_t> captureErrors{0}, playoutErrors{0}, playoutBufferFrames{0};
    std::atomic<uint32_t> estimatedCaptureDelayMs{0}, playoutDelayMs{0};
    std::atomic<uint32_t> playoutEnginePeriodUs{0};
    std::atomic<uint64_t> maxPlayoutGapMs{0};
    std::atomic<uint64_t> maxPlayoutLockWaitUs{0}, maxPlayoutCallbackUs{0}, maxPlayoutWriteUs{0};
    AudioPlayoutTiming playoutTiming;
    // Remote media time from the most recently pulled block; -1 is unavailable.
    std::atomic<int64_t> lastPlayoutNtpMs{-1};
};
webrtc::scoped_refptr<webrtc::AudioDeviceModule> CreatePcmAudioDeviceModule(
    PcmEndpointFactories endpoints, std::shared_ptr<PcmAudioDiagnostics> diagnostics);
}
