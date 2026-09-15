#pragma once
#include "api/video/i420_buffer.h"
#include "api/video/video_frame_buffer.h"
#include "libyuv/convert.h"
#include <mutex>
#include <vector>
#include <stdexcept>

namespace screenshare::media {
// Move-owned software decoder output; I420 is generated only on demand.
class OwnedNv12Buffer : public webrtc::NV12BufferInterface {
public:
    OwnedNv12Buffer(int width, int height, std::vector<std::byte> pixels)
        : width_(width), height_(height), pixels_(std::move(pixels)) {
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || width % 2 || height % 2 ||
            pixels_.size() != size_t(width) * height * 3 / 2) throw std::invalid_argument("Invalid owned NV12 output");
    }
    int width() const override { return width_; }
    int height() const override { return height_; }
    int StrideY() const override { return width_; }
    int StrideUV() const override { return width_; }
    const uint8_t* DataY() const override { return reinterpret_cast<const uint8_t*>(pixels_.data()); }
    const uint8_t* DataUV() const override { return DataY() + size_t(width_) * height_; }
    webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override {
        std::lock_guard lock(mutex_);
        if (!i420_) {
            auto converted = webrtc::I420Buffer::Create(width_, height_);
            if (libyuv::NV12ToI420(DataY(), width_, DataUV(), width_, converted->MutableDataY(), converted->StrideY(),
                converted->MutableDataU(), converted->StrideU(), converted->MutableDataV(), converted->StrideV(), width_, height_) != 0)
                return nullptr;
            i420_ = converted;
        }
        return i420_;
    }
private:
    const int width_, height_;
    const std::vector<std::byte> pixels_;
    std::mutex mutex_;
    webrtc::scoped_refptr<webrtc::I420BufferInterface> i420_;
};
}
