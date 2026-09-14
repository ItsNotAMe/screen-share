#include "media/webrtc/MfVideoDecoderFactory.h"
#include "codec/H264StreamEncoder.h"
#include "api/environment/environment_factory.h"
#include "modules/video_coding/include/video_error_codes.h"

#include <iostream>
#include <stdexcept>
#include <thread>
#include <deque>

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
class Sink final : public webrtc::DecodedImageCallback {
public:
    int32_t Decoded(webrtc::VideoFrame& frame) override {
        Require(std::this_thread::get_id() == caller, "Callback escaped decode caller thread");
        Require(frame.width() == width && frame.height() == height, "Visible aperture mismatch");
        Require(!expected.empty() && frame.rtp_timestamp() == expected.front(), "RTP wrap/timestamp association lost");
        expected.pop_front();
        Require(frame.ntp_time_ms() == 123456, "NTP metadata lost");
        Require(frame.video_frame_buffer()->ToI420()->DataY()[0] >= 65, "Decoded luma incorrect");
        ++count;
        return 0;
    }
    int width = 0, height = 0;
    std::deque<uint32_t> expected;
    unsigned count = 0;
    std::thread::id caller = std::this_thread::get_id();
};

void Run() {
    screenshare::media::MfVideoDecoderFactory factory;
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
        settings.set_max_render_resolution({sink.width, sink.height});
        Require(decoder->Configure(settings), "Configure failed");
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
            for (const auto& packet : encoder.EncodeFrame(frame)) {
                webrtc::EncodedImage image;
                image.SetEncodedData(webrtc::EncodedImageBuffer::Create(
                    reinterpret_cast<const uint8_t*>(packet.bytes.data()), packet.bytes.size()));
                image.SetRtpTimestamp(0xfffff000u + i * 3000u); // Deliberate 32-bit wrap.
                image.ntp_time_ms_ = 123456;
                image._frameType = packet.isKeyframe ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
                if (i == 0) {
                    auto delta = image;
                    delta._frameType = webrtc::VideoFrameType::kVideoFrameDelta;
                    Require(decoder->Decode(delta, 0) == WEBRTC_VIDEO_CODEC_ERROR, "Decoder accepted delta before keyframe");
                }
                sink.expected.push_back(image.RtpTimestamp());
                Require(decoder->Decode(image, 0) == WEBRTC_VIDEO_CODEC_OK, "MF adapter decode failed");
            }
        }
        Require(sink.count - before >= 10, "MF adapter retained excessive output");
        decoder->Release();
        sink.expected.clear();
        decoder->Release();
        Require(decoder->Decode(empty, 0) == WEBRTC_VIDEO_CODEC_UNINITIALIZED, "Decode remained active after Release");
    }
    std::cout << "MF adapter: " << sink.count << " decoded frames, four reset/release cycles, 1080 crop, RTP wrap and callback ownership passed.\n";
}
}
int main() {
    try { Run(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
