#include "media/webrtc/D3dVideoFrameBuffer.h"
#include "D3dNv12Scaler.h"
#include "api/make_ref_counted.h"
#include "api/video/i420_buffer.h"
#include "rtc_base/thread.h"
#include "libyuv/convert.h"
#include <d3d10.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <exception>
#include <optional>
#include <type_traits>

namespace screenshare::media {
namespace {
void Check(HRESULT result) { if (FAILED(result)) throw std::runtime_error("D3D frame operation failed: " + HResultMessage(result)); }
// BlockingCall does not marshal C++ exceptions across the worker boundary.
template<class F> auto OnOwner(webrtc::Thread& owner, F&& work) {
    using Result = std::invoke_result_t<F>;
    std::exception_ptr error;
    if constexpr (std::is_void_v<Result>) {
        owner.BlockingCall([&] { try { work(); } catch (...) { error = std::current_exception(); } });
        if (error) std::rethrow_exception(error);
    } else {
        std::optional<Result> result;
        owner.BlockingCall([&] { try { result = work(); } catch (...) { error = std::current_exception(); } });
        if (error) std::rethrow_exception(error);
        return std::move(*result);
    }
}
}
D3dVideoDevice::D3dVideoDevice() : D3dVideoDevice(nullptr) {}
D3dVideoDevice::D3dVideoDevice(Microsoft::WRL::ComPtr<ID3D11Device> captureDevice) : owner_(webrtc::Thread::Create()) {
    owner_->SetName("D3D frame owner", nullptr);
    if (!owner_->Start()) throw std::runtime_error("D3D owner thread startup failed");
    OnOwner(*owner_, [&] {
        if (captureDevice) {
            device_ = std::move(captureDevice);
            device_->GetImmediateContext(&context_);
        } else {
            D3D_FEATURE_LEVEL level;
            Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &device_, &level, &context_));
        }
        // MF's device manager may use the device from its internal worker threads.
        Microsoft::WRL::ComPtr<ID3D10Multithread> protection;
        Check(device_.As(&protection));
        protection->SetMultithreadProtected(TRUE);
    });
}
webrtc::scoped_refptr<D3dVideoFrameBuffer> D3dVideoDevice::RetainCapture(const CapturedFrame& frame) {
    if (retired()) throw CaptureDeviceLostError(DXGI_ERROR_DEVICE_REMOVED);
    if (!frame.nv12OwnedAndComplete || frame.nv12TextureSubresource != 0 || frame.d3dDevice.Get() != device_.Get())
        throw std::invalid_argument("Capture frame is not completed/owned by this device");
    return webrtc::make_ref_counted<D3dVideoFrameBuffer>(shared_from_this(), frame.nv12Texture, frame.width, frame.height);
}
D3dVideoDevice::~D3dVideoDevice() {
    owner_->Stop();
    // Final release can run on WebRTC's restricted signaling executor. Joining
    // first excludes all context work without a forbidden cross-thread invoke.
    // The scaler owns only free-threaded D3D COM references and bookkeeping;
    // destruction after the join performs no immediate-context operations.
    scaler_.reset();
    readbackStaging_.Reset();
    context_.Reset(); device_.Reset();
}
webrtc::scoped_refptr<D3dVideoFrameBuffer> D3dVideoDevice::UploadNv12(int width, int height, std::span<const uint8_t> pixels) {
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || width % 2 || height % 2 ||
        pixels.size() != size_t(width) * height * 3 / 2) throw std::invalid_argument("Invalid NV12 upload");
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    OnOwner(*owner_, [&] {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width; description.Height = height;
        description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
        description.Format = DXGI_FORMAT_NV12;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = pixels.data(); initial.SysMemPitch = width;
        Check(device_->CreateTexture2D(&description, &initial, &texture));
        // Initial data belongs to the resource on return; no later writes occur.
    });
    return webrtc::make_ref_counted<D3dVideoFrameBuffer>(shared_from_this(), texture, width, height);
}
webrtc::scoped_refptr<webrtc::I420BufferInterface> D3dVideoDevice::Readback(ID3D11Texture2D* texture, int width, int height) {
    return OnOwner(*owner_, [&]() -> webrtc::scoped_refptr<webrtc::I420BufferInterface> {
        if (retired()) return nullptr;
        const auto start = std::chrono::steady_clock::now();
        D3D11_TEXTURE2D_DESC description;
        texture->GetDesc(&description);
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = description.MiscFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        D3D11_TEXTURE2D_DESC current{};
        if (readbackStaging_) readbackStaging_->GetDesc(&current);
        if (!readbackStaging_ || current.Width != description.Width || current.Height != description.Height ||
            current.Format != description.Format) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
            Check(device_->CreateTexture2D(&description, nullptr, &staging));
            readbackStaging_ = std::move(staging);
            ++readbackStagingAllocations_;
        }
        auto output = webrtc::I420Buffer::Create(width, height);
        context_->CopyResource(readbackStaging_.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context_->Map(readbackStaging_.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        const auto* y = static_cast<const uint8_t*>(mapped.pData);
        const int result = libyuv::NV12ToI420(y, mapped.RowPitch, y + size_t(mapped.RowPitch) * height,
            mapped.RowPitch, output->MutableDataY(), output->StrideY(), output->MutableDataU(), output->StrideU(),
            output->MutableDataV(), output->StrideV(), width, height);
        context_->Unmap(readbackStaging_.Get(), 0);
        if (result != 0) throw std::runtime_error("GPU NV12 readback conversion failed");
        ++readbackCount_;
        readbackMicroseconds_ += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        return output;
    });
}
webrtc::scoped_refptr<D3dVideoFrameBuffer> D3dVideoDevice::Scale(ID3D11Texture2D* input,
    int width, int height, int left, int top, int imageWidth, int imageHeight) {
    return OnOwner(*owner_, [&]() -> webrtc::scoped_refptr<D3dVideoFrameBuffer> {
        if (retired() || scalingFailures_) return nullptr;
        try {
            if (!scaler_) scaler_ = std::make_unique<D3dNv12Scaler>(device_.Get());
            auto output = scaler_->Scale(device_.Get(), context_.Get(), input, width, height, left, top, imageWidth, imageHeight);
            maximumPendingScales_ = std::max(maximumPendingScales_.load(), scaler_->pending());
            return webrtc::make_ref_counted<D3dVideoFrameBuffer>(shared_from_this(), std::move(output), width, height);
        } catch (const GpuScalingBusy&) {
            throw; // Backpressure drops a frame; it is neither failure nor CPU fallback.
        } catch (...) {
            // Do not retry a failing shader/driver every frame or every viewer.
            ++scalingFailures_; scaler_.reset(); return nullptr;
        }
    });
}
D3dVideoFrameBuffer::D3dVideoFrameBuffer(std::shared_ptr<D3dVideoDevice> owner,
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture, int width, int height)
    : owner_(std::move(owner)), texture_(std::move(texture)), width_(width), height_(height) {
    if (!owner_ || !texture_ || width <= 0 || height <= 0 || width > 4096 || height > 4096 || width % 2 || height % 2)
        throw std::invalid_argument("Invalid owned GPU frame");
    D3D11_TEXTURE2D_DESC description;
    texture_->GetDesc(&description);
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    texture_->GetDevice(&device);
    if (description.Format != DXGI_FORMAT_NV12 || description.Width != UINT(width) || description.Height != UINT(height) ||
        description.ArraySize != 1 || description.MipLevels != 1 || device.Get() != owner_->device())
        throw std::invalid_argument("GPU frame resource does not match its owner/shape");
}
webrtc::scoped_refptr<webrtc::I420BufferInterface> D3dVideoFrameBuffer::ToI420() {
    std::lock_guard lock(mutex_);
    if (retired()) return nullptr;
    if (!cached_) {
        try { cached_ = owner_->Readback(texture_.Get(), width_, height_); }
        catch (...) { return nullptr; }
    }
    return retired() ? nullptr : cached_;
}
webrtc::scoped_refptr<webrtc::VideoFrameBuffer> D3dVideoFrameBuffer::GetMappedFrameBuffer(std::span<Type> types) {
    if (std::find(types.begin(), types.end(), Type::kI420) == types.end()) return nullptr;
    return ToI420();
}
CapturedFrame D3dVideoFrameBuffer::RetainedNv12() const {
    CapturedFrame frame;
    frame.width = frame.sourceWidth = width_; frame.height = frame.sourceHeight = height_;
    frame.d3dDevice = owner_->device(); frame.nv12Texture = texture_;
    return frame;
}
webrtc::scoped_refptr<D3dVideoFrameBuffer> D3dVideoFrameBuffer::Scale(int width, int height,
    int left, int top, int imageWidth, int imageHeight) {
    if (width < 2 || height < 2 || width > 3840 || height > 2160 || width % 2 || height % 2 ||
        left < 0 || top < 0 || left % 2 || top % 2 || imageWidth < 2 || imageHeight < 2 ||
        imageWidth % 2 || imageHeight % 2 || imageWidth > width || imageHeight > height ||
        left > width - imageWidth || top > height - imageHeight)
        throw std::invalid_argument("Invalid GPU image rectangle");
    return owner_->Scale(texture_.Get(), width, height, left, top, imageWidth, imageHeight);
}
}
