#include "capture/CaptureBackendPolicy.h"
#include "capture/DxgiCursor.h"
#include "media/capture/WindowsCaptureSource.h"
#include "media/capture/SwitchableCaptureSource.h"
#include "media/webrtc/CaptureVideoSource.h"
#include "input/v2/DesktopSink.h"
#include "core/WindowsMediaRuntime.h"
#include "core/ShortWait.h"
#include "CaptureTestWindow.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <string_view>
#include <iomanip>
#include <vector>

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
    const char* stage="initial minimize";
    try {
    Wait([&] { return session.status().state == CaptureState::Minimized; });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Require(session.status().delivered == 0 && session.status().state == CaptureState::Minimized, "Minimized startup was not paused");
    stage="restore first frames";
    window.Invoke([&] { ShowWindow(window.handle(), SW_SHOWNOACTIVATE); });
    Wait([&] { return session.status().delivered > 2; });
    Require(session.status().source.implementation == CaptureImplementation::WindowsGraphicsCapture &&
        !session.status().source.fallback, "Capture backend observation missing");
    window.Invoke([&] { ShowWindow(window.handle(), SW_MINIMIZE); });
    Wait([&] { return session.status().state == CaptureState::Minimized; });
    const auto paused = session.status().delivered;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Require(session.status().delivered == paused, "Minimized source leaked queued frames");
    stage="closed source becomes unavailable";
    window.Close();
    Wait([&] { return session.status().state == CaptureState::Unavailable; });
    stage="replacement source";
    const auto beforeReplacement = session.status().delivered;
    proof::TestWindow replacement;
    config.windowHandle = reinterpret_cast<uint64_t>(replacement.handle());
    auto changed = control->Submit({CaptureKind::Window,0,config.windowHandle,60},
        [=] { return std::make_unique<WindowsCaptureSource>(config); });
    Wait([&] { return changed.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; });
    Require(changed.get().error == CaptureUpdateError::None, "Closed window prevented source replacement");
    Wait([&] { return session.status().delivered > beforeReplacement + 2; });
    session.Stop();
    Require(session.status().failure == CaptureFailure::None, "Closed source became generic failure");
    } catch (...) {
        const auto state=session.status();
        std::cerr<<stage<<": state="<<int(state.state)<<" failure="<<int(state.failure)<<" frames="<<state.delivered<<'\n';throw;
    }
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
double CaptureCpuMs() {
    FILETIME created{}, exited{}, kernel{}, user{};
    Require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user), "Capture CPU query failed");
    auto ticks = [](FILETIME value) { return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
    return double(ticks(kernel) + ticks(user)) / 10000.;
}
void CaptureDemand(bool benchmark) {
    using namespace std::chrono_literals;
    WindowsMediaRuntime runtime;
    Require(SUCCEEDED(runtime.result()), "MTA runtime failed");
    proof::TestWindow window;
    window.Invoke([&] {
        SetWindowLongPtrW(window.handle(), GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(window.handle(), nullptr, 0, 0, 1280, 720,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        SetTimer(window.handle(), 1, 10, nullptr);
    });
    auto demand = std::make_shared<std::atomic<int>>(0);
    auto target = std::make_shared<input::DesktopTargetState>();
    CaptureConfig config; config.sourceType = CaptureSourceType::Window;
    config.windowHandle = reinterpret_cast<uint64_t>(window.handle()); config.targetFps = 60;
    WindowsCaptureSource source(config, target, {}, demand); source.Start();
    std::optional<CaptureSample> sample;
    Wait([&] { sample = source.Poll(); return sample.has_value(); });
    ShortWait timer;
    auto pump = [&](std::chrono::milliseconds duration, unsigned& samples, unsigned& converted) {
        auto previous = sample->resource;
        const auto until = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < until) {
            if (auto frame = source.Poll()) {
                ++samples; converted += frame->resource != previous; previous = frame->resource; sample = std::move(frame);
            }
            timer.Wait();
        }
        Require(target->Read().generation != 0, "Idle capture let input geometry expire");
    };
    if (benchmark) for (unsigned pair = 0; pair < 6; ++pair) {
        for (bool active : {pair % 2 == 0, pair % 2 != 0}) {
            *demand = active ? 60 : 0;
            unsigned ignored = 0, ignoredConversions = 0; pump(500ms, ignored, ignoredConversions);
            unsigned samples = 0, converted = 0;
            const auto cpu = CaptureCpuMs(); const auto begin = std::chrono::steady_clock::now();
            pump(2000ms, samples, converted);
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            const auto used = CaptureCpuMs() - cpu;
            Require(active ? samples >= 95 && samples <= 135 : samples >= 8 && samples <= 12,
                "Native capture demand did not control sample cadence");
            std::cout << std::fixed << std::setprecision(4) << "CAPTURE_DEMAND {\"pair\":" << pair
                << ",\"active\":" << (active ? "true" : "false") << ",\"cpuMs\":" << used
                << ",\"wallMs\":" << elapsed << ",\"samples\":" << samples << ",\"converted\":" << converted << "}\n" << std::flush;
        }
    }
    // Queue two distinct paints without polling, then wake before the 200 ms
    // idle slot. The very first resumed sample must contain the newest paint.
    auto paint = [&](int shade) { window.Invoke([&] {
        KillTimer(window.handle(), 1); SetWindowLongPtrW(window.handle(), GWLP_USERDATA, shade - 70);
        RedrawWindow(window.handle(), nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }); };
    auto center = [&] {
        const auto resource = std::static_pointer_cast<WindowsCaptureResource>(sample->resource);
        const auto pixels = resource->buffer->ToNv12(); Require(bool(pixels), "Demand proof readback failed");
        return pixels->DataY()[(pixels->height() / 2) * pixels->StrideY() + pixels->width() / 2];
    };
    *demand = 0; paint(80);
    Wait([&] { if (auto frame = source.Poll()) sample = std::move(frame); return center() >= 80 && center() <= 95; });
    const auto identity = target->Read().generation;
    paint(110); std::this_thread::sleep_for(50ms);
    paint(170); std::this_thread::sleep_for(60ms);
    *demand = 60;
    const auto awake = std::chrono::steady_clock::now(); sample = source.Poll();
    Require(bool(sample), "Active capture waited for the idle frame slot");
    const auto wakeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - awake).count();
    Require(center() >= 150, "Resumed capture converted an older queued WGC frame");
    Require(target->Read().generation == identity, "Demand change rotated the input identity");
    *demand = 0;
    window.Invoke([&] { ShowWindow(window.handle(), SW_MINIMIZE); });
    Wait([&] { source.Poll(); return source.Minimized(); });
    unsigned ignored = 0, ignoredConversions = 0; pump(1200ms, ignored, ignoredConversions);
    Require(!source.Poll() && target->Read().generation == identity, "Idle minimization replayed pixels or lost input identity");
    window.Invoke([&] { ShowWindow(window.handle(), SW_SHOWNOACTIVATE); });
    Wait([&] { sample = source.Poll(); return sample.has_value(); });
    window.Close();
    Wait([&] { source.Poll(); return source.Closed(); });
    Require(target->Read().generation == 0, "Idle source closure retained input geometry");
    std::cout << "Capture demand: newest first frame, immediate wake (" << wakeMs
        << " ms), input identity, minimize/restore and close passed\n";
}
void StreamRateBenchmark(bool compareRates) {
    using namespace std::chrono_literals;
    WindowsMediaRuntime runtime;
    Require(SUCCEEDED(runtime.result()), "MTA runtime failed");
    proof::TestWindow window;
    window.Invoke([&] {
        SetWindowLongPtrW(window.handle(), GWL_STYLE, WS_POPUP | WS_VISIBLE);
        Require(SetWindowPos(window.handle(), nullptr, 0, 0, 1920, 1080,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED), "Benchmark window resize failed");
        SetTimer(window.handle(), 1, 10, nullptr);
    });
    auto demand = std::make_shared<std::atomic<int>>(60);
    CaptureConfig config;
    config.sourceType = CaptureSourceType::Window;
    config.windowHandle = reinterpret_cast<uint64_t>(window.handle());
    auto track = webrtc::make_ref_counted<CaptureVideoSource>();
    StreamPreferences preferences;
    preferences.resolution = ResolutionMode::Native;
    preferences.fps = compareRates ? 30 : 60;
    track->Configure(preferences, 1);
    struct Sink final : webrtc::VideoSinkInterface<webrtc::VideoFrame> {
        unsigned frames = 0;
        std::vector<double> gaps;
        std::chrono::steady_clock::time_point last{};
        void OnFrame(const webrtc::VideoFrame& frame) override {
            Require(frame.width() == 1920 && frame.height() == 1080, "Benchmark frame dimensions changed");
            const auto now = std::chrono::steady_clock::now();
            if (frames++) gaps.push_back(std::chrono::duration<double, std::milli>(now - last).count());
            last = now;
        }
    } sink;
    track->AddOrUpdateSink(&sink, webrtc::VideoSinkWants());
    WindowsCaptureSource capture(config, {}, {}, demand);
    capture.Start();
    ShortWait timer;
    std::shared_ptr<CaptureResource> previous;
    auto captureCycles = [] {
        ULONG64 cycles = 0;
        Require(QueryThreadCycleTime(GetCurrentThread(), &cycles), "Capture CPU cycle query failed");
        return cycles;
    };
    auto pump = [&](std::chrono::milliseconds duration, unsigned& samples, unsigned& converted) {
        const auto until = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < until) {
            if (auto frame = capture.Poll()) {
                ++samples;
                converted += frame->resource != previous;
                previous = std::move(frame->resource);
                const auto pixels = std::static_pointer_cast<WindowsCaptureResource>(previous);
                track->PushBuffer(pixels->buffer, frame->capturedAt);
            }
            timer.Wait();
        }
    };
    const auto measure = [&](int pair, int rate) {
        *demand = rate;
        unsigned samples = 0, converted = 0;
        pump(500ms, samples, converted);
        samples = converted = sink.frames = 0;
        sink.gaps.clear();
        const auto cpu = CaptureCpuMs();
        const auto cyclesBefore = captureCycles();
        const auto start = std::chrono::steady_clock::now();
        pump(2000ms, samples, converted);
        const auto wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const auto cycles = captureCycles() - cyclesBefore;
        const auto used = CaptureCpuMs() - cpu;
        Require(sink.frames >= (compareRates ? 56u : 114u) && sink.frames <= unsigned(preferences.fps * 2 + 2),
            "Capture did not sustain the requested track rate");
        std::sort(sink.gaps.begin(), sink.gaps.end());
        std::cout << std::fixed << std::setprecision(4) << "CAPTURE_FPS {\"pair\":" << pair
            << ",\"rate\":" << rate << ",\"cpuMs\":" << used << ",\"captureCycles\":" << cycles << ",\"wallMs\":" << wall
            << ",\"samples\":" << samples << ",\"converted\":" << converted << ",\"frames\":" << sink.frames
            << ",\"gapP95Ms\":" << sink.gaps[sink.gaps.size() * 95 / 100] << "}\n" << std::flush;
    };
    if (compareRates) {
        // The 30 FPS track receives both 60 and 30 FPS capture in alternating order.
        for (int pair = 0; pair < 6; ++pair) for (int rate : {pair % 2 ? 30 : 60, pair % 2 ? 60 : 30}) measure(pair, rate);
    } else measure(0, 60);
    track->RemoveSink(&sink);
}
void SourceSwitches() {
    WindowsMediaRuntime runtime;
    Require(SUCCEEDED(runtime.result()), "MTA runtime failed");
    proof::TestWindow original, replacement;
    original.Invoke([&] {
        SetWindowLongPtrW(original.handle(), GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(original.handle(), nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    });
    auto target = std::make_shared<input::DesktopTargetState>();
    CaptureConfig config;
    config.sourceType = CaptureSourceType::Window;
    config.targetWidth = 320; config.targetHeight = 180;
    config.windowHandle = reinterpret_cast<uint64_t>(original.handle());
    auto factory = [target](CaptureConfig selected) -> CaptureSession::Factory {
        return [selected, target] { return std::make_unique<WindowsCaptureSource>(selected, target); };
    };
    auto control = std::make_shared<CaptureSwitchControl>(CaptureSelection{CaptureKind::Window, 0, config.windowHandle, 60});
    CaptureSession session(1, [=] { return std::make_unique<SwitchableCaptureSource>(factory(config), control); }, [](auto) {});
    session.EnableDelivery();
    try { Wait([&] { return session.status().delivered >= 5 && target->Read().generation; }); }
    catch (...) {
        const auto status = session.status();
        std::cerr << "Initial switch source state " << int(status.state) << " failure " << int(status.failure)
            << " frames " << status.delivered << " input generation " << target->Read().generation << '\n';
        throw;
    }
    auto sink = input::CreateWindowsDesktopSink(target);
    Require(sink->Grant("window-viewer", input::Keyboard, 0) && sink->Healthy("window-viewer"),
        "Borderless captured window rejected keyboard permission");
    sink->Release("window-viewer");
    const auto beforeMinimize = target->Read().generation;
    original.Invoke([&] { ShowWindow(original.handle(), SW_MINIMIZE); });
    Wait([&] { return session.status().state == CaptureState::Minimized; });
    // Wait beyond the normal one-second target expiry to exercise paused
    // liveness. Grant/health checks do not inject any desktop input.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    Require(target->Read().generation == beforeMinimize, "Minimized source lost its captured input identity");
    Require(sink->Grant("paused-viewer", input::Keyboard, 0) && sink->Healthy("paused-viewer"), "Minimized window rejected keyboard permission");
    sink->Release("paused-viewer");
    original.Invoke([&] { ShowWindow(original.handle(), SW_SHOWNOACTIVATE); });
    Wait([&] { return session.status().state == CaptureState::Running; });
    for (int cycle = 0; cycle < 6; ++cycle) {
        config.windowHandle = reinterpret_cast<uint64_t>((cycle % 2 ? original : replacement).handle());
        const auto before = session.status().delivered;
        auto changed = control->Submit({CaptureKind::Window, 0, config.windowHandle, 60}, factory(config));
        Wait([&] { return changed.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; });
        Require(changed.get().error == CaptureUpdateError::None, "Generated window switch failed");
        Wait([&] { return session.status().delivered >= before + 10 && target->Read().target.window == config.windowHandle; });
        Require(session.status().state == CaptureState::Running, "Replacement capture stopped");
    }
    session.Stop();
    Require(!target->Read().generation, "Switched capture retained its input target after stop");
    std::cout << "Six generated-window source switches kept frames and input geometry flowing\n";
}
void PreviewReadback() {
    WindowsMediaRuntime runtime;
    Require(SUCCEEDED(runtime.result()), "MTA runtime failed");
    proof::TestWindow window;
    CaptureConfig config; config.sourceType = CaptureSourceType::Window;
    config.windowHandle = reinterpret_cast<uint64_t>(window.handle());
    for (const auto size : {SIZE{1280,720}, SIZE{400,800}, SIZE{300,200}}) {
        window.Invoke([&] { SetWindowPos(window.handle(), nullptr, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); });
        DesktopCapturer capture;
        config.targetWidth = config.targetHeight = 0; config.fitWithinTarget = false;
        capture.Start(config);
        std::optional<CapturedFrame> original;
        Wait([&] { original = capture.TryCaptureFrame(std::chrono::milliseconds(30)); return original && !original->pixels.empty(); });
        const auto originalBytes = original->pixels.size(); capture.Stop();
        config.targetWidth = 640; config.targetHeight = 360; config.fitWithinTarget = true;
        capture.Start(config);
        std::optional<CapturedFrame> preview;
        Wait([&] { preview = capture.TryCaptureFrame(std::chrono::milliseconds(30)); return preview && !preview->pixels.empty(); });
        Require(preview->width <= 640 && preview->height <= 360 && preview->width <= preview->sourceWidth && preview->height <= preview->sourceHeight,
            "Preview exceeded its bounds or upscaled a small source");
        Require(preview->sourceWidth == original->sourceWidth && preview->sourceHeight == original->sourceHeight,
            "Preview discarded original source geometry");
        Require(std::abs(int64_t(preview->width) * preview->sourceHeight - int64_t(preview->height) * preview->sourceWidth) <=
            preview->sourceWidth + preview->sourceHeight, "Preview stretched its source");
        Require(preview->pixels.size() <= originalBytes && preview->pixels.size() == size_t(preview->rowPitch) * preview->height,
            "Preview readback retained native-resolution pixels");
        std::cout << "Preview " << preview->sourceWidth << 'x' << preview->sourceHeight << " -> " << preview->width << 'x' << preview->height
            << "; readback bytes " << originalBytes << " -> " << preview->pixels.size() << '\n';
        capture.Stop();
    }
}
void OversizedSource() {
    WindowsMediaRuntime runtime;
    Require(SUCCEEDED(runtime.result()), "MTA runtime failed");
    proof::TestWindow window;
    const auto resize = [&](int width, int height) {
        window.Invoke([&] {
            SetWindowLongPtrW(window.handle(), GWL_STYLE, WS_POPUP | WS_VISIBLE);
            Require(SetWindowPos(window.handle(), nullptr, 0, 0, width, height,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED), "Proof window resize failed");
        });
    };
    resize(5120, 1440);
    CaptureConfig config;
    config.sourceType = CaptureSourceType::Window;
    config.windowHandle = reinterpret_cast<uint64_t>(window.handle());
    auto target = std::make_shared<input::DesktopTargetState>();
    WindowsCaptureSource source(config, target);
    source.Start();
    const auto verify = [&](int width, int height, int outputWidth, int outputHeight) {
        Wait([&] {
            const auto sample = source.Poll();
            if (!sample) return false;
            const auto geometry = target->Read();
            if (geometry.target.width != width || geometry.target.height != height) return false;
            const auto resource = std::static_pointer_cast<WindowsCaptureResource>(sample->resource);
            Require(resource->buffer->width() == outputWidth && resource->buffer->height() == outputHeight,
                "Oversized capture produced incorrect NV12 dimensions");
            Require(geometry.generation && geometry.generation == resource->inputGeneration && geometry.target.mouseMapped,
                "Capture resize lost the original input geometry");
            return true;
        });
        std::cout << "Room capture " << width << 'x' << height << " -> " << outputWidth << 'x' << outputHeight << '\n';
    };
    verify(5120, 1440, 3840, 1080);
    resize(1280, 720);
    verify(1280, 720, 1280, 720);
}
int main(int argc, char** argv) try {
    Policy(); CursorPixels();
    if (argc > 1 && std::string_view(argv[1]) == "--live") WindowLifecycle();
    if (argc > 1 && std::string_view(argv[1]) == "--display") DisplayRebuild();
    if (argc > 1 && std::string_view(argv[1]) == "--cadence") StaticCadence();
    if (argc > 1 && std::string_view(argv[1]) == "--demand") CaptureDemand(true);
    if (argc > 1 && std::string_view(argv[1]) == "--demand-check") CaptureDemand(false);
    if (argc > 1 && std::string_view(argv[1]) == "--stream-rate") StreamRateBenchmark(true);
    if (argc > 1 && std::string_view(argv[1]) == "--capture-60") StreamRateBenchmark(false);
    if (argc > 1 && std::string_view(argv[1]) == "--switch") SourceSwitches();
    if (argc > 1 && std::string_view(argv[1]) == "--preview") PreviewReadback();
    if (argc > 1 && std::string_view(argv[1]) == "--large-source") OversizedSource();
    std::cout << "Capture backend checks passed\n"; return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
