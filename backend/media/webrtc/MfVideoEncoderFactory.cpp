#include "media/webrtc/MfVideoEncoderFactory.h"
#include "media/CodecDiagnostics.h"
#include "media/webrtc/MfHardwareSession.h"
#include "codec/HardwareFrameWait.h"
#include "media/webrtc/MappedVideoBuffer.h"
#include "codec/InputMappingSei.h"
#include "codec/H264StreamEncoder.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/thread.h"
#include "rtc_base/logging.h"
#include "libyuv/convert_from.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <stdexcept>

namespace screenshare::media {
namespace {
struct RetiredVideoFrame {};
class MfVideoEncoder final : public webrtc::VideoEncoder {
public:
    explicit MfVideoEncoder(std::shared_ptr<MfHardwareSession> hardware, int level, std::shared_ptr<DiagnosticHistory> diagnostics)
        : worker_(webrtc::Thread::Create()), hardware_(std::move(hardware)), diagnostics_(std::move(diagnostics)), level_(level) {
        worker_->SetName("MF encoder", nullptr);
        if (!worker_->Start()) throw std::runtime_error("MF encoder worker startup failed");
    }
    ~MfVideoEncoder() override { Release(); worker_->Stop(); }
    int InitEncode(const webrtc::VideoCodec* codec, const Settings&) override {
        if (!codec || codec->codecType != webrtc::kVideoCodecH264 ||
            codec->width < 16 || codec->height < 16 || codec->width > (level_ >= 52 ? 3840 : 1920) || codec->height > (level_ >= 52 ? 2160 : 1080) ||
            codec->width % 2 || codec->height % 2 || codec->maxFramerate == 0 || codec->maxFramerate > 60 ||
            codec->numberOfSimulcastStreams > 1 || codec->startBitrate > 40'000)
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        CancelPending();
        return worker_->BlockingCall([&] {
            hardwareActive_ = false;
            encoder_.reset();
            config_ = {};
            config_.width = codec->width;
            config_.height = codec->height;
            config_.fps = codec->maxFramerate;
            maximumFps_ = codec->maxFramerate;
            config_.bitrate = codec->startBitrate * 1000;
            config_.levelIdc = config_.width > 1920 || config_.height > 1080 ? level_ : 42;
            try {
                encoder_ = std::make_unique<H264StreamEncoder>();
                if (config_.bitrate) StartTransform();
                std::lock_guard lock(mutex_);
                failed_ = false;
                initialized_ = true;
                suspended_ = config_.bitrate == 0;
                keyframe_ = true;
                return WEBRTC_VIDEO_CODEC_OK;
            } catch (const std::exception& error) { CodecFailure(diagnostics_, "encoder-configure-failed", error); encoder_.reset(); return WEBRTC_VIDEO_CODEC_ERROR; }
        });
    }
    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override {
        worker_->BlockingCall([&] { callback_ = callback; });
        return WEBRTC_VIDEO_CODEC_OK;
    }
    int32_t Release() override {
        CancelPending();
        worker_->BlockingCall([&] { encoder_.reset(); hardwareActive_ = false; callback_ = nullptr; });
        return WEBRTC_VIDEO_CODEC_OK;
    }
    int32_t Encode(const webrtc::VideoFrame& frame, const std::vector<webrtc::VideoFrameType>* types) override {
        std::optional<uint32_t> dropped;
        {
            std::lock_guard lock(mutex_);
            if (!initialized_ || !callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
            if (failed_) return WEBRTC_VIDEO_CODEC_ERROR;
            if (frame.width() != config_.width || frame.height() != config_.height)
                return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
            if (types && std::find(types->begin(), types->end(), webrtc::VideoFrameType::kVideoFrameKey) != types->end())
                keyframe_ = true;
            if (suspended_) dropped = frame.rtp_timestamp();
            else {
                if (pending_) dropped = pending_->rtp_timestamp();
                pending_ = frame;
                if (!scheduled_) {
                    scheduled_ = true;
                    const auto generation = generation_;
                    worker_->PostTask([this, generation] { Process(generation); });
                }
            }
        }
        if (dropped) callback_->OnFrameDropped(*dropped, 0, true);
        return WEBRTC_VIDEO_CODEC_OK;
    }
    void SetRates(const RateControlParameters& rates) override {
        const auto bitrate = rates.bitrate.get_sum_bps();
        // Barrier orders the new assignment after the active frame. Zero is a
        // suspension, never a substitute positive bitrate floor.
        worker_->BlockingCall([&] {
            std::optional<uint32_t> dropped;
            {
                std::lock_guard lock(mutex_);
                if (!initialized_) return;
                suspended_ = bitrate == 0;
                if (suspended_ && pending_) { dropped = pending_->rtp_timestamp(); pending_.reset(); }
            }
            if (dropped && callback_) callback_->OnFrameDropped(*dropped, 0, true);
            if (!bitrate) return;
            try {
                // WebRTC permits an unavailable FPS target. Still apply the
                // bitrate (including resume), using InitEncode's FPS as required
                // by RateControlParameters. Clamp before integer conversion.
                const double targetFps = std::isfinite(rates.framerate_fps) && rates.framerate_fps > 0
                    ? rates.framerate_fps : double(maximumFps_);
                const int requestedFps = static_cast<int>(std::round(std::clamp(targetFps, 1.0, 60.0)));
                // This is WebRTC's measured input cadence, not a new user FPS
                // setting. Restarting MF on 54/55 FPS jitter discards rate-control
                // history and emits unnecessary IDRs. Real cadence changes still
                // reconfigure; capture/WebRTC continue to enforce the user cap.
                const int fps = std::abs(requestedFps - config_.fps) >= std::max(3, config_.fps / 10)
                    ? requestedFps : config_.fps;
                const bool restart = !encoder_->isRunning() || fps != config_.fps;
                config_.fps = fps;
                config_.bitrate = bitrate;
                if (restart) { StartTransform(); std::lock_guard lock(mutex_); keyframe_ = true; }
                else if (!encoder_->TryUpdateBitrate(bitrate)) throw std::runtime_error("MF rejected assigned bitrate");
            } catch (const std::exception& error) {
                CodecFailure(diagnostics_, "encoder-rate-update-failed", error);
                if (hardwareActive_) {
                    try { SoftwareFallback(error.what()); } catch (const std::exception& failure) { Fail(failure.what()); }
                } else Fail(error.what());
            }
        });
    }
    EncoderInfo GetEncoderInfo() const override {
        EncoderInfo info;
        info.implementation_name = hardwareActive_ ? "Media Foundation H264 hardware (D3D11/NV12)" :
            "Media Foundation H264 software (CPU I420/NV12)";
        info.is_hardware_accelerated = hardwareActive_;
        info.supports_native_handle = true;
        info.has_trusted_rate_controller = false;
        info.requested_resolution_alignment = 2;
        return info;
    }
private:
    bool Cancelled(uint64_t generation) {
        std::lock_guard lock(mutex_);
        return generation != generation_;
    }
    EncodedPacket HardwarePacket(const CapturedFrame& raw, uint64_t generation) {
        const int64_t timestamp = ++hardwareSampleId_ * 10'000'000 / config_.fps;
        return WaitForHardwareFrame(timestamp,
            [&] {
                if (hardware_->DeviceRetired()) throw std::runtime_error("Encoder graphics device retired");
                return hardware_->PollOutput(*encoder_);
            },
            [&] { return encoder_->TrySubmitHardwareFrame(raw, timestamp); },
            [&] { return Cancelled(generation); });
    }
    void SoftwareFallback(const char* reason) {
        if (diagnostics_) diagnostics_->Event("encoder-software-fallback");
        RTC_LOG(LS_WARNING) << "MF hardware quarantined for this session: " << reason;
        if (hardware_) { hardware_->quarantined = true; ++hardware_->softwareFallbacks; }
        hardwareActive_ = false;
        config_.backend = H264StreamEncoderBackend::Software;
        config_.externalHardwareScheduling = false;
        config_.d3dDevice.Reset();
        encoder_->Start(config_);
        std::lock_guard lock(mutex_);
        keyframe_ = true;
    }
    void StartTransform() {
        uint64_t generation;
        { std::lock_guard lock(mutex_); generation = generation_; }
        hardwareActive_ = false;
        config_.backend = H264StreamEncoderBackend::Software;
        config_.externalHardwareScheduling = false;
        config_.d3dDevice.Reset();
        if (hardware_ && hardware_->device && !hardware_->DeviceRetired() && !hardware_->quarantined) {
            try {
                config_.backend = H264StreamEncoderBackend::Hardware;
                config_.externalHardwareScheduling = true;
                config_.d3dDevice = hardware_->device->device();
                encoder_->Start(config_);
                // Probe actual configured size, two rate assignments and keyframes
                // before reporting hardware capability. Synthetic input only.
                CapturedFrame probe;
                probe.width = config_.width; probe.height = config_.height;
                probe.nv12Pixels.assign(size_t(probe.width) * probe.height * 3 / 2, std::byte{128});
                for (auto rate : {std::max(1u, config_.bitrate / 2), config_.bitrate}) {
                    if (!encoder_->TryUpdateBitrate(rate) || !encoder_->RequestKeyframe())
                        throw std::runtime_error("Hardware rate/keyframe probe rejected");
                    if (!HardwarePacket(probe, generation).isKeyframe) throw std::runtime_error("Hardware keyframe probe failed");
                }
                hardwareActive_ = true;
                if (diagnostics_) diagnostics_->Event("encoder-hardware-ready", config_.width, config_.height);
                return;
            } catch (const HardwareFrameCancelled&) { throw; }
            catch (const std::exception& error) { CodecFailure(diagnostics_, "encoder-hardware-failed", error); SoftwareFallback(error.what()); return; }
        }
        encoder_->Start(config_);
        if (diagnostics_) diagnostics_->Event("encoder-software-ready", config_.width, config_.height);
    }
    CapturedFrame RawFrame(const webrtc::VideoFrame& frame) {
        if (IsRetiredFrame(frame)) throw RetiredVideoFrame{};
        if (hardwareActive_) {
            if (auto* native = dynamic_cast<D3dVideoFrameBuffer*>(Unwrap(frame.video_frame_buffer()).get())) {
                auto raw = native->RetainedNv12();
                if (raw.d3dDevice.Get() == config_.d3dDevice.Get()) return raw;
            }
        }
        auto pixels = frame.video_frame_buffer()->ToI420();
        if (!pixels) throw std::runtime_error("I420 frame unavailable");
        CapturedFrame raw;
        raw.width = raw.sourceWidth = config_.width;
        raw.height = raw.sourceHeight = config_.height;
        raw.nv12Pixels.resize(size_t(raw.width) * raw.height * 3 / 2);
        auto* y = reinterpret_cast<uint8_t*>(raw.nv12Pixels.data());
        if (libyuv::I420ToNV12(pixels->DataY(), pixels->StrideY(), pixels->DataU(), pixels->StrideU(),
            pixels->DataV(), pixels->StrideV(), y, raw.width, y + raw.width * raw.height,
            raw.width, raw.width, raw.height) != 0) throw std::runtime_error("NV12 conversion failed");
        return raw;
    }
    EncodedPacket EncodePacket(const webrtc::VideoFrame& frame, bool keyframe, uint64_t generation) {
        const auto start = std::chrono::steady_clock::now();
        if (Cancelled(generation)) throw HardwareFrameCancelled();
        auto raw = RawFrame(frame);
        if (keyframe && !encoder_->RequestKeyframe()) throw std::runtime_error("MF keyframe request rejected");
        if (hardwareActive_) {
            const auto before = std::chrono::steady_clock::now();
            auto packet = HardwarePacket(raw, generation);
            ++hardware_->hardwareFrames;
            const uint64_t age = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - before).count();
            auto previous = hardware_->maxFrameMicroseconds.load();
            while (previous < age && !hardware_->maxFrameMicroseconds.compare_exchange_weak(previous, age)) {}
            return packet;
        }
        auto packets = encoder_->EncodeFrame(raw);
        if (packets.size() != 1) throw std::runtime_error("MF software retained output");
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(500))
            throw std::runtime_error("MF software encode exceeded 500 ms");
        return std::move(packets.front());
    }
    void CancelPending() {
        std::lock_guard lock(mutex_);
        ++generation_;
        pending_.reset();
        initialized_ = false;
        scheduled_ = false;
    }
    void Fail(const char* error) {
        if (diagnostics_) diagnostics_->Event("encoder-terminal-failure");
        RTC_LOG(LS_WARNING) << "MF encoder failed: " << error;
        std::lock_guard lock(mutex_);
        failed_ = true;
    }
    bool IsRetiredFrame(const webrtc::VideoFrame& frame) {
        const auto* native = dynamic_cast<D3dVideoFrameBuffer*>(Unwrap(frame.video_frame_buffer()).get());
        return native && native->retired();
    }
    void Process(uint64_t generation) {
        std::optional<webrtc::VideoFrame> frame;
        bool keyframe;
        bool failed;
        {
            std::lock_guard lock(mutex_);
            if (generation != generation_) return;
            frame.swap(pending_);
            keyframe = keyframe_;
            keyframe_ = false;
            failed = failed_;
        }
        if (frame) {
            try {
                if (failed) throw std::runtime_error("Encoder requires reinitialization");
                if (hardwareActive_ && hardware_->DeviceRetired()) SoftwareFallback("Capture device retired");
                EncodedPacket packet;
                try { packet = EncodePacket(*frame, keyframe, generation); }
                catch (const HardwareFrameCancelled&) { throw; }
                catch (const std::exception& error) {
                    CodecFailure(diagnostics_, "encoder-frame-fallback", error);
                    if (!hardwareActive_) throw;
                    SoftwareFallback(error.what());
                    packet = EncodePacket(*frame, true, generation);
                }
                if (Cancelled(generation)) throw HardwareFrameCancelled();
                if (IsRetiredFrame(*frame)) throw RetiredVideoFrame{};
                webrtc::EncodedImage image;
                if (const auto* metadata = dynamic_cast<MappedVideoBuffer*>(frame->video_frame_buffer().get()); metadata && metadata->preset) {
                    // Best-effort RTP playout request, not a deadline guarantee.
                    // Quality explicitly restores the upstream adaptive range;
                    // omitting the extension would retain the previous request.
                    // A zero minimum enables zero render timestamps in the
                    // pinned SDK; switching from Quality then starves its
                    // monotonic prerender queue. Keep the smallest nonzero
                    // RTP delay so both presets use the same clock domain.
                    image.SetPlayoutDelay(*metadata->preset == StreamPreset::Gaming
                        ? webrtc::VideoPlayoutDelay(webrtc::TimeDelta::Millis(10), webrtc::TimeDelta::Millis(10))
                        : webrtc::VideoPlayoutDelay{});
                }
                input::InsertMappingSei(packet.bytes,Mapping(frame->video_frame_buffer()));
                image.SetEncodedData(webrtc::EncodedImageBuffer::Create(
                    reinterpret_cast<const uint8_t*>(packet.bytes.data()), packet.bytes.size()));
                image.SetRtpTimestamp(frame->rtp_timestamp());
                image.ntp_time_ms_ = frame->ntp_time_ms();
                image.capture_time_ms_ = frame->timestamp_us() / 1000;
                image._encodedWidth = config_.width;
                image._encodedHeight = config_.height;
                image._frameType = packet.isKeyframe ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
                webrtc::CodecSpecificInfo info{};
                info.codecType = webrtc::kVideoCodecH264;
                info.codecSpecific.H264.packetization_mode = webrtc::H264PacketizationMode::NonInterleaved;
                if (callback_) callback_->OnEncodedImage(image, &info);
            } catch (const RetiredVideoFrame&) {
                // Keep the encoder alive for fresh input from capture recovery.
                // Its first output must replace the receiver's old references.
                { std::lock_guard lock(mutex_); keyframe_ = true; }
                if (callback_) callback_->OnFrameDropped(frame->rtp_timestamp(), 0, true);
            } catch (const HardwareFrameCancelled&) {
                // Release/reset owns retirement. Never quarantine a cancelled GPU.
            } catch (const std::exception& error) {
                CodecFailure(diagnostics_, "encoder-frame-failed", error);
                if (IsRetiredFrame(*frame)) {
                    std::lock_guard lock(mutex_); keyframe_ = true;
                } else Fail(error.what());
                if (callback_) callback_->OnFrameDropped(frame->rtp_timestamp(), 0, true);
            }
        }
        std::lock_guard lock(mutex_);
        if (generation != generation_) return;
        if (pending_) worker_->PostTask([this, generation] { Process(generation); });
        else scheduled_ = false;
    }
    std::unique_ptr<webrtc::Thread> worker_;
    std::shared_ptr<MfHardwareSession> hardware_;
    std::shared_ptr<DiagnosticHistory> diagnostics_;
    std::atomic<bool> hardwareActive_{false};
    int64_t hardwareSampleId_ = 0;
    const int level_;
    int maximumFps_ = 60;
    std::unique_ptr<H264StreamEncoder> encoder_;
    H264StreamEncoderConfig config_;
    webrtc::EncodedImageCallback* callback_ = nullptr;
    std::mutex mutex_;
    std::optional<webrtc::VideoFrame> pending_;
    uint64_t generation_ = 0;
    bool initialized_ = false, scheduled_ = false, suspended_ = true, failed_ = false, keyframe_ = true;
};
}
std::vector<webrtc::SdpVideoFormat> MfVideoEncoderFactory::GetSupportedFormats() const {
    return {{"H264", {{"profile-level-id", "640034"}, {"level-asymmetry-allowed", "1"}, {"packetization-mode", "1"}}}};
}
webrtc::VideoEncoderFactory::CodecSupport MfVideoEncoderFactory::QueryCodecSupport(
    const webrtc::SdpVideoFormat& format, std::optional<std::string> mode, std::optional<webrtc::Resolution> size) const {
    const auto level = format.parameters.find("profile-level-id");
    // IsCodecInList compares profiles, not levels. Until per-level MF limits are
    // implemented, only accept the explicit 1080p and UHD limits below.
    const bool highLevel = level != format.parameters.end() && level->second == "640034";
    const bool levelSupported = highLevel || (level != format.parameters.end() && level->second == "64002a");
    return {levelSupported && (!mode || *mode == "L1T1") && (!size || (size->width >= 16 && size->width <= (highLevel ? 3840 : 1920) &&
        size->height >= 16 && size->height <= (highLevel ? 2160 : 1080) && size->width % 2 == 0 && size->height % 2 == 0)) &&
        format.IsCodecInList(GetSupportedFormats()), false};
}
std::unique_ptr<webrtc::VideoEncoder> MfVideoEncoderFactory::Create(const webrtc::Environment&,
    const webrtc::SdpVideoFormat& format) {
    if (!QueryCodecSupport(format, std::nullopt, std::nullopt).is_supported) return nullptr;
    return std::make_unique<MfVideoEncoder>(hardware_,format.parameters.at("profile-level-id")=="640034"?52:42, diagnostics_);
}
}
