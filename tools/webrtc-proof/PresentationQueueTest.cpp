#include "media/webrtc/LatestVideoFrameSink.h"
#include "media/webrtc/OwnedNv12Buffer.h"
#include "api/make_ref_counted.h"
#include <future>
#include <iostream>

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Run() {
    using namespace screenshare::media;
    std::vector<std::byte> pixels(640 * 360 * 3 / 2, std::byte(128));
    std::fill_n(pixels.begin(), 640 * 360, std::byte(90));
    auto buffer = webrtc::make_ref_counted<OwnedNv12Buffer>(640, 360, std::move(pixels));
    auto a = std::async(std::launch::async, [&] { return buffer->ToI420(); });
    auto b = std::async(std::launch::async, [&] { return buffer->ToI420(); });
    auto first = a.get(); auto second = b.get();
    Require(first && first == second && first->DataY()[0] == 90 && first->DataU()[0] == 128,
        "Concurrent NV12 fallback lost pixels or duplicated its cache");
    LatestVideoFrameSink sink;
    for (uint32_t i = 0; i < 500; ++i)
        sink.OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer).set_rtp_timestamp(i).build());
    auto newest = sink.Take();
    Require(newest && newest->rtp_timestamp() == 499 && sink.replaced() == 499 && !sink.Take(), "Presentation retained an obsolete frame/backlog");
    sink.OnFrame(*newest); sink.Stop(); sink.Stop();
    sink.OnFrame(*newest);
    Require(!sink.Take(), "Retired sink accepted late decoded frames");
    std::cout << "500-frame burst reduced to newest; retired sink rejects frames; NV12 ownership/cache passed.\n";
}
}
int main() { try { Run(); return 0; } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
