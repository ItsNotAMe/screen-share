#pragma once
#include "media/DiagnosticHistory.h"
#ifdef _WIN32
#include <Windows.h>
#include <avrt.h>
#endif

namespace screenshare::media {
enum class AudioWorker { Capture = 1, Playout = 2, DeviceCapture = 3 };
// Same per-thread MMCSS policy as the pinned WebRTC Windows audio device.
// Keep creation/destruction on the audio owner; failure must not stop audio.
class AudioThreadTask final {
public:
    AudioThreadTask(AudioWorker worker, const std::shared_ptr<DiagnosticHistory>& history = {}) {
#ifdef _WIN32
        DWORD task = 0;
        handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        const auto error = handle_ ? ERROR_SUCCESS : GetLastError();
        const auto priorityError = handle_ && !AvSetMmThreadPriority(handle_, AVRT_PRIORITY_CRITICAL) ? GetLastError() : ERROR_SUCCESS;
        try {
            if (history) history->TryEvent(handle_ ? "audio-scheduler-ready" : "audio-scheduler-unavailable", int(worker), error);
            if (history && priorityError) history->TryEvent("audio-scheduler-priority-unavailable", int(worker), priorityError);
        } catch (...) {} // Diagnostics must not leak the registration or stop audio.
#endif
    }
    ~AudioThreadTask() {
#ifdef _WIN32
        if (handle_) AvRevertMmThreadCharacteristics(handle_);
#endif
    }
    AudioThreadTask(const AudioThreadTask&) = delete;
    AudioThreadTask& operator=(const AudioThreadTask&) = delete;
    bool active() const {
#ifdef _WIN32
        return handle_ != nullptr;
#else
        return false;
#endif
    }
private:
#ifdef _WIN32
    HANDLE handle_ = nullptr;
#endif
};
}
