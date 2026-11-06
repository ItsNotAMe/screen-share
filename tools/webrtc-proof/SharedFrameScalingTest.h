#pragma once
#include "media/webrtc/CaptureVideoSource.h"
#include "api/make_ref_counted.h"
#include <array>
#include <future>
#include <iostream>
#include <set>
#include <stdexcept>

// Generated textures only: no desktop capture, input or audio devices.
inline void SharedFrameScalingTest() {
    using namespace screenshare::media;
    auto check = [](bool ok) { if (!ok) throw std::runtime_error("Shared-frame scaling regression"); };
    auto device = std::make_shared<D3dVideoDevice>();
    struct Sink : webrtc::VideoSinkInterface<webrtc::VideoFrame> {
        webrtc::scoped_refptr<webrtc::VideoFrameBuffer> last;
        unsigned frames = 0;
        void OnFrame(const webrtc::VideoFrame& frame) override { last = frame.video_frame_buffer(); ++frames; }
    };
    struct Measurement { uint64_t scales, readbacks, textures; };
    auto measure = [&](bool share) {
        std::array<Sink, 4> sinks;
        std::array<webrtc::scoped_refptr<CaptureVideoSource>, 4> sources;
        StreamPreferences preferences; preferences.resolution = ResolutionMode::Fixed;
        preferences.width = 1280; preferences.height = 720;
        for (size_t i = 0; i < sources.size(); ++i) {
            sources[i] = webrtc::make_ref_counted<CaptureVideoSource>();
            preferences.preset = i % 2 ? StreamPreset::Gaming : StreamPreset::Quality;
            sources[i]->Configure(preferences, 1);
            sources[i]->AddOrUpdateSink(&sinks[i], webrtc::VideoSinkWants());
        }
        const auto startScales = device->scalingCount(), startReads = device->readbackCount();
        const auto beginning = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        webrtc::scoped_refptr<webrtc::I420BufferInterface> held;
        uint64_t textureCount = 0;
        std::vector<uint8_t> nv12(1920 * 1080 * 3 / 2, 128);
        for (int frame = 0; frame < 24; ++frame) {
            std::fill_n(nv12.begin(), 1920 * 1080, uint8_t(40 + frame));
            auto input = device->UploadNv12(1920, 1080, nv12);
            const auto retained = input->RetainedNv12();
            std::array<std::future<void>, 4> workers;
            for (size_t i = 0; i < workers.size(); ++i) workers[i] = std::async(std::launch::async, [&, i] {
                // Separate wrappers reproduce the former per-viewer scaling;
                // production distributes one immutable input to all viewers.
                auto buffer = share ? input : webrtc::make_ref_counted<D3dVideoFrameBuffer>(
                    device, retained.nv12Texture, 1920, 1080);
                sources[i]->PushBuffer(buffer, beginning + std::chrono::milliseconds(frame * 40), 17);
            });
            for (auto& worker : workers) worker.get();
            std::set<ID3D11Texture2D*> textures;
            for (size_t i = 0; i < sinks.size(); ++i) {
                check(sinks[i].frames == unsigned(frame + 1) && sinks[i].last);
                auto* metadata = dynamic_cast<MappedVideoBuffer*>(sinks[i].last.get());
                check(metadata && metadata->mapping.generation == 17 && metadata->preset ==
                    (i % 2 ? StreamPreset::Gaming : StreamPreset::Quality));
                auto* native = dynamic_cast<D3dVideoFrameBuffer*>(Unwrap(sinks[i].last).get());
                check(native != nullptr); textures.insert(native->RetainedNv12().nv12Texture.Get());
                auto pixels = sinks[i].last->ToI420();
                check(pixels && pixels->width() == 1280 && pixels->height() == 720 && pixels->DataY()[0] == 40 + frame);
                if (!held) held = pixels;
            }
            textureCount += textures.size();
            check(held->DataY()[0] == 40); // New source pixels never overwrite old output.
        }
        for (size_t i = 0; i < sources.size(); ++i) sources[i]->RemoveSink(&sinks[i]);
        return Measurement{device->scalingCount() - startScales, device->readbackCount() - startReads, textureCount};
    };
    const auto separate = measure(false), shared = measure(true);
    std::cout << "Four viewers, 24 generated 1080p frames -> 720p: separate scales/readbacks="
        << separate.scales << '/' << separate.readbacks << " shared=" << shared.scales << '/' << shared.readbacks << '\n';
    check(separate.scales == 96 && separate.readbacks == 96 && separate.textures == 96 &&
        shared.scales == 24 && shared.readbacks == 24 && shared.textures == 24);

    std::vector<uint8_t> pixels(640 * 360 * 3 / 2, 128);
    std::fill_n(pixels.begin(), 640 * 360, uint8_t(80));
    auto input = device->UploadNv12(640, 360, pixels);
    auto top = input->Scale(320, 240, 0, 0, 320, 180);
    check(top && top->ToI420()->DataY()[0] == 80);
    auto centered = input->Scale(320, 240, 0, 30, 320, 180);
    check(centered && centered->ToI420()->DataY()[0] == 16 && centered.get() != top.get());
    auto narrower = input->Scale(320, 240, 0, 30, 160, 180);
    check(narrower && narrower->ToI420()->DataY()[200] == 16 && narrower.get() != centered.get());
    auto shifted = input->Scale(320, 240, 80, 30, 160, 180);
    check(shifted && shifted->ToI420()->DataY()[60 * shifted->ToI420()->StrideY()] == 16 && shifted.get() != narrower.get());
    auto shorter = input->Scale(320, 240, 80, 30, 160, 90);
    check(shorter && shorter->ToI420()->DataY()[150 * shorter->ToI420()->StrideY() + 120] == 16 && shorter.get() != shifted.get());
    const auto beforeEvicted = device->scalingCount();
    auto topAgain = input->Scale(320, 240, 0, 0, 320, 180);
    check(topAgain && device->scalingCount() == beforeEvicted + 1 && topAgain.get() != top.get());
    check(top->ToI420()->DataY()[0] == 80 && centered->ToI420()->DataY()[0] == 16);
    check(input->Scale(320, 240, 0, 0, 320, 180).get() == topAgain.get());
    device->Retire();
    check(!input->Scale(320, 240, 0, 0, 320, 180) && !topAgain->ToI420());
}
