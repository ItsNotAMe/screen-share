#pragma once
#include "api/video_codecs/video_encoder_factory.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "rtc_base/time_utils.h"
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>

// Single-viewer, same-process diagnostic only. No image readback, network
// extension, logging or clock synchronization is added to the media pipeline.
struct PipelineTrace {
    enum Stage { EncoderInput, Encoded, DecoderInput, Decoded, Consumed };
    struct Frame { double at[5]{}; double handoffMs = 0; };
    std::mutex mutex;
    bool enabled = false;
    std::map<uint32_t, Frame> frames;
    QJsonArray rates;
    int encodedFrames = 0, keyframes = 0;
    double began = 0;
    static double Now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    void Begin() { std::lock_guard lock(mutex); frames.clear(); rates = {}; encodedFrames = keyframes = 0; began = Now(); enabled = true; }
    void Rate(uint32_t bitrate, double fps) {
        std::lock_guard lock(mutex);
        if (enabled && rates.size() < 256) rates.append(QJsonObject{{"atMs", Now() - began}, {"bitrate", double(bitrate)}, {"fps", fps}});
    }
    void Output(bool keyframe) { std::lock_guard lock(mutex); if (enabled) { ++encodedFrames; keyframes += keyframe; } }
    QJsonObject Control() { std::lock_guard lock(mutex); return {{"encodedFrames", encodedFrames}, {"keyframes", keyframes}, {"rates", rates}}; }
    void Record(Stage stage, uint32_t rtp, double handoffMs = 0) {
        const double now = Now();
        std::lock_guard lock(mutex);
        if (!enabled || frames.size() >= 4096) return;
        auto& frame = frames[rtp]; frame.at[stage] = now;
        if (stage == EncoderInput) frame.handoffMs = handoffMs;
    }
    QJsonObject End() {
        std::lock_guard lock(mutex); enabled = false;
        std::vector<double> handoff, encoding, decoding, delivery;
        for (const auto& [rtp, frame] : frames) {
            // WebRTC adds a randomized RTP timestamp offset at transport. Pair
            // only within sender or receiver; never assume their RTP epochs match.
            if (frame.at[EncoderInput] && frame.at[Encoded]) {
                handoff.push_back(frame.handoffMs);
                encoding.push_back(frame.at[Encoded] - frame.at[EncoderInput]);
            }
            if (frame.at[DecoderInput] && frame.at[Decoded]) decoding.push_back(frame.at[Decoded] - frame.at[DecoderInput]);
            if (frame.at[Decoded] && frame.at[Consumed]) delivery.push_back(frame.at[Consumed] - frame.at[Decoded]);
        }
        auto stats = [](std::vector<double>& values) {
            std::sort(values.begin(), values.end());
            auto q = [&](double p) -> QJsonValue { return values.empty() ? QJsonValue(QJsonValue::Null) : QJsonValue(values[size_t(std::ceil(p * values.size())) - 1]); };
            return QJsonObject{{"samples", int(values.size())}, {"p50Ms", q(.5)}, {"p95Ms", q(.95)}, {"p99Ms", q(.99)}};
        };
        return {{"captureReturnToEncoderInput", stats(handoff)}, {"encoderInputToOutput", stats(encoding)},
            {"decoderInputToOutput", stats(decoding)},
            {"decodedToCpuConsumption", stats(delivery)}};
    }
};

