#include "media/webrtc/MfVideoDecoderFactory.h"
#include "codec/H264StreamEncoder.h"
#include "api/environment/environment_factory.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "media/webrtc/D3dVideoFrameBuffer.h"
#include "../../frontend/shared/LatestRoomVideoFrame.h"

#include <iostream>
#include <stdexcept>
#include <thread>
#include <deque>
#include <map>

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
class Sink final : public webrtc::DecodedImageCallback {
public:
    int32_t Decoded(webrtc::VideoFrame& frame) override {
        Require(std::this_thread::get_id() == caller, "Callback escaped decode caller thread");
        const auto dimensions = expectedSizes.find(frame.rtp_timestamp());
        const auto expectedWidth = dimensions == expectedSizes.end() ? width : dimensions->second.first;
        const auto expectedHeight = dimensions == expectedSizes.end() ? height : dimensions->second.second;
        if (frame.width() != expectedWidth || frame.height() != expectedHeight)
            throw std::runtime_error("Visible aperture mismatch: expected " + std::to_string(expectedWidth) + "x" + std::to_string(expectedHeight) +
                ", received " + std::to_string(frame.width()) + "x" + std::to_string(frame.height()));
        if (dimensions != expectedSizes.end()) expectedSizes.erase(dimensions);
        Require(!expected.empty() && frame.rtp_timestamp() == expected.front(), "RTP wrap/timestamp association lost");
        expected.pop_front();
        Require(frame.ntp_time_ms() == 123456, "NTP metadata lost");
        if (gpu) {
            Require(frame.video_frame_buffer()->type() == webrtc::VideoFrameBuffer::Type::kNative, "Hardware decode fell back unexpectedly");
            LatestRoomVideoFrame latest;
            const auto before = gpu->readbackCount();
            latest.OnFrame(frame); latest.OnFrame(frame);
            retained = latest.Take(); latest.Stop();
            Require(retained && retained->native && retained->nv12.empty(), "GPU handoff lost native texture");
            Require(gpu->readbackCount() == before && latest.statistics().converted == 0 && latest.statistics().replaced == 1,
                "GPU handoff performed a CPU conversion or queued stale frames");
            if (!firstRetained) firstRetained = retained;
        }
        else Require(frame.video_frame_buffer()->ToI420()->DataY()[0] >= 65, "Decoded luma incorrect");
        ++count;
        return 0;
    }
    int width = 0, height = 0;
    std::deque<uint32_t> expected;
    std::map<uint32_t, std::pair<int, int>> expectedSizes;
    unsigned count = 0;
    std::shared_ptr<screenshare::media::D3dVideoDevice> gpu;
    std::optional<screenshare::Nv12VideoFrame> retained;
    std::optional<screenshare::Nv12VideoFrame> firstRetained;
    std::thread::id caller = std::this_thread::get_id();
};

