#include "CaptureTestWindow.h"
#include "media/webrtc/MfHardwareSession.h"
#include "media/webrtc/MfVideoEncoderFactory.h"
#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include <future>
#include <condition_variable>
#include <iostream>
#include <thread>

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
class Sink : public webrtc::EncodedImageCallback {
public:
    Result OnEncodedImage(const webrtc::EncodedImage&, const webrtc::CodecSpecificInfo*) override { ++count; return Result(Result::OK); }
    void OnFrameDropped(uint32_t, int, bool) override {}
    std::atomic<unsigned> count{0};
};
void Run() {
    using namespace screenshare;
    using namespace screenshare::media;
    proof::TestWindow window;
    DesktopCapturer capture;
    CaptureConfig config; config.sourceType = CaptureSourceType::Window;
    config.windowHandle = reinterpret_cast<uint64_t>(window.handle());
    config.targetWidth = 640; config.targetHeight = 360;
    config.includeNv12 = config.ownedNv12 = true;
    config.includeNv12Readback = config.includeBgraReadback = false;
    capture.Start(config);
    auto next = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto frame = capture.TryCaptureFrame(std::chrono::milliseconds(20))) return *frame;
        }
        throw std::runtime_error("Live capture frame deadline exceeded");
    };
    auto first = next();
    window.Invoke([&] { ShowWindow(window.handle(), SW_MINIMIZE); });
    Require(!capture.TryCaptureFrame(std::chrono::milliseconds(20)) && capture.sourceState() == CaptureSourceState::Minimized,
        "Minimized source published a stale frame");
    window.Invoke([&] { ShowWindow(window.handle(), SW_SHOWNOACTIVATE); });
    auto restored = next();
    Require(capture.sourceState() == CaptureSourceState::Active && restored.width == 640 && restored.height == 360,
        "Restored source did not resume capture");
    Require(first.pixels.empty() && first.nv12Pixels.empty(), "Capture performed CPU readback");
    auto device = std::make_shared<D3dVideoDevice>(first.d3dDevice);
    auto retained = device->RetainCapture(first);
    // A separate wrapper captures the expected pixels without populating the
    // retained wrapper's lazy readback cache.
    auto expected = device->RetainCapture(first)->ToI420();
    Require(expected != nullptr, "Initial capture validation readback failed");
    const auto validationReadbacks = device->readbackCount();
    auto borrowed = first; borrowed.nv12OwnedAndComplete = false;
    bool rejected = false;
    try { device->RetainCapture(borrowed); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "Borrowed capture texture accepted");
    auto hardware = std::make_shared<MfHardwareSession>(device);
    MfVideoEncoderFactory factory(hardware);
    auto encoder = factory.Create(webrtc::CreateEnvironment(), factory.GetSupportedFormats().front());
    Sink sink;
    struct Release { webrtc::VideoEncoder& encoder; ~Release() { encoder.Release(); } } release{*encoder};
    webrtc::VideoCodec codec; codec.codecType = webrtc::kVideoCodecH264;
    codec.width = 640; codec.height = 360; codec.maxFramerate = 60; codec.startBitrate = 1000;
    Require(encoder->InitEncode(&codec, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 2, 1200)) == 0,
        "Live capture encoder init failed");
    encoder->RegisterEncodeCompleteCallback(&sink);
    webrtc::VideoBitrateAllocation rates; rates.SetBitrate(0, 0, 1000000);
    encoder->SetRates(webrtc::VideoEncoder::RateControlParameters(rates, 30));
    bool resized = false;
    for (int i = 0; i < 80; ++i) {
        if (i == 25) window.Resize();
        auto frame = next();
        resized |= frame.sourceWidth != first.sourceWidth || frame.sourceHeight != first.sourceHeight;
        auto buffer = device->RetainCapture(frame);
        Require(buffer->width() == 640 && buffer->height() == 360, "Resize changed fixed output shape");
        std::vector<webrtc::VideoFrameType> types{i == 0 ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta};
        Require(encoder->Encode(webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer)
            .set_timestamp_us(int64_t(i + 1) * 33333).set_rtp_timestamp((i + 1) * 3000).build(), &types) == 0, "Live frame encode failed");
    }
    Require(resized && sink.count >= 50 && !hardware->quarantined && hardware->hardwareFrames >= 50, "Live resize/hardware output failed");
    Require(device->readbackCount() == validationReadbacks, "Live encoding used CPU readback");
    // Retire the old device before rebuilding capture. Exercise real WGC/GPU
    // resource recreation without inducing a system-wide driver reset.
    hardware->RetireDevice();
    capture.RebuildWindowDevice();
    auto recovered = next();
    Require(recovered.d3dDevice.Get() != first.d3dDevice.Get(), "Capture reused retired GPU device");
    auto recoveredDevice = std::make_shared<D3dVideoDevice>(recovered.d3dDevice);
    const auto beforeRecovery = sink.count.load();
    for (int i = 0; i < 10; ++i) {
        auto frame = next();
        Require(encoder->Encode(webrtc::VideoFrame::Builder().set_video_frame_buffer(recoveredDevice->RetainCapture(frame))
            .set_rtp_timestamp(300000 + i * 3000).build(), nullptr) == 0, "Recovered capture encode rejected");
    }
    encoder->Release();
    Require(sink.count > beforeRecovery && hardware->softwareFallbacks == 1,
        "Recovered capture did not resume software encoding");
    Require(device->readbackCount() == validationReadbacks, "Recovery read back retired capture textures");
    std::cerr << "Live encode and resize passed; closing source window\n";
    window.Close();
    bool closed = false;
    try { capture.TryCaptureFrame(std::chrono::milliseconds(20)); } catch (const std::runtime_error&) { closed = true; }
    Require(closed, "Closed selected window was not reported");
    Require(capture.sourceState() == CaptureSourceState::Closed, "Closed source status missing");
    proof::TestWindow replacement;
    bool stillClosed = false;
    try { capture.TryCaptureFrame(std::chrono::milliseconds(0)); } catch (const std::runtime_error&) { stillClosed = true; }
    Require(stillClosed && capture.sourceState() == CaptureSourceState::Closed, "Replacement window revived the closed source");
    bool rebuildRejected = false;
    try { capture.RebuildWindowDevice(); } catch (const std::runtime_error&) { rebuildRejected = true; }
    Require(rebuildRejected && capture.sourceState() == CaptureSourceState::Closed, "Recovery revived a closed source");
    std::cerr << "Source closure detected; stopping capture\n";
    capture.Stop(); capture.Stop();
    std::cerr << "Capture stopped; checking retained pixels\n";
    // Read only after many writes, resize and capture destruction: this cannot
    // pass by returning an I420 cache populated before the texture was reused.
    auto oldPixels = retained->ToI420();
    Require(oldPixels && oldPixels->DataY()[oldPixels->StrideY() * 180 + 320] > 30, "Retained capture texture lost after stop");
    for (int y = 0; y < 360; ++y)
        Require(std::equal(expected->DataY() + y * expected->StrideY(), expected->DataY() + y * expected->StrideY() + 640,
            oldPixels->DataY() + y * oldPixels->StrideY()), "Retained capture pixels changed after reuse/resize/stop");
    std::cout << "Live selected-window WGC: " << sink.count << " outputs; fixed-size resize, hardware delivery, device rebuild/software recovery, closure and retained texture passed.\n";
}
}
int main(int argc, char** argv) {
    try {
        const int cycles = argc == 2 && std::string(argv[1]) == "--repeat" ? 3 : 1;
        for (int cycle = 0; cycle < cycles; ++cycle) Run();
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
