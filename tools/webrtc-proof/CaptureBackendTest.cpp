#include "capture/CaptureBackendPolicy.h"
#include "capture/DxgiCursor.h"
#include "media/capture/WindowsCaptureSource.h"
#include "media/capture/SwitchableCaptureSource.h"
#include "core/WindowsMediaRuntime.h"
#include "core/ShortWait.h"
#include "CaptureTestWindow.h"
#include <array>
#include <cstring>
#include <iostream>
#include <string_view>

using namespace screenshare;
using namespace screenshare::media;
using Microsoft::WRL::ComPtr;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Check(HRESULT hr) { Require(SUCCEEDED(hr), "Test D3D operation failed"); }
template<class Predicate> void Wait(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!predicate()) {
        Require(std::chrono::steady_clock::now() < end, "Capture lifecycle deadline exceeded");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
void Policy() {
    for (auto hr : {E_NOINTERFACE, E_NOTIMPL, REGDB_E_CLASSNOTREG, DXGI_ERROR_UNSUPPORTED,
                   HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), E_ACCESSDENIED, E_FAIL, DXGI_ERROR_DEVICE_REMOVED}) {
        for (bool enabled : {false, true}) {
            int fallback = 0; bool caught = false;
            try { StartDisplayCapture(enabled, [&] { throw CaptureBackendError(hr, "Injected"); }, [&] { ++fallback; }); }
            catch (const CaptureBackendError& error) { caught = error.result() == hr; }
            const bool allowed = enabled && CaptureBackendUnavailable(hr);
            Require(fallback == int(allowed) && caught != allowed, "Unsafe fallback classification");
        }
    }
    int fallback = 0;
    StartDisplayCapture(true, [] {}, [&] { ++fallback; });
    try { StartDisplayCapture(true, [] { throw std::runtime_error("Source gone"); }, [&] { ++fallback; }); }
    catch (const std::runtime_error&) {}
    Require(!fallback, "Fallback after successful or arbitrary failed capture");
    DXGI_OUTDUPL_POINTER_SHAPE_INFO bad{};
    DxgiCursor cursor; bool rejected = false;
    try { cursor.Shape(bad, {}); } catch (const std::runtime_error&) { rejected = true; }
    Require(rejected, "Empty cursor accepted");
    bad = {DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 1025, 1, 4100, {}};
    rejected = false;
    try { cursor.Shape(bad, {}); } catch (const std::runtime_error&) { rejected = true; }
    Require(rejected, "Oversized cursor accepted");
}
void CursorPixels() {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    const std::array<uint32_t, 8> pixels{0xff204060, 0xff204060, 0xff204060, 0xff204060,
                                      0xff204060, 0xff204060, 0xff204060, 0xff204060};
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = 4; desc.Height = 2; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA initial{pixels.data(), 16, 0};
    ComPtr<ID3D11Texture2D> desktop, readback;
    Check(device->CreateTexture2D(&desc, &initial, &desktop));
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(device->CreateTexture2D(&desc, nullptr, &readback));
    auto read = [&](DxgiCursor& cursor, UINT sourceWidth = 4, UINT sourceHeight = 2) {
        context->CopyResource(readback.Get(), cursor.Composite(device.Get(), context.Get(), desktop.Get(), sourceWidth, sourceHeight));
        D3D11_MAPPED_SUBRESOURCE mapped{}; Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::array<uint32_t, 8> result{};
        for (int y = 0; y < 2; ++y) std::memcpy(result.data() + y * 4, static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, 16);
        context->Unmap(readback.Get(), 0); return result;
    };
    DxgiCursor cursor;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 2, 1, 8, {99, 99}};
    const std::array<uint32_t, 2> color{0xffff0000, 0x80000000};
    cursor.Shape(shape, std::as_bytes(std::span(color))); cursor.Position({1, 0}, true);
    auto result = read(cursor);
    Require(result[0] == pixels[0] && result[1] == color[0] && result[3] == pixels[3], "Color cursor placement/hotspot wrong");
    Require(result[2] == 0xff102030, "Straight alpha cursor blend wrong");
    cursor.Position({-1, 0}, true); result = read(cursor);
    Require(result[0] == 0xff102030 && result[1] == pixels[1], "Negative cursor clipping wrong");
    cursor.Position({0, 0}, false); Require(read(cursor) == pixels, "Invisible cursor changed pixels");
    const std::array<std::byte, 2> mono{std::byte{0x30}, std::byte{0x50}};
    shape = {DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME, 4, 2, 1, {}};
    cursor.Shape(shape, mono); cursor.Position({0, 0}, true); result = read(cursor);
    Require(result[0] == 0xff000000 && result[1] == 0xffffffff && result[2] == pixels[2] && result[3] == 0xffdfbf9f,
            "Monochrome AND/XOR truth table wrong");
    const std::array<uint32_t, 2> masked{0x00112233, 0xff010203};
    shape = {DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR, 2, 1, 8, {}};
    cursor.Shape(shape, std::as_bytes(std::span(masked))); result = read(cursor);
    Require(result[0] == 0xff112233 && result[1] == 0xff214263 && result[2] == pixels[2], "Masked color XOR wrong");
    shape = {DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 2, 1, 8, {}};
    cursor.Shape(shape, std::as_bytes(std::span(color))); cursor.Position({1, 1}, true);
    result = read(cursor, 8, 4);
    Require(result[0] == 0xffff0000 && result[1] == pixels[1], "Scaled pointer coordinates wrong");
    std::cout << "Synthetic GPU cursor pixels passed (WARP; no physical input)\n";
}
void WindowLifecycle() {
    WindowsMediaRuntime runtime;
    Require(SUCCEEDED(runtime.result()), "MTA runtime failed");
    proof::TestWindow window;
    window.Invoke([&] { ShowWindow(window.handle(), SW_MINIMIZE); });
    CaptureConfig config; config.sourceType = CaptureSourceType::Window;
    config.windowHandle = reinterpret_cast<uint64_t>(window.handle());
    auto control = std::make_shared<CaptureSwitchControl>(CaptureSelection{});
    CaptureSession session(1, [=] {
        return std::make_unique<SwitchableCaptureSource>([=] { return std::make_unique<WindowsCaptureSource>(config); }, control);
    }, [](auto) {}, std::chrono::milliseconds(200));
    session.EnableDelivery();
    Wait([&] { return session.status().state == CaptureState::Minimized; });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Require(session.status().delivered == 0 && session.status().state == CaptureState::Minimized, "Minimized startup was not paused");
    window.Invoke([&] { ShowWindow(window.handle(), SW_SHOWNOACTIVATE); });
    Wait([&] { return session.status().delivered > 2; });
    Require(session.status().source.implementation == CaptureImplementation::WindowsGraphicsCapture &&
        !session.status().source.fallback, "Capture backend observation missing");
    window.Invoke([&] { ShowWindow(window.handle(), SW_MINIMIZE); });
    Wait([&] { return session.status().state == CaptureState::Minimized; });
    const auto paused = session.status().delivered;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Require(session.status().delivered == paused, "Minimized source leaked queued frames");
    window.Close();
    Wait([&] { return session.status().state == CaptureState::Closed; }); session.Stop();
    Require(session.status().failure == CaptureFailure::None, "Closed source became generic failure");
    config.backend = CaptureBackend::DesktopDuplication; config.allowDisplayFallback = true;
    DesktopCapturer invalid; bool rejected = false;
    try { invalid.Start(config); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "Window capture broadened to desktop duplication");
    std::cout << "Generated-window minimize/restore/close privacy checks passed\n";
}
void DisplayRebuild() {
    WindowsMediaRuntime runtime;
    Require(SUCCEEDED(runtime.result()), "MTA runtime failed");
    proof::TestWindow window;
    for (const auto backend : {CaptureBackend::WindowsGraphicsCapture, CaptureBackend::DesktopDuplication}) {
        CaptureConfig config; config.backend = backend; config.ownedNv12 = config.includeNv12 = true;
        config.includeBgraReadback = config.includeNv12Readback = false;
        config.targetWidth = 640; config.targetHeight = 360;
        DesktopCapturer capture;
        try { capture.Start(config); }
        catch (const CaptureBackendError& error) {
            if (backend != CaptureBackend::DesktopDuplication || error.result() != DXGI_ERROR_UNSUPPORTED) throw;
            std::cout << "DXGI display unavailable on this desktop mode: " << error.what() << "; rebuild acceptance remains open\n";
            continue;
        }
        std::optional<CapturedFrame> frame;
        Wait([&] { frame = capture.TryCaptureFrame(std::chrono::milliseconds(10)); return frame.has_value(); });
        auto previousDevice = frame->d3dDevice;
        Require(frame->nv12OwnedAndComplete && frame->pixels.empty() && frame->nv12Pixels.empty(), "Display pixels read back unexpectedly");
        capture.RebuildDevice(); frame.reset();
        Wait([&] { frame = capture.TryCaptureFrame(std::chrono::milliseconds(10)); return frame.has_value(); });
        Require(frame->d3dDevice != previousDevice && frame->nv12OwnedAndComplete, "Pinned display rebuild failed");
        capture.Stop();
        std::cout << (backend == CaptureBackend::WindowsGraphicsCapture ? "WGC" : "DXGI") << " pinned-display rebuild passed\n";
    }
    std::cout << "Display checks finished; no pixel files/readback\n";
}
void StaticCadence() {
    WindowsMediaRuntime runtime;
    Require(SUCCEEDED(runtime.result()), "MTA runtime failed");
    proof::TestWindow window(false);
    for(int fps : {30,60}) {
        CaptureConfig config;config.sourceType=CaptureSourceType::Window;
        config.windowHandle=reinterpret_cast<uint64_t>(window.handle());
        config.targetWidth=640;config.targetHeight=360;config.targetFps=fps;
        WindowsCaptureSource source(config);source.Start();
        std::optional<CaptureSample> frame;
        Wait([&]{frame=source.Poll();return frame.has_value();});
        auto previous=frame->resource;
        unsigned frames=0,repeated=0;
        const auto start=std::chrono::steady_clock::now();
        ShortWait wait;
        while(std::chrono::steady_clock::now()-start<std::chrono::seconds(2)) {
            if(auto sample=source.Poll()) {++frames;if(sample->resource==previous)++repeated;previous=sample->resource;}
            wait.Wait();
        }
        Require(frames>=unsigned(fps*1.8) && frames<=unsigned(fps*2.2),"Static source did not sustain the selected cadence");
        Require(repeated>unsigned(fps),"Static-source test did not exercise retained frames");
        std::cout<<"Static capture "<<fps<<" FPS: "<<frames<<" frames in 2s, "<<repeated<<" retained\n";
        window.Invoke([&]{ShowWindow(window.handle(),SW_MINIMIZE);});
        Wait([&]{source.Poll();return source.Minimized();});
        Require(!source.Poll(),"Minimized source replayed retained pixels");
        window.Invoke([&]{ShowWindow(window.handle(),SW_RESTORE);RedrawWindow(window.handle(),nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW);});
        Wait([&]{frame=source.Poll();return frame.has_value();});
    }
}
int main(int argc, char** argv) try {
    Policy(); CursorPixels();
    if (argc > 1 && std::string_view(argv[1]) == "--live") WindowLifecycle();
    if (argc > 1 && std::string_view(argv[1]) == "--display") DisplayRebuild();
    if (argc > 1 && std::string_view(argv[1]) == "--cadence") StaticCadence();
    std::cout << "Capture backend checks passed\n"; return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
