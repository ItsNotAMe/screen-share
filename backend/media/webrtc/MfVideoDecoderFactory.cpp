#include "media/webrtc/MfVideoDecoderFactory.h"
#include "media/webrtc/OwnedNv12Buffer.h"
#include "media/webrtc/D3dVideoFrameBuffer.h"
#include "media/webrtc/MappedVideoBuffer.h"
#include "codec/InputMappingSei.h"
#include "api/make_ref_counted.h"

#include "codec/H264StreamDecoder.h"
#include "api/video/i420_buffer.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/thread.h"
#include "rtc_base/logging.h"
#include "libyuv/convert.h"

#include <cstring>
#include <map>
#include <stdexcept>
#include <atomic>
#include <chrono>

namespace screenshare::media {
namespace {
void RequireGpuPresentation(ID3D11Device* device) {
    // Reject decode devices that cannot expose the two NV12 planes to the
    // renderer. Fallback happens before accepting a GPU-only stream.
    D3D11_TEXTURE2D_DESC description{};
    description.Width = description.Height = 16;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_NV12; description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &texture))) throw std::runtime_error("NV12 GPU presentation unavailable");
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels = 1;
    for (auto format : {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM}) {
        view.Format = format;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> plane;
        if (FAILED(device->CreateShaderResourceView(texture.Get(), &view, &plane))) throw std::runtime_error("NV12 GPU plane unavailable");
    }
}
// Hold at most eight published GPU frames per decoder configuration. A slow
// consumer drops output rather than accumulating textures or blocking decoding.
class DecodedGpuBuffer : public D3dVideoFrameBuffer {
public:
    DecodedGpuBuffer(std::shared_ptr<D3dVideoDevice> device, Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
        int width, int height, std::shared_ptr<std::atomic<unsigned>> retained)
        : D3dVideoFrameBuffer(std::move(device), std::move(texture), width, height), retained_(std::move(retained)) { ++*retained_; }
    ~DecodedGpuBuffer() override { --*retained_; }
private:
    std::shared_ptr<std::atomic<unsigned>> retained_;
};
class MfVideoDecoder final : public webrtc::VideoDecoder {
public:
    MfVideoDecoder(bool preferHardware, MfVideoDecoderFactory::DeviceFactory factory, std::shared_ptr<std::atomic<bool>> quarantine)
        : worker_(webrtc::Thread::Create()), preferHardware_(preferHardware), deviceFactory_(std::move(factory)), hardwareQuarantined_(std::move(quarantine)) {
        worker_->SetName("MF decoder", nullptr);
        if (!worker_->Start()) throw std::runtime_error("MF decoder thread startup failed");
    }
    ~MfVideoDecoder() override { Release(); worker_->Stop(); }

    bool Configure(const Settings& settings) override {
        if (settings.codec_type() != webrtc::kVideoCodecH264) return false;
        const auto maximum = settings.max_render_resolution();
        if (maximum.Valid() && (maximum.Width() > 4096 || maximum.Height() > 4096)) return false;
        return worker_->BlockingCall([&] {
            Reset();
            maxWidth_ = maximum.Valid() ? maximum.Width() : 4096;
            maxHeight_ = maximum.Valid() ? maximum.Height() : 4096;
            try {
                decoder_ = std::make_unique<H264StreamDecoder>();
                if (preferHardware_ && !hardwareQuarantined_->load()) {
                    try {
                        gpu_ = deviceFactory_ ? deviceFactory_() : std::make_shared<D3dVideoDevice>();
                        if (!gpu_ || gpu_->retired()) throw std::runtime_error("Decoder GPU unavailable");
                        RequireGpuPresentation(gpu_->device());
                        decoder_->Start(4096, 4096, gpu_->device());
                        hardware_ = true;
                        return true;
                    } catch (const std::exception& error) {
                        *hardwareQuarantined_ = true;
                        RTC_LOG(LS_WARNING) << "MF hardware decoder unavailable: " << error.what();
                        decoder_->Stop(); gpu_.reset();
                    }
                }
                StartTransform();
                return true;
            } catch (...) { Reset(); return false; }
        });
    }

