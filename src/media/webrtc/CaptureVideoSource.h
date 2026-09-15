#pragma once
#include "media/capture/SyntheticCaptureSource.h"
#include "D3dVideoFrameBuffer.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_broadcaster.h"
#include "pc/video_track_source.h"
#include "rtc_base/time_utils.h"
#include <algorithm>

namespace screenshare::media {
// One wrapper per outbound viewer track. Broadcaster restrictions and sinks
// must never be shared across viewers. Source adaptation policy is a separate
// pending integration; this wrapper preserves owned input dimensions.
class CaptureVideoSource : public webrtc::VideoTrackSource {
public:
    bool is_screencast() const override { return true; }
    std::optional<bool> needs_denoising() const override { return false; }
    void PushBuffer(webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer,
                    std::chrono::steady_clock::time_point capturedAt = std::chrono::steady_clock::now()) {
        if (!buffer) throw std::invalid_argument("Missing video frame buffer");
        broadcaster_.OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(std::move(buffer))
            .set_timestamp_us(Timestamp(capturedAt)).build());
    }
    explicit CaptureVideoSource(std::shared_ptr<screenshare::media::D3dVideoDevice> device = {})
        : VideoTrackSource(false), device_(std::move(device)) {}
    void Push(const SyntheticCaptureResource& frame,
              std::chrono::steady_clock::time_point capturedAt = std::chrono::steady_clock::now()) {
        if (frame.width <= 0 || frame.height <= 0 || frame.width > 3840 || frame.height > 2160 || frame.width % 2 || frame.height % 2 ||
            frame.luma.size() != static_cast<size_t>(frame.width) * frame.height)
            throw std::invalid_argument("Invalid planar capture frame");
        if (device_) {
            std::vector<uint8_t> pixels(frame.luma.size() * 3 / 2, 128);
            std::copy(frame.luma.begin(), frame.luma.end(), pixels.begin());
            auto buffer = device_->UploadNv12(frame.width, frame.height, pixels);
            PushBuffer(std::move(buffer), capturedAt);
            return;
        }
        auto buffer = webrtc::I420Buffer::Create(frame.width, frame.height);
        for (int y = 0; y < frame.height; ++y)
            std::copy_n(frame.luma.data() + y * frame.width, frame.width, buffer->MutableDataY() + y * buffer->StrideY());
        for (int y = 0; y < frame.height / 2; ++y) {
            std::fill_n(buffer->MutableDataU() + y * buffer->StrideU(), frame.width / 2, uint8_t(128));
            std::fill_n(buffer->MutableDataV() + y * buffer->StrideV(), frame.width / 2, uint8_t(128));
        }
        PushBuffer(std::move(buffer), capturedAt);
    }
protected:
    webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override { return &broadcaster_; }
private:
    static int64_t Timestamp(std::chrono::steady_clock::time_point capturedAt) {
        // Translate local monotonic clocks by age, without assuming their epochs
        // match. Preserve acquisition time across handoff and pixel conversion.
        const auto age = std::chrono::steady_clock::now() - capturedAt;
        if (age < std::chrono::steady_clock::duration::zero())
            throw std::invalid_argument("Capture timestamp is in the future");
        return webrtc::TimeMicros() - std::chrono::duration_cast<std::chrono::microseconds>(age).count();
    }
    webrtc::VideoBroadcaster broadcaster_;
    std::shared_ptr<screenshare::media::D3dVideoDevice> device_;
};

}
