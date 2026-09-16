#pragma once
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "codec/H264StreamDecoder.h"
#include "libyuv/convert_from.h"
#include <mutex>
#include <optional>
#include <stdexcept>

// Decoder callbacks never render or wait for the window thread. Retain at most
// one frame; conversion happens only when the presentation owner consumes it.
class LatestRoomVideoFrame final : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    struct Statistics { uint64_t received = 0, replaced = 0; };
    void OnFrame(const webrtc::VideoFrame& frame) override {
        std::lock_guard lock(mutex_);
        ++stats_.received;
        if (pending_) ++stats_.replaced;
        pending_ = frame;
    }
    Statistics statistics() const { std::lock_guard lock(mutex_); return stats_; }
    std::optional<screenshare::DecodedFrameInfo> Take() {
        std::optional<webrtc::VideoFrame> frame;
        { std::lock_guard lock(mutex_); frame.swap(pending_); }
        if (!frame) return {};
        const int width = frame->width(), height = frame->height();
        if (width < 2 || height < 2 || width > 3840 || height > 2160 || width % 2 || height % 2)
            throw std::runtime_error("Unsupported preview dimensions");
        auto pixels = frame->video_frame_buffer()->ToI420();
        if (!pixels) throw std::runtime_error("Preview conversion failed");
        screenshare::DecodedFrameInfo result;
        result.width = result.codedWidth = width; result.height = result.codedHeight = height;
        result.timestamp100ns = frame->timestamp_us() * 10;
        result.data.resize(static_cast<size_t>(width) * height * 3 / 2);
        result.bytes = static_cast<uint32_t>(result.data.size());
        auto* output = reinterpret_cast<uint8_t*>(result.data.data());
        if (libyuv::I420ToNV12(pixels->DataY(), pixels->StrideY(), pixels->DataU(), pixels->StrideU(),
                pixels->DataV(), pixels->StrideV(), output, width, output + width * height, width, width, height))
            throw std::runtime_error("Preview conversion failed");
        return result;
    }
private:
    mutable std::mutex mutex_;
    std::optional<webrtc::VideoFrame> pending_;
    Statistics stats_;
};
