#include "media/webrtc/MfVideoDecoderFactory.h"
#include "media/webrtc/OwnedNv12Buffer.h"
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

namespace screenshare::media {
namespace {
// Explicit software/CPU fallback. No hardware or zero-copy claim is made.
class MfVideoDecoder final : public webrtc::VideoDecoder {
public:
    MfVideoDecoder() : worker_(webrtc::Thread::Create()) {
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
            try {
                if (timestamps_.size() >= 32) throw std::runtime_error("MF decoder retained too many frames");
                EncodedPacket packet;
                packet.timestamp100ns = ++sampleId_ * 100'000;
                packet.duration100ns = 100'000;
                packet.isKeyframe = image._frameType == webrtc::VideoFrameType::kVideoFrameKey;
                packet.bytes.resize(image.size());
                std::memcpy(packet.bytes.data(), image.data(), image.size());
                timestamps_.emplace(packet.timestamp100ns, Timing{image.RtpTimestamp(), image.NtpTimeMs()});
                for (auto& output : decoder_->DecodePacket(packet)) {
                    auto timestamp = timestamps_.find(output.timestamp100ns);
                    if (timestamp == timestamps_.end()) throw std::runtime_error("MF decoder lost timestamp association");
                    if (output.width > maxWidth_ || output.height > maxHeight_ ||
                        output.data.size() < size_t(output.width) * output.height * 3 / 2)
                        throw std::runtime_error("MF decoder returned invalid visible buffer");
                    auto buffer = webrtc::make_ref_counted<OwnedNv12Buffer>(output.width, output.height, std::move(output.data));
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
                try { StartTransform(); } catch (...) { decoder_.reset(); }
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
    DecoderInfo GetDecoderInfo() const override { return {"Media Foundation H264 (CPU NV12)", false}; }

private:
    void StartTransform() {
        // MF can initially enumerate a default type larger than the stream, then
        // signal a type change. Bound allocation globally; validate visible output
        // against WebRTC's negotiated limit once an actual frame is returned.
        decoder_->Start(4096, 4096);
    }
    void Reset() {
        decoder_.reset();
        timestamps_.clear();
        sampleId_ = 0;
        needsKeyframe_ = true;
    }
    std::unique_ptr<webrtc::Thread> worker_;
    std::unique_ptr<H264StreamDecoder> decoder_;
    struct Timing { uint32_t rtp; int64_t ntp; };
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
    return std::make_unique<MfVideoDecoder>();
}
} // namespace screenshare::media