void Run(bool gpu, bool unavailable = false) {
    std::shared_ptr<screenshare::media::D3dVideoDevice> device;
    screenshare::media::MfVideoDecoderFactory factory(gpu, [&] {
        if (unavailable) throw std::runtime_error("Injected unavailable decoder device");
        return device = std::make_shared<screenshare::media::D3dVideoDevice>();
    });
    auto environment = webrtc::CreateEnvironment();
    Require(!factory.Create(environment, webrtc::SdpVideoFormat("VP9")), "Factory accepted unsupported codec");
    const auto format = factory.GetSupportedFormats().front();
    Require(!factory.QueryCodecSupport(format, true, std::nullopt).is_supported, "Factory advertised spatial scaling");
    auto decoder = factory.Create(environment, format);
    Require(decoder && !decoder->GetDecoderInfo().is_hardware_accelerated, "CPU decoder advertised hardware");
    Sink sink;
    for (int cycle = 0; cycle < 4; ++cycle) {
        sink.width = cycle % 2 ? 1920 : 640;
        sink.height = cycle % 2 ? 1080 : 360;
        webrtc::VideoDecoder::Settings settings;
        settings.set_codec_type(webrtc::kVideoCodecH264);
        settings.set_max_render_resolution({320, 180}); // Receiver's first-frame hint, not a permanent source-size cap.
        Require(decoder->Configure(settings), "Configure failed");
        sink.gpu = device;
        Require(decoder->GetDecoderInfo().is_hardware_accelerated == (gpu && !unavailable), "Wrong decoder implementation reported");
        decoder->RegisterDecodeCompleteCallback(&sink);
        webrtc::EncodedImage empty;
        Require(decoder->Decode(empty, 0) == WEBRTC_VIDEO_CODEC_ERR_PARAMETER, "Empty frame accepted");
        screenshare::H264StreamEncoder encoder;
        screenshare::H264StreamEncoderConfig config;
        config.width = sink.width;
        config.height = sink.height;
        encoder.Start(config);
        screenshare::CapturedFrame frame;
        frame.width = frame.sourceWidth = sink.width;
        frame.height = frame.sourceHeight = sink.height;
        frame.nv12Pixels.assign(size_t(sink.width) * sink.height * 3 / 2, std::byte{128});
        std::fill_n(frame.nv12Pixels.begin(), size_t(sink.width) * sink.height, std::byte{80});
        const auto before = sink.count;
        for (unsigned i = 0; i < 12; ++i) {
            std::fill_n(frame.nv12Pixels.begin(), size_t(sink.width) * sink.height, std::byte(80 + i * 3));
            for (const auto& packet : encoder.EncodeFrame(frame)) {
                webrtc::EncodedImage image;
                image.SetEncodedData(webrtc::EncodedImageBuffer::Create(
                    reinterpret_cast<const uint8_t*>(packet.bytes.data()), packet.bytes.size()));
                image.SetRtpTimestamp(0xfffff000u + i * 3000u); // Deliberate 32-bit wrap.
                image.ntp_time_ms_ = 123456;
                image._frameType = packet.isKeyframe ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
                image._encodedWidth = sink.width; image._encodedHeight = sink.height;
                if (i == 0) {
                    auto delta = image;
                    delta._frameType = webrtc::VideoFrameType::kVideoFrameDelta;
                    Require(decoder->Decode(delta, 0) == WEBRTC_VIDEO_CODEC_ERROR, "Decoder accepted delta before keyframe");
                    auto oversized = image; oversized._encodedWidth = 4097;
                    Require(decoder->Decode(oversized, 0) == WEBRTC_VIDEO_CODEC_ERR_PARAMETER, "Oversized keyframe declaration accepted");
                }
                sink.expected.push_back(image.RtpTimestamp());
                Require(decoder->Decode(image, 0) == WEBRTC_VIDEO_CODEC_OK, "MF adapter decode failed");
            }
        }
        Require(sink.count - before >= 10, "MF adapter retained excessive output");
        decoder->Release();
        if (sink.retained) {
            Require(device->readbackCount() == 0, "Normal GPU path read back decoded frames");
            Require(sink.retained->pixels().size() == size_t(sink.width) * sink.height * 3 / 2 &&
                sink.retained->pixels()[0] >= 100, "Retained GPU frame did not survive decoder release");
            Require(sink.firstRetained->pixels()[0] >= 65 && sink.firstRetained->pixels()[0] <= 90,
                "Decoder recycled a published texture while it was still retained");
            const auto pixels = sink.retained->pixels();
            const auto luma = size_t(sink.width) * sink.height;
            Require(pixels[luma - 1] >= 100 && pixels[luma] >= 120 && pixels[luma] <= 136 &&
                pixels.back() >= 120 && pixels.back() <= 136, "GPU aperture lost bottom-row luma or chroma");
            Require(device->readbackCount() == 2, "Explicit readback was not cached");
            sink.retained.reset(); sink.firstRetained.reset();
        }
        sink.expected.clear();
        decoder->Release();
        Require(decoder->Decode(empty, 0) == WEBRTC_VIDEO_CODEC_UNINITIALIZED, "Decode remained active after Release");
    }
    std::cout << "MF adapter " << (gpu ? (unavailable ? "GPU startup fallback" : "GPU") : "CPU") << ": " << sink.count << " decoded frames, four reset/release cycles, 1080 crop, RTP wrap and callback ownership passed.\n";
}

