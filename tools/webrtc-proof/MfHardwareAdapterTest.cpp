#include "media/webrtc/MfHardwareSession.h"
#include "media/webrtc/MfVideoEncoderFactory.h"
#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include <condition_variable>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Events : screenshare::media::MfHardwareSession {
    using MfHardwareSession::MfHardwareSession;
    std::atomic<bool> omitOutput{false};
    std::atomic<unsigned> polls{0};
    std::vector<screenshare::EncodedPacket> PollOutput(screenshare::H264StreamEncoder& encoder) override {
        ++polls;
        auto packets = encoder.PollHardwareOutput();
        if (omitOutput) packets.clear();
        return packets;
    }
};
class Sink : public webrtc::EncodedImageCallback {
public:
    Result OnEncodedImage(const webrtc::EncodedImage& image, const webrtc::CodecSpecificInfo*) override {
        std::lock_guard lock(mutex);
        last = image; ++count; changed.notify_all();
        return Result(Result::OK);
    }
    void OnFrameDropped(uint32_t, int, bool) override {
        std::lock_guard lock(mutex); ++dropped; changed.notify_all();
    }
    void WaitDropped(unsigned expected) {
        std::unique_lock lock(mutex);
        Require(changed.wait_for(lock, std::chrono::seconds(3), [&] { return dropped >= expected; }), "Retired frame drop timeout");
    }
    void Wait(unsigned expected) {
        std::unique_lock lock(mutex);
        Require(changed.wait_for(lock, std::chrono::seconds(3), [&] { return count >= expected; }), "Hardware/fallback callback timeout");
    }
    std::mutex mutex;
    std::condition_variable changed;
    webrtc::EncodedImage last;
    unsigned count = 0;
    unsigned dropped = 0;
};
void Run() {
    using namespace screenshare::media;
    auto device = std::make_shared<D3dVideoDevice>();
    std::vector<uint8_t> pixels(640 * 360 * 3 / 2, 128);
    std::fill_n(pixels.begin(), 640 * 360, uint8_t(80));
    auto original = device->UploadNv12(640, 360, pixels);
    // A later upload must never mutate a frame retained by another viewer.
    std::fill_n(pixels.begin(), 640 * 360, uint8_t(170));
    // The production GPU scaler's output must remain native all the way into MF.
    auto larger = device->UploadNv12(1280, 720, std::vector<uint8_t>(1280 * 720 * 3 / 2, 128));
    auto newer = larger->Scale(640, 360, 0, 0, 640, 360);
    Require(newer && device->readbackCount() == 0, "GPU scale before encoding used readback");
    auto a = std::async(std::launch::async, [&] { return original->ToI420(); });
    auto b = std::async(std::launch::async, [&] { return original->ToI420(); });
    auto first = a.get(); auto second = b.get();
    Require(first && second && first.get() == second.get() && first->DataY()[0] == 80,
        "GPU frame was overwritten or concurrent readback was not cached");
    Require(device->readbackCount() == 1, "GPU fallback performed duplicate readbacks");
    auto events = std::make_shared<Events>(device);
    MfVideoEncoderFactory factory(events);
    auto encoder = factory.Create(webrtc::CreateEnvironment(), factory.GetSupportedFormats().front());
    Sink sink;
    struct Cleanup { webrtc::VideoEncoder& value; ~Cleanup() { value.Release(); } } cleanup{*encoder};
    webrtc::VideoCodec codec;
    codec.codecType = webrtc::kVideoCodecH264; codec.width = 640; codec.height = 360;
    codec.maxFramerate = 60; codec.startBitrate = 1000;
    const webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 2, 1200);
    Require(encoder->InitEncode(&codec, settings) == 0 && encoder->GetEncoderInfo().is_hardware_accelerated,
        "Hardware initialization/probe failed");
    encoder->RegisterEncodeCompleteCallback(&sink);
    auto frame = webrtc::VideoFrame::Builder().set_video_frame_buffer(newer).set_rtp_timestamp(77).build();
    Require(encoder->Encode(frame, nullptr) == 0, "Hardware input rejected");
    sink.Wait(1);
    Require(events->hardwareFrames == 1 && device->readbackCount() == 1, "Hardware used CPU fallback");
    webrtc::VideoBitrateAllocation zero;
    encoder->SetRates({zero, 0});
    Require(encoder->Encode(frame, nullptr) == 0, "Suspended hardware input failed");
    webrtc::VideoBitrateAllocation reduced;
    reduced.SetBitrate(0, 0, 400'000);
    encoder->SetRates({reduced, 60});
    Require(sink.count == 1 && events->hardwareFrames == 1, "Zero rate still submitted hardware video");
    // Drain real events but withhold output from the adapter. It must time out,
    // quarantine and re-encode this retained GPU frame in software with an IDR.
    events->omitOutput = true;
    frame.set_rtp_timestamp(88);
    const auto before = std::chrono::steady_clock::now();
    Require(encoder->Encode(frame, nullptr) == 0, "Failure test input rejected");
    sink.Wait(2);
    Require(std::chrono::steady_clock::now() - before >= std::chrono::milliseconds(500), "Missing-output deadline was bypassed");
    Require(events->quarantined && events->softwareFallbacks == 1 && !encoder->GetEncoderInfo().is_hardware_accelerated,
        "Hardware failure did not enter quarantined software fallback");
    Require(sink.last.RtpTimestamp() == 88 && sink.last._frameType == webrtc::VideoFrameType::kVideoFrameKey,
        "Fallback lost timestamp or IDR recovery");
    Require(device->readbackCount() == 2, "GPU software fallback did not read back exactly once");
    encoder->Release();
    const auto polls = events->polls.load();
    Require(encoder->InitEncode(&codec, settings) == 0 && !encoder->GetEncoderInfo().is_hardware_accelerated,
        "Reset retried quarantined hardware");
    encoder->RegisterEncodeCompleteCallback(&sink);
    frame.set_rtp_timestamp(99);
    Require(encoder->Encode(frame, nullptr) == 0, "Software reset input failed");
    sink.Wait(3);
    encoder->Release();
    Require(events->polls == polls, "Quarantined hardware received more polls");
    // Cancellation interrupts the polling loop without marking a healthy GPU bad.
    auto cancellableEvents = std::make_shared<Events>(device);
    MfVideoEncoderFactory cancellableFactory(cancellableEvents);
    auto cancellable = cancellableFactory.Create(webrtc::CreateEnvironment(), cancellableFactory.GetSupportedFormats().front());
    Require(cancellable->InitEncode(&codec, settings) == 0 && cancellable->GetEncoderInfo().is_hardware_accelerated,
        "Cancellation hardware probe failed");
    cancellable->RegisterEncodeCompleteCallback(&sink);
    cancellableEvents->omitOutput = true;
    const auto initialPolls = cancellableEvents->polls.load();
    Require(cancellable->Encode(frame, nullptr) == 0, "Cancellation frame rejected");
    const auto waitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (cancellableEvents->polls <= initialPolls + 2 && std::chrono::steady_clock::now() < waitDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(cancellableEvents->polls > initialPolls + 2, "Cancellation never reached output wait");
    const auto stopStart = std::chrono::steady_clock::now();
    cancellable->Release();
    Require(std::chrono::steady_clock::now() - stopStart < std::chrono::milliseconds(300) && !cancellableEvents->quarantined,
        "Cancellation waited for deadline or quarantined hardware");
    Require(sink.count == 3, "Retired hardware frame delivered a callback");
    // Device retirement differs from a transform stall: old textures cannot be
    // read back for fallback. Both viewers must accept a fresh device afterwards.
    auto retired = std::make_shared<MfHardwareSession>(device);
    MfVideoEncoderFactory recoveryFactory(retired);
    auto replacement = std::make_shared<D3dVideoDevice>();
    auto fresh = replacement->UploadNv12(640, 360, pixels);
    std::vector<std::unique_ptr<webrtc::VideoEncoder>> viewers;
    for (int i = 0; i < 2; ++i) {
        auto viewer = recoveryFactory.Create(webrtc::CreateEnvironment(), recoveryFactory.GetSupportedFormats().front());
        Require(viewer->InitEncode(&codec, settings) == 0, "Recovery viewer initialization failed");
        viewer->RegisterEncodeCompleteCallback(&sink);
        viewers.push_back(std::move(viewer));
    }
    retired->RetireDevice();
    const auto readbacksBeforeRetirement = device->readbackCount();
    unsigned expectedDrops = sink.dropped;
    unsigned expectedOutputs = sink.count;
    for (auto& viewer : viewers) {
        frame.set_rtp_timestamp(101);
        Require(viewer->Encode(frame, nullptr) == 0, "Retired input failed synchronously");
        sink.WaitDropped(++expectedDrops);
        auto replacementFrame = webrtc::VideoFrame::Builder().set_video_frame_buffer(fresh).set_rtp_timestamp(102).build();
        Require(viewer->Encode(replacementFrame, nullptr) == 0, "Fresh device input rejected");
        sink.Wait(++expectedOutputs);
        Require(sink.last.RtpTimestamp() == 102 && sink.last._frameType == webrtc::VideoFrameType::kVideoFrameKey,
            "Recovery did not preserve fresh timestamp/IDR");
        Require(!viewer->GetEncoderInfo().is_hardware_accelerated, "Retired hardware was reused");
    }
    Require(device->readbackCount() == readbacksBeforeRetirement, "Read back a retired device texture");
    Require(replacement->readbackCount() == 1, "Fresh frame readback was not shared between viewers");
    replacement->Retire();
    auto thirdDevice = std::make_shared<D3dVideoDevice>();
    auto thirdFrame = thirdDevice->UploadNv12(640, 360, pixels);
    for (auto& viewer : viewers) {
        auto stale = webrtc::VideoFrame::Builder().set_video_frame_buffer(fresh).set_rtp_timestamp(103).build();
        Require(viewer->Encode(stale, nullptr) == 0, "Later retired generation input failed");
        sink.WaitDropped(++expectedDrops);
        auto next = webrtc::VideoFrame::Builder().set_video_frame_buffer(thirdFrame).set_rtp_timestamp(104).build();
        Require(viewer->Encode(next, nullptr) == 0, "Third generation rejected");
        sink.Wait(++expectedOutputs);
        Require(sink.last.RtpTimestamp() == 104 && sink.last._frameType == webrtc::VideoFrameType::kVideoFrameKey,
            "Later generation did not resume with IDR");
        viewer->Release();
    }
    Require(!fresh->ToI420() && replacement->readbackCount() == 1, "Retired generation exposed cached/readback pixels");
    std::cout << "Hardware fallback, quarantine, retained textures and cached CPU conversion passed; readback_us="
              << device->readbackMicroseconds() << '\n';
    // Isolate the frame lifetime proof from factory/session ownership.
    auto heldDevice = std::make_shared<D3dVideoDevice>();
    std::weak_ptr<D3dVideoDevice> heldLifetime = heldDevice;
    auto held = heldDevice->UploadNv12(640, 360, pixels);
    heldDevice.reset();
    Require(!heldLifetime.expired() && held->ToI420(), "Frame did not retain its GPU owner");
    held = nullptr;
    Require(heldLifetime.expired(), "Released frame retained its GPU owner thread");
}
}
int main() {
    try { Run(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
