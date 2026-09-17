#pragma once
#include "media/capture/SyntheticCaptureSource.h"
#include "media/StreamPreferences.h"
#include "media/SourceVideoStatus.h"
#include "D3dVideoFrameBuffer.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_broadcaster.h"
#include "api/video/video_adapter.h"
#include "pc/video_track_source.h"
#include "rtc_base/time_utils.h"
#include <algorithm>
#include <mutex>

namespace screenshare::media {
// One wrapper per outbound viewer track. Broadcaster restrictions and sinks
// must never be shared across viewers. Configure is coordinator-thread safe;
// Push calls remain serialized on this viewer's delivery worker. Unconfigured
// sources preserve input for low-level probes; production applies preferences.
using SourceSettingsStats = SourceVideoStatus;
class CaptureVideoSource : public webrtc::VideoTrackSource {
public:
    bool is_screencast() const override { return true; }
    std::optional<bool> needs_denoising() const override { return false; }
    void Configure(const StreamPreferences& preferences, uint64_t revision) {
        ValidateStreamPreferences(preferences);
        std::lock_guard lock(settingsMutex_);
        if (!revision || revision <= revision_) throw std::invalid_argument("Stale source settings revision");
        preferences_ = preferences; revision_ = revision;
    }
    uint64_t requestedRevision() const { std::lock_guard lock(settingsMutex_); return revision_; }
    SourceSettingsStats settingsStats() const { std::lock_guard lock(settingsMutex_); return stats_; }
    void PushBuffer(webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer,
                    std::chrono::steady_clock::time_point capturedAt = std::chrono::steady_clock::now()) {
        if (!buffer) throw std::invalid_argument("Missing video frame buffer");
        const auto timestamp = Timestamp(capturedAt);
        buffer = Adapt(std::move(buffer), timestamp);
        if (!buffer) return;
        broadcaster_.OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(std::move(buffer))
            .set_timestamp_us(timestamp).build());
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
    static std::pair<int, int> Fit(int width, int height, int maximumWidth, int maximumHeight, bool upscale) {
        double scale = std::min(double(maximumWidth) / width, double(maximumHeight) / height);
        if (!upscale) scale = std::min(1.0, scale);
        return {std::max(2, int(width * scale) & ~1), std::max(2, int(height * scale) & ~1)};
    }
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> Adapt(
        webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer, int64_t timestamp) {
        std::optional<StreamPreferences> preferences;
        uint64_t revision;
        { std::lock_guard lock(settingsMutex_); preferences = preferences_; revision = revision_; }
        if (!preferences) return buffer;
        const auto& value = *preferences;
        auto wants = broadcaster_.wants();
        wants.requested_resolution.reset(); // This wrapper owns canvas selection.
        wants.aggregates.reset();
        if (value.resolution != ResolutionMode::Auto) {
            wants.max_pixel_count = std::numeric_limits<int>::max();
            wants.target_pixel_count.reset();
            wants.resolution_alignment = 2; // MF handles coded-size alignment.
        }
        if (value.fpsMode == SettingMode::Manual) wants.max_framerate_fps = value.fps;
        adapter_.OnSinkWants(wants);
        int canvasWidth = buffer->width(), canvasHeight = buffer->height();
        std::optional<int> maximumPixels;
        if (value.resolution == ResolutionMode::Fixed) {
            canvasWidth = value.width; canvasHeight = value.height;
        } else if (value.resolution == ResolutionMode::Auto) {
            const auto fitted = Fit(canvasWidth, canvasHeight, value.width, value.height, false);
            maximumPixels = fitted.first * fitted.second;
        }
        // This upstream call resets the frame-rate controller. Invoke it only
        // when the source limit changes, not once per frame.
        if (outputFps_ != value.fps || outputPixels_ != maximumPixels) {
            adapter_.OnOutputFormatRequest(std::nullopt, maximumPixels, value.fps);
            outputFps_ = value.fps; outputPixels_ = maximumPixels;
        }
        int cropWidth, cropHeight, width, height;
        if (!adapter_.AdaptFrameResolution(canvasWidth, canvasHeight, timestamp * 1000,
                                           &cropWidth, &cropHeight, &width, &height)) {
            std::lock_guard lock(settingsMutex_); ++stats_.dropped;
            return nullptr;
        }
        if (value.resolution != ResolutionMode::Auto && (width != canvasWidth || height != canvasHeight))
            throw std::runtime_error("Fixed output canvas is unsupported");
        const auto fitted = Fit(buffer->width(), buffer->height(), width, height, true);
        const int left = ((width - fitted.first) / 2) & ~1;
        const int top = ((height - fitted.second) / 2) & ~1;
        auto scalingPath = SourceScalingPath::Unchanged;
        if (width != buffer->width() || height != buffer->height()) {
            const bool gpu = buffer->type() == webrtc::VideoFrameBuffer::Type::kNative;
            webrtc::scoped_refptr<D3dVideoFrameBuffer> gpuOutput;
            try {
                if (auto* native = dynamic_cast<D3dVideoFrameBuffer*>(buffer.get()))
                    gpuOutput = native->Scale(width, height, left, top, fitted.first, fitted.second);
            } catch (const GpuScalingBusy&) {
                std::lock_guard lock(settingsMutex_); ++stats_.dropped; ++stats_.gpuBusyDrops; return nullptr;
            }
            if (gpuOutput) {
                buffer = std::move(gpuOutput); scalingPath = SourceScalingPath::Gpu;
            } else {
                scalingPath = gpu ? SourceScalingPath::CpuReadback : SourceScalingPath::Cpu;
                auto pixels = buffer->ToI420();
                if (!pixels) {
                    std::lock_guard lock(settingsMutex_); ++stats_.dropped; return nullptr;
                }
                auto scaled = webrtc::I420Buffer::Create(fitted.first, fitted.second);
                scaled->ScaleFrom(*pixels);
                if (fitted.first == width && fitted.second == height) buffer = std::move(scaled);
                else {
                    auto canvas = webrtc::I420Buffer::Create(width, height);
                    for (int y = 0; y < height; ++y) std::fill_n(canvas->MutableDataY() + y * canvas->StrideY(), width, uint8_t(16));
                    for (int y = 0; y < height / 2; ++y) {
                        std::fill_n(canvas->MutableDataU() + y * canvas->StrideU(), width / 2, uint8_t(128));
                        std::fill_n(canvas->MutableDataV() + y * canvas->StrideV(), width / 2, uint8_t(128));
                    }
                    for (int y = 0; y < fitted.second; ++y)
                        std::copy_n(scaled->DataY() + y * scaled->StrideY(), fitted.first, canvas->MutableDataY() + (y + top) * canvas->StrideY() + left);
                    for (int y = 0; y < fitted.second / 2; ++y) {
                        std::copy_n(scaled->DataU() + y * scaled->StrideU(), fitted.first / 2, canvas->MutableDataU() + (y + top / 2) * canvas->StrideU() + left / 2);
                        std::copy_n(scaled->DataV() + y * scaled->StrideV(), fitted.first / 2, canvas->MutableDataV() + (y + top / 2) * canvas->StrideV() + left / 2);
                    }
                    buffer = std::move(canvas);
                }
            }
            std::lock_guard lock(settingsMutex_); ++stats_.scaled;
            stats_.gpuReadbackFallbacks += scalingPath == SourceScalingPath::CpuReadback;
            stats_.gpuScaled += scalingPath == SourceScalingPath::Gpu;
        }
        {
            std::lock_guard lock(settingsMutex_);
            stats_.observedRevision = revision; stats_.width = width; stats_.height = height;
            stats_.imageLeft = left; stats_.imageTop = top; stats_.imageWidth = fitted.first; stats_.imageHeight = fitted.second;
            stats_.scalingPath = scalingPath;
        }
        return buffer;
    }
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
    webrtc::VideoAdapter adapter_{2};
    std::optional<int> outputPixels_;
    int outputFps_ = 0;
    mutable std::mutex settingsMutex_;
    std::optional<StreamPreferences> preferences_;
    uint64_t revision_ = 0;
    SourceSettingsStats stats_;
};

}
