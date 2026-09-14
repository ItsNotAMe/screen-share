#include "codec/H264StreamDecoder.h"
#include "codec/H264StreamEncoder.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void Probe(bool hardware) {
    using Clock = std::chrono::steady_clock;
    constexpr int width = 640;
    constexpr int height = 360;
    screenshare::CapturedFrame frame;
    frame.width = frame.sourceWidth = width;
    frame.height = frame.sourceHeight = height;
    frame.nv12Pixels.assign(width * height * 3 / 2, std::byte{128});
    std::fill_n(frame.nv12Pixels.begin(), width * height, std::byte{80});
    if (hardware) {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        D3D_FEATURE_LEVEL level;
        Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &device, &level, nullptr)), "D3D device creation failed");
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = description.ArraySize = 1;
        description.Format = DXGI_FORMAT_NV12;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = frame.nv12Pixels.data();
        initial.SysMemPitch = width;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Require(SUCCEEDED(device->CreateTexture2D(&description, &initial, &texture)), "NV12 texture creation failed");
        frame.nv12Texture = texture;
        frame.d3dDevice = device;
        // Only the retained frame references remain after this scope.
    }
    for (int cycle = 0; cycle != 2; ++cycle) {
        screenshare::H264StreamEncoder encoder;
        screenshare::H264StreamDecoder decoder;
        screenshare::H264StreamEncoderConfig config;
        config.width = width;
        config.height = height;
        config.fps = 60;
        config.bitrate = 1'000'000;
        config.backend = hardware ? screenshare::H264StreamEncoderBackend::Hardware : screenshare::H264StreamEncoderBackend::Software;
        config.d3dDevice = frame.d3dDevice;
        encoder.Start(config);
        decoder.Start();
        unsigned decoded = 0;
        unsigned keyframes = 0;
        double longestEncodeMs = 0;
        size_t maxPending = 0;
        auto consume = [&](const std::vector<screenshare::EncodedPacket>& packets) {
            for (const auto& packet : packets) {
                keyframes += packet.isKeyframe;
                for (const auto& output : decoder.DecodePacket(packet)) {
                    Require(output.width == width && output.height == height, "Visible aperture changed");
                    Require(!output.data.empty(), "Decoder returned empty pixels");
                    ++decoded;
                }
            }
        };
        auto next = Clock::now();
        for (int index = 0; index != 120; ++index) {
            if (index == 30) Require(encoder.RequestKeyframe(), "Keyframe request rejected");
            if (index == 60) Require(encoder.TryUpdateBitrate(400'000), "Rate update rejected");
            LARGE_INTEGER timestamp;
            QueryPerformanceCounter(&timestamp);
            frame.lastPresentTimeQpc = timestamp.QuadPart;
            const auto before = Clock::now();
            auto packets = encoder.EncodeFrame(frame);
            longestEncodeMs = std::max(longestEncodeMs, std::chrono::duration<double, std::milli>(Clock::now() - before).count());
            maxPending = std::max(maxPending, encoder.queuedInputCount());
            consume(packets);
            next += std::chrono::microseconds(16'667);
            std::this_thread::sleep_until(next);
        }
        consume(encoder.Drain());
        decoded += static_cast<unsigned>(decoder.Drain().size());
        std::cout << "backend=" << (hardware ? "hardware" : "software") << " cycle=" << cycle
                  << " decoded=" << decoded << " keyframes=" << keyframes
                  << " max_encode_call_ms=" << longestEncodeMs << " max_pending=" << maxPending
                  << " input=" << screenshare::H264StreamEncoderInputModeName(encoder.lastInputMode()) << '\n';
        Require(decoded == 120, "Frames were lost or retained by the codec");
        Require(keyframes >= 2, "Requested keyframe was not produced");
        Require(longestEncodeMs < 500, "Codec call exceeded the 500ms stall threshold");
        encoder.Stop();
        decoder.Stop();
    }
}
}

int main(int argc, char** argv) {
    try {
        Probe(argc == 2 && std::string(argv[1]) == "--hardware");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
