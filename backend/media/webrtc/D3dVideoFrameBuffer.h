#pragma once

#include "api/video/video_frame_buffer.h"
#include "capture/DesktopCapturer.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>

namespace webrtc { class Thread; }
namespace screenshare::media {
class D3dVideoFrameBuffer;
class D3dNv12Scaler;
class GpuScalingBusy : public std::runtime_error {
public: GpuScalingBusy() : std::runtime_error("GPU scaling submission limit reached") {}
};

// Private Windows/WebRTC boundary. Readback/scaling run on this owner; MF and
// presentation may share the multithread-protected device. Published textures
// are never overwritten or returned to a producer pool early.
class D3dVideoDevice : public std::enable_shared_from_this<D3dVideoDevice> {
public:
    D3dVideoDevice();
    explicit D3dVideoDevice(Microsoft::WRL::ComPtr<ID3D11Device> captureDevice);
    ~D3dVideoDevice();
    webrtc::scoped_refptr<D3dVideoFrameBuffer> UploadNv12(int width, int height, std::span<const uint8_t> pixels);
    webrtc::scoped_refptr<D3dVideoFrameBuffer> RetainCapture(const CapturedFrame&);
    ID3D11Device* device() const noexcept { return device_.Get(); }
    void Retire() noexcept { retired_ = true; }
    bool retired() noexcept {
        if (!retired_ && FAILED(device_->GetDeviceRemovedReason())) retired_ = true;
        return retired_;
    }
    uint64_t readbackCount() const noexcept { return readbackCount_; }
    uint64_t readbackMicroseconds() const noexcept { return readbackMicroseconds_; }
    uint64_t scalingFailures() const noexcept { return scalingFailures_; }
    unsigned maximumPendingScales() const noexcept { return maximumPendingScales_; }
private:
    friend class D3dVideoFrameBuffer;
    webrtc::scoped_refptr<webrtc::I420BufferInterface> Readback(ID3D11Texture2D*, int, int);
    webrtc::scoped_refptr<D3dVideoFrameBuffer> Scale(ID3D11Texture2D*, int, int, int, int, int, int);
    std::unique_ptr<webrtc::Thread> owner_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    std::atomic<uint64_t> readbackCount_{0}, readbackMicroseconds_{0};
    std::atomic<bool> retired_{false};
    std::unique_ptr<D3dNv12Scaler> scaler_;
    std::atomic<uint64_t> scalingFailures_{0};
    std::atomic<unsigned> maximumPendingScales_{0};
};

class D3dVideoFrameBuffer : public webrtc::VideoFrameBuffer {
public:
    D3dVideoFrameBuffer(std::shared_ptr<D3dVideoDevice>, Microsoft::WRL::ComPtr<ID3D11Texture2D>, int, int);
    Type type() const override { return Type::kNative; }
    int width() const override { return width_; }
    int height() const override { return height_; }
    webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override;
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> GetMappedFrameBuffer(std::span<Type>) override;
    CapturedFrame RetainedNv12() const;
    // Null means GPU scaling is unavailable; the caller may use ToI420().
    webrtc::scoped_refptr<D3dVideoFrameBuffer> Scale(int width, int height,
        int left, int top, int imageWidth, int imageHeight);
    bool retired() const noexcept { return owner_->retired(); }
private:
    std::shared_ptr<D3dVideoDevice> owner_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    int width_, height_;
    std::mutex mutex_;
    webrtc::scoped_refptr<webrtc::I420BufferInterface> cached_;
};
}