void Recovery() {
    std::shared_ptr<screenshare::media::D3dVideoDevice> device;
    unsigned devices = 0;
    screenshare::media::MfVideoDecoderFactory factory(true, [&] {
        ++devices; return device = std::make_shared<screenshare::media::D3dVideoDevice>();
    });
    auto decoder = factory.Create(webrtc::CreateEnvironment(), factory.GetSupportedFormats().front());
    webrtc::VideoDecoder::Settings settings; settings.set_codec_type(webrtc::kVideoCodecH264);
    settings.set_max_render_resolution({640, 360});
    Require(decoder->Configure(settings) && decoder->GetDecoderInfo().is_hardware_accelerated, "Recovery GPU configure failed");
    Sink sink; sink.width = 640; sink.height = 360; decoder->RegisterDecodeCompleteCallback(&sink);
    screenshare::H264StreamEncoder encoder; screenshare::H264StreamEncoderConfig config;
    config.width = sink.width; config.height = sink.height; encoder.Start(config);
    screenshare::CapturedFrame frame; frame.width = frame.sourceWidth = sink.width; frame.height = frame.sourceHeight = sink.height;
    frame.nv12Pixels.assign(size_t(sink.width) * sink.height * 3 / 2, std::byte{128});
    std::fill_n(frame.nv12Pixels.begin(), size_t(sink.width) * sink.height, std::byte{80});
    std::vector<webrtc::EncodedImage> images;
    for (unsigned i = 0; i < 16; ++i) for (auto& packet : encoder.EncodeFrame(frame)) {
        webrtc::EncodedImage image;
        image.SetEncodedData(webrtc::EncodedImageBuffer::Create(reinterpret_cast<const uint8_t*>(packet.bytes.data()), packet.bytes.size()));
        image.SetRtpTimestamp(i * 3000); image.ntp_time_ms_ = 123456;
        image._frameType = packet.isKeyframe ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
        image._encodedWidth = sink.width; image._encodedHeight = sink.height;
        images.push_back(image);
    }
    Require(images.size() >= 12 && images.front()._frameType == webrtc::VideoFrameType::kVideoFrameKey, "Recovery encoder output missing");
    // Deliberately hold every callback: decoding still progresses without
    // accumulating more than eight published textures or requesting new keys.
    class HoldingSink : public webrtc::DecodedImageCallback {
    public:
        std::vector<webrtc::scoped_refptr<webrtc::VideoFrameBuffer>> frames;
        int32_t Decoded(webrtc::VideoFrame& value) override { frames.push_back(value.video_frame_buffer()); return 0; }
    } holding;
    decoder->RegisterDecodeCompleteCallback(&holding);
    for (const auto& image : images) Require(decoder->Decode(image, 0) == WEBRTC_VIDEO_CODEC_OK, "Pressure blocked decoding");
    Require(holding.frames.size() == 8 && device->readbackCount() == 0, "GPU output retention is not bounded");
    holding.frames.clear();
    decoder->RegisterDecodeCompleteCallback(&sink);
    device->Retire();
    Require(decoder->Decode(images.front(), 0) == WEBRTC_VIDEO_CODEC_ERROR, "Retired GPU was accepted");
    Require(!decoder->GetDecoderInfo().is_hardware_accelerated, "Retired decoder still reported hardware");
    Require(decoder->Decode(images.front(), 0) == WEBRTC_VIDEO_CODEC_ERROR, "Decoder ignored recovery backoff");
    Require(decoder->Decode(images.back(), 0) == WEBRTC_VIDEO_CODEC_ERROR, "Recovery accepted delta without keyframe");
    std::this_thread::sleep_for(std::chrono::milliseconds(270));
    for (const auto& image : images) {
        sink.expected.push_back(image.RtpTimestamp());
        Require(decoder->Decode(image, 0) == WEBRTC_VIDEO_CODEC_OK, "Software recovery failed");
    }
    Require(sink.count >= 10 && devices == 1, "Hardware retried or fixed-size fallback failed");
    // Malformed keyframes may be buffered by MF instead of failing immediately.
    // The association bound must still trigger recovery and ultimately stop it.
    const uint8_t malformed[] = {0, 0, 0, 1, 0};
    auto corrupt = images.front(); corrupt.SetEncodedData(webrtc::EncodedImageBuffer::Create(malformed, sizeof(malformed)));
    int result = WEBRTC_VIDEO_CODEC_OK;
    for (unsigned attempt = 0; attempt < 160 && result != WEBRTC_VIDEO_CODEC_UNINITIALIZED; ++attempt) {
        result = decoder->Decode(corrupt, 0);
        if (result == WEBRTC_VIDEO_CODEC_ERROR) std::this_thread::sleep_for(std::chrono::milliseconds(270));
    }
    Require(result == WEBRTC_VIDEO_CODEC_UNINITIALIZED && devices == 1, "Corrupt input rebuilt the decoder indefinitely");
    decoder->Release();
    Require(decoder->Configure(settings) && !decoder->GetDecoderInfo().is_hardware_accelerated && devices == 1,
        "Reconfiguration revived quarantined hardware");
    decoder->Release();
    auto replacement = factory.Create(webrtc::CreateEnvironment(), factory.GetSupportedFormats().front());
    Require(replacement->Configure(settings) && !replacement->GetDecoderInfo().is_hardware_accelerated && devices == 1,
        "Decoder replacement revived quarantined hardware");
    replacement->Release();
    std::cout << "GPU pressure, device retirement, fixed-size software recovery, keyframe/backoff and exhausted recovery passed.\n";
}
}
void ResizeWithoutReconfigure(bool gpu = false) {
    std::shared_ptr<screenshare::media::D3dVideoDevice> device;
    screenshare::media::MfVideoDecoderFactory factory(gpu, [&] { return device = std::make_shared<screenshare::media::D3dVideoDevice>(); });
    auto decoder = factory.Create(webrtc::CreateEnvironment(), factory.GetSupportedFormats().front());
    webrtc::VideoDecoder::Settings settings; settings.set_codec_type(webrtc::kVideoCodecH264);
    settings.set_max_render_resolution({320, 180});
    Require(decoder->Configure(settings), "Resize decoder configure failed");
    Sink sink; sink.gpu = device; decoder->RegisterDecodeCompleteCallback(&sink);
    Require(!gpu || decoder->GetDecoderInfo().is_hardware_accelerated, "Resize hardware decoder fell back");
    struct Cleanup { webrtc::VideoDecoder& decoder; ~Cleanup() { decoder.Release(); } } cleanup{*decoder};
    unsigned timestamp = 0;
    for (int width : {320, 640, 320, 1280}) {
        // A decoder may emit its pending old-size frame after the next input.
        // Validate dimensions against each output's RTP timestamp, not the
        // most recently submitted frame.
        Require(sink.expected.size() <= 1, "Resize accumulated old decoder output");
        sink.width = width; sink.height = width * 9 / 16;
        screenshare::H264StreamEncoder encoder;
        screenshare::H264StreamEncoderConfig config; config.width = sink.width; config.height = sink.height;
        encoder.Start(config);
        screenshare::CapturedFrame frame; frame.width = frame.sourceWidth = sink.width; frame.height = frame.sourceHeight = sink.height;
        frame.nv12Pixels.assign(size_t(sink.width) * sink.height * 3 / 2, std::byte{128});
        const auto before = sink.count;
        for (unsigned i = 0; i < 6; ++i) for (const auto& packet : encoder.EncodeFrame(frame)) {
            webrtc::EncodedImage image;
            image.SetEncodedData(webrtc::EncodedImageBuffer::Create(reinterpret_cast<const uint8_t*>(packet.bytes.data()), packet.bytes.size()));
            image.SetRtpTimestamp(++timestamp * 3000); image.ntp_time_ms_ = 123456;
            image._frameType = packet.isKeyframe ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
            image._encodedWidth = sink.width; image._encodedHeight = sink.height;
            sink.expected.push_back(image.RtpTimestamp());
            sink.expectedSizes.emplace(image.RtpTimestamp(), std::pair{sink.width, sink.height});
            Require(decoder->Decode(image, 0) == WEBRTC_VIDEO_CODEC_OK, "Dynamic resize decode failed");
        }
        Require(sink.count >= before + 5, "Dynamic resize stopped output");
    }
    if (gpu) {
        Require(device->readbackCount() == 0 && sink.firstRetained && sink.firstRetained->width == 320 &&
            sink.retained && sink.retained->width == 1280, "Resize lost retained GPU frame ownership");
        Require(sink.firstRetained->pixels().size() == 320 * 180 * 3 / 2, "Old GPU frame did not survive resize");
    }
}
int main(int argc, char** argv) {
    try { ResizeWithoutReconfigure(); Run(false); Run(true, true); if (argc > 1 && std::string(argv[1]) == "--gpu") { ResizeWithoutReconfigure(true); Run(true); Recovery(); } return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
