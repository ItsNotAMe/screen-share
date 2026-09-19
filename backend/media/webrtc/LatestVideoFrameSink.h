#pragma once
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include <mutex>
#include <optional>

namespace screenshare::media {
// A sink belongs to one session generation. It cannot be restarted after Stop.
// Decode threads retain one frame only; conversion and rendering happen at Take.
class LatestVideoFrameSink final : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    void OnFrame(const webrtc::VideoFrame& frame) override {
        std::lock_guard lock(mutex_);
        if (stopped_) return;
        if (pending_) ++replaced_;
        pending_ = frame;
    }
    std::optional<webrtc::VideoFrame> Take() {
        std::lock_guard lock(mutex_);
        auto result = std::move(pending_); pending_.reset(); return result;
    }
    void Stop() { std::lock_guard lock(mutex_); stopped_ = true; pending_.reset(); }
    uint64_t replaced() const { std::lock_guard lock(mutex_); return replaced_; }
private:
    mutable std::mutex mutex_;
    std::optional<webrtc::VideoFrame> pending_;
    uint64_t replaced_ = 0;
    bool stopped_ = false;
};
}
