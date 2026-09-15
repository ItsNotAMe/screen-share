#pragma once
#include "api/video/video_frame.h"
#include "api/video/nv12_buffer.h"
#include "render/Nv12D3D11Presenter.h"
#include "PresentationRecovery.h"
#include "libyuv/planar_functions.h"
#include <vector>
#include <stdexcept>

namespace screenshare::media {
// All calls, construction and destruction belong to the target window thread.
// Decode callbacks use LatestVideoFrameSink and never call this directly.
class Nv12VideoPresenter {
public:
    explicit Nv12VideoPresenter(HWND window) : owner_(GetCurrentThreadId()) {
        if (!window || GetWindowThreadProcessId(window, nullptr) != owner_)
            throw std::invalid_argument("Presenter must run on the target window thread");
        presenter_.SetLowLatency(true); presenter_.Attach(window);
    }
    bool Present(const webrtc::VideoFrame& frame) {
        CheckOwner();
        if (frame.width() <= 0 || frame.height() <= 0 || frame.width() > 4096 || frame.height() > 4096 ||
            frame.width() % 2 || frame.height() % 2 || frame.rotation() != webrtc::kVideoRotation_0)
            throw std::invalid_argument("Unsupported presentation shape/rotation");
        auto buffer = frame.video_frame_buffer();
        webrtc::scoped_refptr<webrtc::NV12Buffer> converted;
        const webrtc::NV12BufferInterface* nv12 = nullptr;
        if (buffer->type() == webrtc::VideoFrameBuffer::Type::kNV12) nv12 = buffer->GetNV12();
        else {
            auto i420 = buffer->ToI420();
            if (!i420) throw std::runtime_error("Presentation CPU fallback failed");
            converted = webrtc::NV12Buffer::Copy(*i420); nv12 = converted.get(); ++conversions_;
        }
        const int width = frame.width(), height = frame.height();
        const size_t bytes = size_t(width) * height * 3 / 2;
        const uint8_t* data = nv12->DataY();
        if (nv12->StrideY() != width || nv12->StrideUV() != width || nv12->DataUV() != data + size_t(width) * height) {
            packed_.resize(bytes);
            if (libyuv::NV12Copy(data, nv12->StrideY(), nv12->DataUV(), nv12->StrideUV(),
                packed_.data(), width, packed_.data() + size_t(width) * height, width, width, height) != 0)
                throw std::runtime_error("Presentation plane packing failed");
            data = packed_.data(); ++repacks_;
        }
        return recovery_.Present([&] { return presenter_.TryPresent({width, height, data, bytes}); },
            [&] { presenter_.Reset(); });
    }
    void Clear() { CheckOwner(); presenter_.Clear(); }
    uint64_t presented() const { CheckOwner(); return presenter_.framesPresented(); }
    uint64_t conversions() const { CheckOwner(); return conversions_; }
    uint64_t repacks() const { CheckOwner(); return repacks_; }
    uint64_t recoveries() const { CheckOwner(); return recovery_.recoveries(); }
    bool hardware() const { CheckOwner(); return presenter_.isHardwareAccelerated(); }
    uint32_t maximumFrameLatency() const { CheckOwner(); return presenter_.maximumFrameLatency(); }
private:
    void CheckOwner() const { if (GetCurrentThreadId() != owner_) throw std::logic_error("Wrong presentation thread"); }
    DWORD owner_;
    Nv12D3D11Presenter presenter_;
    PresentationRecovery recovery_;
    std::vector<uint8_t> packed_;
    uint64_t conversions_ = 0, repacks_ = 0;
};
}