class TraceEncoder final : public webrtc::VideoEncoder, private webrtc::EncodedImageCallback {
    std::unique_ptr<webrtc::VideoEncoder> inner_;
    std::shared_ptr<PipelineTrace> trace_;
    webrtc::EncodedImageCallback* callback_ = nullptr;
public:
    TraceEncoder(std::unique_ptr<webrtc::VideoEncoder> inner, std::shared_ptr<PipelineTrace> trace) : inner_(std::move(inner)), trace_(std::move(trace)) {}
    ~TraceEncoder() override { inner_->Release(); }
    int InitEncode(const webrtc::VideoCodec* codec, const Settings& settings) override { return inner_->InitEncode(codec, settings); }
    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override { callback_ = callback; return inner_->RegisterEncodeCompleteCallback(this); }
    int32_t Release() override { return inner_->Release(); }
    int32_t Encode(const webrtc::VideoFrame& frame, const std::vector<webrtc::VideoFrameType>* types) override {
        trace_->Record(PipelineTrace::EncoderInput, frame.rtp_timestamp(), (webrtc::TimeMicros() - frame.timestamp_us()) / 1000.0);
        return inner_->Encode(frame, types);
    }
    void SetRates(const RateControlParameters& rates) override { trace_->Rate(rates.bitrate.get_sum_bps(), rates.framerate_fps); inner_->SetRates(rates); }
    EncoderInfo GetEncoderInfo() const override { return inner_->GetEncoderInfo(); }
private:
    Result OnEncodedImage(const webrtc::EncodedImage& image, const webrtc::CodecSpecificInfo* info) override {
        trace_->Record(PipelineTrace::Encoded, image.RtpTimestamp());
        trace_->Output(image._frameType == webrtc::VideoFrameType::kVideoFrameKey);
        return callback_->OnEncodedImage(image, info);
    }
    void OnFrameDropped(uint32_t rtp, int layer, bool last) override { callback_->OnFrameDropped(rtp, layer, last); }
};
class TraceDecoder final : public webrtc::VideoDecoder, private webrtc::DecodedImageCallback {
    std::unique_ptr<webrtc::VideoDecoder> inner_;
    std::shared_ptr<PipelineTrace> trace_;
    webrtc::DecodedImageCallback* callback_ = nullptr;
public:
    TraceDecoder(std::unique_ptr<webrtc::VideoDecoder> inner, std::shared_ptr<PipelineTrace> trace) : inner_(std::move(inner)), trace_(std::move(trace)) {}
    ~TraceDecoder() override { inner_->Release(); }
    bool Configure(const Settings& settings) override { return inner_->Configure(settings); }
    int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override { callback_ = callback; return inner_->RegisterDecodeCompleteCallback(this); }
    int32_t Release() override { return inner_->Release(); }
    int32_t Decode(const webrtc::EncodedImage& image, int64_t render) override {
        trace_->Record(PipelineTrace::DecoderInput, image.RtpTimestamp()); return inner_->Decode(image, render);
    }
    DecoderInfo GetDecoderInfo() const override { return inner_->GetDecoderInfo(); }
private:
    int32_t Decoded(webrtc::VideoFrame& frame) override { trace_->Record(PipelineTrace::Decoded, frame.rtp_timestamp()); return callback_->Decoded(frame); }
    void Decoded(webrtc::VideoFrame& frame, std::optional<int32_t> time, std::optional<uint8_t> qp) override {
        trace_->Record(PipelineTrace::Decoded, frame.rtp_timestamp()); callback_->Decoded(frame, time, qp);
    }
};
class TraceEncoderFactory final : public webrtc::VideoEncoderFactory {
    std::unique_ptr<webrtc::VideoEncoderFactory> inner_; std::shared_ptr<PipelineTrace> trace_;
public:
    TraceEncoderFactory(std::unique_ptr<webrtc::VideoEncoderFactory> inner, std::shared_ptr<PipelineTrace> trace) : inner_(std::move(inner)), trace_(std::move(trace)) {}
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override { return inner_->GetSupportedFormats(); }
    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format, std::optional<std::string> mode, std::optional<webrtc::Resolution> resolution) const override { return inner_->QueryCodecSupport(format, mode, resolution); }
    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override { return std::make_unique<TraceEncoder>(inner_->Create(env, format), trace_); }
};
class TraceDecoderFactory final : public webrtc::VideoDecoderFactory {
    std::unique_ptr<webrtc::VideoDecoderFactory> inner_; std::shared_ptr<PipelineTrace> trace_;
public:
    TraceDecoderFactory(std::unique_ptr<webrtc::VideoDecoderFactory> inner, std::shared_ptr<PipelineTrace> trace) : inner_(std::move(inner)), trace_(std::move(trace)) {}
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override { return inner_->GetSupportedFormats(); }
    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format, bool scaling, std::optional<webrtc::Resolution> resolution) const override { return inner_->QueryCodecSupport(format, scaling, resolution); }
    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override { return std::make_unique<TraceDecoder>(inner_->Create(env, format), trace_); }
};
