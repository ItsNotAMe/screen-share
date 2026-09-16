#pragma once
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "render/Nv12VideoFrame.h"
#include "api/video/nv12_buffer.h"
#include "libyuv/planar_functions.h"
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>

// Decoder callbacks never render or wait for the window thread. Retain at most
// one frame; conversion happens only when the presentation owner consumes it.
class LatestRoomVideoFrame final : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    struct Statistics { uint64_t received = 0, replaced = 0, delivered = 0, retained = 0, converted = 0, repacked = 0, rejectedAfterStop = 0; };
    void OnFrame(const webrtc::VideoFrame& frame) override {
        std::lock_guard lock(mutex_);
        if (stopped_) { ++stats_.rejectedAfterStop; return; }
        ++stats_.received;
        if (pending_) ++stats_.replaced;
        pending_ = frame;
    }
    Statistics statistics() const { std::lock_guard lock(mutex_); return stats_; }
    void Stop() { std::lock_guard lock(mutex_); stopped_ = true; pending_.reset(); }
    std::optional<screenshare::Nv12VideoFrame> Take() {
        std::optional<webrtc::VideoFrame> frame;
        { std::lock_guard lock(mutex_); frame.swap(pending_); }
        if (!frame) return {};
        const int width = frame->width(), height = frame->height();
        if (width < 2 || height < 2 || width > 3840 || height > 2160 || width % 2 || height % 2 ||
            frame->rotation() != webrtc::kVideoRotation_0 || frame->timestamp_us() > std::numeric_limits<int64_t>::max() / 10 ||
            frame->timestamp_us() < std::numeric_limits<int64_t>::min() / 10)
            throw std::runtime_error("Unsupported preview dimensions");
        auto buffer = frame->video_frame_buffer();
        webrtc::scoped_refptr<webrtc::NV12Buffer> converted;
        const webrtc::NV12BufferInterface* pixels = nullptr;
        bool conversion = false, repack = false;
        if (buffer->type() == webrtc::VideoFrameBuffer::Type::kNV12) pixels = buffer->GetNV12();
        else {
            auto planar = buffer->ToI420();
            if (!planar) throw std::runtime_error("Preview conversion failed");
            converted = webrtc::NV12Buffer::Copy(*planar); buffer = converted; pixels = converted.get(); conversion = true;
        }
        if (!pixels || pixels->width() != width || pixels->height() != height || pixels->StrideY() < width || pixels->StrideUV() < width ||
            !pixels->DataY() || !pixels->DataUV()) throw std::runtime_error("Invalid preview planes");
        screenshare::Nv12VideoFrame result;
        result.width = result.codedWidth = width; result.height = result.codedHeight = height;
        result.timestamp100ns = frame->timestamp_us() * 10;
        const auto size = static_cast<size_t>(width) * height * 3 / 2;
        if (pixels->StrideY() == width && pixels->StrideUV() == width && pixels->DataUV() == pixels->DataY() + size_t(width) * height) {
            auto owner = std::make_shared<webrtc::scoped_refptr<webrtc::VideoFrameBuffer>>(std::move(buffer));
            result.retainedPixels = std::shared_ptr<const uint8_t>(std::move(owner), pixels->DataY()); result.retainedBytes = size;
        } else {
            result.nv12.resize(size); repack = true;
            if (libyuv::NV12Copy(pixels->DataY(), pixels->StrideY(), pixels->DataUV(), pixels->StrideUV(), result.nv12.data(), width,
                result.nv12.data() + size_t(width) * height, width, width, height)) throw std::runtime_error("Preview packing failed");
        }
        { std::lock_guard lock(mutex_); ++stats_.delivered; stats_.retained += !conversion && !repack; stats_.converted += conversion; stats_.repacked += repack; }
        return result;
    }
private:
    mutable std::mutex mutex_;
    std::optional<webrtc::VideoFrame> pending_;
    Statistics stats_;
    bool stopped_ = false;
};