    int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override {
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Decode(const webrtc::EncodedImage& image, int64_t) override {
        if (!callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
        if (!image.size() || image.size() > 16 * 1024 * 1024) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        // WebRTC serializes decoder API calls. MF construction, calls and destruction
        // all run on its owned MTA thread; callbacks run on the calling decode thread.
        std::vector<webrtc::VideoFrame> frames;
        const int result = worker_->BlockingCall([&] {
            if (!decoder_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
            if (needsKeyframe_ && image._frameType != webrtc::VideoFrameType::kVideoFrameKey)
                return WEBRTC_VIDEO_CODEC_ERROR;
            // The pinned receiver initializes max_render_resolution from its
            // first frame and does not necessarily reconfigure on source resize.
            // Permit a bounded keyframe-declared resize, still validating decoded
            // output against that declaration and the global allocation bound.
            if (image._frameType == webrtc::VideoFrameType::kVideoFrameKey && image._encodedWidth && image._encodedHeight) {
                if (image._encodedWidth > 4096 || image._encodedHeight > 4096) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
                maxWidth_ = image._encodedWidth; maxHeight_ = image._encodedHeight;
            }
            try {
                if (restartPending_) {
                    if (std::chrono::steady_clock::now() < retryAfter_) return WEBRTC_VIDEO_CODEC_ERROR;
                    if (restarts_ >= 3) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
                    ++restarts_;
                    StartTransform();
                    restartPending_ = false;
                }
                if (gpu_ && gpu_->retired()) throw std::runtime_error("Decoder device retired");
                if (timestamps_.size() >= 32) throw std::runtime_error("MF decoder retained too many frames");
                EncodedPacket packet;
                packet.timestamp100ns = ++sampleId_ * 100'000;
                packet.duration100ns = 100'000;
                packet.isKeyframe = image._frameType == webrtc::VideoFrameType::kVideoFrameKey;
                packet.bytes.resize(image.size());
                std::memcpy(packet.bytes.data(), image.data(), image.size());
                timestamps_.emplace(packet.timestamp100ns, Timing{image.RtpTimestamp(), image.NtpTimeMs(), maxWidth_, maxHeight_,
                    input::ReadMappingSei({image.data(),image.size()})});
                for (auto& output : decoder_->DecodePacket(packet)) {
                    auto timestamp = timestamps_.find(output.timestamp100ns);
                    if (timestamp == timestamps_.end()) throw std::runtime_error("MF decoder lost timestamp association");
                    if (output.width < 2 || output.height < 2 || output.width > timestamp->second.width || output.height > timestamp->second.height ||
                        (!output.texture && output.data.size() < size_t(output.width) * output.height * 3 / 2))
                        throw std::runtime_error("MF decoder returned invalid visible buffer " + std::to_string(output.width) + "x" +
                            std::to_string(output.height) + " bound " + std::to_string(timestamp->second.width) + "x" + std::to_string(timestamp->second.height));
                    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer;
                    if (output.texture) {
                        if (retained_->load() >= 8) { timestamps_.erase(timestamp); continue; }
                        buffer = webrtc::make_ref_counted<DecodedGpuBuffer>(gpu_, std::move(output.texture), output.width, output.height, retained_);
                    }
                    else buffer = webrtc::make_ref_counted<OwnedNv12Buffer>(output.width, output.height, std::move(output.data));
                    buffer = WithMapping(std::move(buffer),timestamp->second.mapping);
                    frames.push_back(webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer)
                        .set_rtp_timestamp(timestamp->second.rtp).set_ntp_time_ms(timestamp->second.ntp).build());
                    timestamps_.erase(timestamp);
                }
                needsKeyframe_ = false;
                return WEBRTC_VIDEO_CODEC_OK;
            } catch (const std::exception& error) {
                RTC_LOG(LS_WARNING) << "MF decode failed: " << error.what();
                frames.clear();
                timestamps_.clear();
                needsKeyframe_ = true;
                // Retire hardware for this configuration. Rebuild only on a new
                // keyframe, at most three times; corrupt input cannot spin MF up.
                decoder_->Stop();
                if (gpu_) *hardwareQuarantined_ = true;
                gpu_.reset(); hardware_ = false;
                restartPending_ = true;
                retryAfter_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
        });
        for (auto& frame : frames) callback_->Decoded(frame);
        return result;
    }

    int32_t Release() override {
        worker_->BlockingCall([&] { Reset(); });
        callback_ = nullptr;
        return WEBRTC_VIDEO_CODEC_OK;
    }
    DecoderInfo GetDecoderInfo() const override {
        const bool hardware = hardware_;
        return {hardware ? "Media Foundation H264 (D3D11 NV12)" : "Media Foundation H264 (CPU NV12)", hardware};
    }

private:
    void StartTransform() {
        // MF can initially enumerate a default type larger than the stream, then
        // signal a type change. Bound allocation globally; validate visible output
        // against the initial hint or bounded keyframe declaration once returned.
        decoder_->Start(4096, 4096);
    }
    void Reset() {
        decoder_.reset();
        gpu_.reset(); hardware_ = false;
        retained_ = std::make_shared<std::atomic<unsigned>>(0);
        restarts_ = 0; restartPending_ = false;
        timestamps_.clear();
        sampleId_ = 0;
        needsKeyframe_ = true;
    }
    std::unique_ptr<webrtc::Thread> worker_;
    std::unique_ptr<H264StreamDecoder> decoder_;
    bool preferHardware_;
    MfVideoDecoderFactory::DeviceFactory deviceFactory_;
    std::shared_ptr<std::atomic<bool>> hardwareQuarantined_;
    std::shared_ptr<D3dVideoDevice> gpu_;
    std::shared_ptr<std::atomic<unsigned>> retained_ = std::make_shared<std::atomic<unsigned>>(0);
    std::atomic<bool> hardware_{false};
    unsigned restarts_ = 0;
    bool restartPending_ = false;
    std::chrono::steady_clock::time_point retryAfter_{};
    // Decoder output can lag an incoming resize keyframe. Validate against the
    // declaration attached to that output, never the newest stream dimensions.
    struct Timing { uint32_t rtp; int64_t ntp; int width, height; input::FrameMapping mapping; };
    std::map<int64_t, Timing> timestamps_;
    int maxWidth_ = 4096;
    int maxHeight_ = 4096;
    int64_t sampleId_ = 0;
    bool needsKeyframe_ = true;
    webrtc::DecodedImageCallback* callback_ = nullptr;
};
}

std::vector<webrtc::SdpVideoFormat> MfVideoDecoderFactory::GetSupportedFormats() const {
    return {{"H264", {{"profile-level-id", "64002a"}, {"level-asymmetry-allowed", "1"}, {"packetization-mode", "1"}}},
        {"H264", {{"profile-level-id", "42e01f"}, {"level-asymmetry-allowed", "1"}, {"packetization-mode", "1"}}}};
}

webrtc::VideoDecoderFactory::CodecSupport MfVideoDecoderFactory::QueryCodecSupport(
    const webrtc::SdpVideoFormat& format, bool scaling, std::optional<webrtc::Resolution> resolution) const {
    const bool sizeSupported = !resolution || (resolution->width > 0 && resolution->height > 0 &&
        resolution->width <= 4096 && resolution->height <= 4096);
    return {!scaling && sizeSupported && format.IsCodecInList(GetSupportedFormats()), false};
}

std::unique_ptr<webrtc::VideoDecoder> MfVideoDecoderFactory::Create(
    const webrtc::Environment&, const webrtc::SdpVideoFormat& format) {
    if (!QueryCodecSupport(format, false, std::nullopt).is_supported) return nullptr;
    return std::make_unique<MfVideoDecoder>(preferHardware_, deviceFactory_, hardwareQuarantined_);
}
} // namespace screenshare::media
