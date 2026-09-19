#pragma once
#include "../backend-comparison/ComparisonScene.h"
#include "media/webrtc/WindowsRoomRuntime.h"
#include "core/WindowsMediaRuntime.h"
#include "media/audio/SilentPcmCapture.h"
#include "media/audio/DiscardPcmPlayout.h"
#include "media/webrtc/D3dVideoFrameBuffer.h"
#include "../../frontend/shared/LatestRoomVideoFrame.h"
#include "render/FramePresentationBackend.h"
#include <psapi.h>
#include <processsnapshot.h>
#include <set>
#include <string_view>
#include <vector>

namespace loadproof {
using Clock = std::chrono::steady_clock;
inline QJsonObject HandleTypes() {
    HPSS snapshot{};
    Check(PssCaptureSnapshot(GetCurrentProcess(), PSS_CAPTURE_HANDLES | PSS_CAPTURE_HANDLE_NAME_INFORMATION, 0, &snapshot) == ERROR_SUCCESS);
    struct Snapshot { HPSS value; ~Snapshot() { PssFreeSnapshot(GetCurrentProcess(), value); } } owned{snapshot};
    HPSSWALK marker{}; Check(PssWalkMarkerCreate(nullptr, &marker) == ERROR_SUCCESS);
    struct Marker { HPSSWALK value; ~Marker() { PssWalkMarkerFree(value); } } walk{marker};
    QJsonObject types;
    PSS_HANDLE_ENTRY entry{}; DWORD status;
    while ((status = PssWalkSnapshot(snapshot, PSS_WALK_HANDLES, marker, &entry, sizeof(entry))) == ERROR_SUCCESS) {
        auto type = entry.TypeName ? QString::fromWCharArray(entry.TypeName, entry.TypeNameLength / sizeof(wchar_t)) : QStringLiteral("Unknown");
        while (type.endsWith(QChar(0))) type.chop(1);
        types[type] = types[type].toInt() + 1;
    }
    Check(status == ERROR_NO_MORE_ITEMS);
    return types; // Object names/addresses are deliberately not retained.
}
class Sink final : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
    std::mutex mutex_;
    unsigned frames_ = 0, invalid_ = 0;
    std::set<unsigned> ids_;
public:
    std::shared_ptr<LatestRoomVideoFrame> presentation;
    HANDLE presentationReady = nullptr; // Owned until the room's callbacks drain.
    void OnFrame(const webrtc::VideoFrame& frame) override {
        if (presentation) { presentation->OnFrame(frame); SetEvent(presentationReady); }
        const auto pixels = frame.video_frame_buffer()->ToI420();
        std::lock_guard lock(mutex_); ++frames_;
        if (!pixels || frame.width() != 1920 || frame.height() != 1080) { ++invalid_; return; }
        unsigned id = 0;
        for (unsigned bit = 0; bit < 16; ++bit) {
            const int x = int(32 + bit * 36 + 18) * 3;
            const int upper = pixels->DataY()[172 * 3 * pixels->StrideY() + x];
            const int lower = pixels->DataY()[196 * 3 * pixels->StrideY() + x];
            if (upper > 170 && lower < 85) id |= 1u << bit;
            else if (!(upper < 85 && lower > 170)) { ++invalid_; return; }
        }
        if (!id) { ++invalid_; return; }
        ids_.insert(id);
    }
    void Reset() { std::lock_guard lock(mutex_); frames_ = invalid_ = 0; ids_.clear(); }
    QJsonObject Read() {
        std::lock_guard lock(mutex_);
        return {{"frames", int(frames_)}, {"freshFrames", int(ids_.size())}, {"invalidFrames", int(invalid_)}};
    }
};
// Uses the production bounded handoff and presentation backend on its window
// owner thread. No OS input is sent; successful Present is not photon timing.
class Presentation {
    HWND window_ = nullptr;
    HANDLE ready_ = nullptr;
    FramePresentationSession backend_;
    std::shared_ptr<LatestRoomVideoFrame> frames_;
    uint64_t presented_ = 0;
    EXECUTION_STATE previousExecutionState_ = 0;
public:
    explicit Presentation(std::shared_ptr<LatestRoomVideoFrame> frames) : frames_(std::move(frames)) {
        WNDCLASSW type{}; type.lpfnWndProc = DefWindowProcW;
        type.hInstance = GetModuleHandle(nullptr); type.lpszClassName = L"ScreenShareLoadPresentation";
        Check(RegisterClassW(&type) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
        window_ = CreateWindowExW(WS_EX_NOACTIVATE, type.lpszClassName, L"ScreenShare silent presentation test",
            WS_OVERLAPPEDWINDOW, 50, 50, 960, 540, nullptr, nullptr, type.hInstance, nullptr);
        Check(window_ != nullptr); ShowWindow(window_, SW_SHOWNOACTIVATE);
        Check(SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
        previousExecutionState_ = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
        ready_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); Check(ready_ != nullptr);
    }
    ~Presentation() {
        frames_->Stop(); backend_.Release(); DestroyWindow(window_);
        CloseHandle(ready_);
        if (previousExecutionState_) SetThreadExecutionState(previousExecutionState_);
    }
    HANDLE readyEvent() const { return ready_; }
    void WaitForFrame() {
        Check(MsgWaitForMultipleObjectsEx(1, &ready_, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE) != WAIT_FAILED);
    }
    void Pump() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Check(IsWindow(window_));
        if (auto frame = frames_->Take()) {
            RECT client{}; Check(GetClientRect(window_, &client));
            const auto pixels = frame->native ? std::span<const uint8_t>{} : frame->pixels();
            screenshare::Nv12D3D11Presenter::FrameView view{frame->width, frame->height,
                pixels.data(), pixels.size(), frame->native ? frame->native->texture() : nullptr};
            if (backend_.Present(window_, client.right, client.bottom, false, true, view)) ++presented_;
            Check(!backend_.statistics().terminal);
        }
    }
    QJsonObject Read() const {
        const auto stats = backend_.statistics(); const auto queue = frames_->statistics();
        RECT client{}; GetClientRect(window_, &client);
        return {{"presented", qint64(presented_)}, {"errors", qint64(stats.errors)},
            {"hardwareAccelerated", stats.hardwareAccelerated},
            {"visible", bool(IsWindowVisible(window_))}, {"clientWidth", int(client.right)}, {"clientHeight", int(client.bottom)},
            {"recoveries", qint64(stats.recoveries)}, {"maximumFrameLatency", int(stats.maximumFrameLatency)},
            {"busyDrops", qint64(stats.busyDrops)}, {"occludedDrops", qint64(stats.occludedDrops)},
            {"unavailableDrops", qint64(stats.unavailableDrops)}, {"minimizedDrops", qint64(stats.minimizedDrops)},
            {"pending", qint64(queue.pending)}, {"replaced", qint64(queue.replaced)}, {"maxWaitUs", qint64(queue.maxWaitUs)}};
    }
    QJsonObject ExerciseRecovery() {
        auto advance = [&] {
            const auto target = presented_ + 8; const auto deadline = Clock::now() + 3s;
            while (presented_ < target && Clock::now() < deadline) { Pump(); WaitForFrame(); }
            return presented_ >= target;
        };
        auto resize = [&](int width, int height) {
            return SetWindowPos(window_, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) && advance();
        };
        const bool resizedSmall = resize(640, 360), resizedLarge = resize(1280, 720);
        ShowWindow(window_, SW_MINIMIZE); Pump();
        const auto before = presented_; const auto dropped = backend_.statistics().minimizedDrops;
        const auto until = Clock::now() + 500ms;
        do { Pump(); WaitForFrame(); } while (Clock::now() < until);
        const bool minimized = presented_ == before && backend_.statistics().minimizedDrops > dropped && frames_->statistics().pending <= 1;
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        const bool restored = advance();
        return {{"resizeSmall", resizedSmall}, {"resizeLarge", resizedLarge}, {"minimizedBounded", minimized},
            {"restored", restored}, {"passed", resizedSmall && resizedLarge && minimized && restored && backend_.statistics().errors == 0}};
    }
};
inline QJsonObject Resources() {
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory); DWORD handles = 0;
    Check(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) &&
        GetProcessHandleCount(GetCurrentProcess(), &handles));
    return {{"privateBytes", qint64(memory.PrivateUsage)}, {"workingSetBytes", qint64(memory.WorkingSetSize)}, {"handles", int(handles)}};
}
inline double CpuSeconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    Check(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user));
    auto number = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return double(number(kernel) + number(user)) / 1e7;
}
// Attribution only: isolates graphics allocation/readback from MF, WebRTC and
// signaling. These measurements are never a streaming acceptance result.
__declspec(noinline) inline void GpuResourcePhase(unsigned phase) {
    volatile unsigned observed = phase; (void)observed; // Debugger boundary, no production hook.
}
inline QJsonObject GpuResources(int seconds) {
    Check(seconds >= 10 && seconds <= 120);
    auto device = std::make_shared<screenshare::media::D3dVideoDevice>();
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi; Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Check(SUCCEEDED(device->device()->QueryInterface(IID_PPV_ARGS(&dxgi))));
    Check(SUCCEEDED(dxgi->GetAdapter(&adapter))); DXGI_ADAPTER_DESC description{};
    Check(SUCCEEDED(adapter->GetDesc(&description)));
    const auto adapterName = QString::fromWCharArray(description.Description);
    adapter.Reset(); dxgi.Reset();
    std::vector<uint8_t> pixels(1920 * 1080 * 3 / 2, 128);
    QJsonArray phases;
    unsigned phaseIndex = 0;
    for (const char* mode : {"idle", "raw-upload", "upload", "upload-readback"}) {
        GpuResourcePhase(phaseIndex++);
        QJsonObject phase{{"mode", mode}, {"before", Resources()}, {"handleTypesBefore", HandleTypes()}};
        const auto start = Clock::now(); unsigned frames = 0;
        while (Clock::now() - start < std::chrono::seconds(seconds)) {
            if (std::string_view(mode) == "raw-upload") {
                D3D11_TEXTURE2D_DESC description{};
                description.Width = 1920; description.Height = 1080;
                description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
                description.Format = DXGI_FORMAT_NV12; description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = pixels.data(); initial.SysMemPitch = 1920;
                Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
                Check(SUCCEEDED(device->device()->CreateTexture2D(&description, &initial, &texture)));
            } else if (std::string_view(mode) != "idle") {
                auto frame = device->UploadNv12(1920, 1080, pixels);
                if (std::string_view(mode) == "upload-readback") {
                    const auto image = frame->ToI420(); Check(image && image->DataY()[0] == 128);
                }
            }
            std::this_thread::sleep_until(start + std::chrono::microseconds(++frames * 16667LL));
        }
        phase["iterations"] = int(frames); phase["after"] = Resources();
        phase["handleTypesAfter"] = HandleTypes(); phases.append(phase);
    }
    const auto staging = device->readbackStagingAllocations(); device.reset();
    return {{"diagnosticOnly", true}, {"adapter", adapterName}, {"phases", phases}, {"stagingAllocations", qint64(staging)},
        {"afterStop", Resources()}, {"handleTypesStopped", HandleTypes()}};
}
inline QJsonObject Run(bool host, const std::string& origin, const QString& invitation, int seconds, bool hardwareDecode = true, bool present = false, unsigned viewers = 1) {
    Check(seconds >= 10 && seconds <= 300);
    Check(viewers == 1 || viewers == 4);
    screenshare::WindowsMediaRuntime media;
    std::unique_ptr<proof1080::Scene> scene;
    if (host) scene = std::make_unique<proof1080::Scene>("motion");
    auto sink = std::make_shared<Sink>();
    std::unique_ptr<Presentation> presentation;
    if (present && !host) {
        sink->presentation = std::make_shared<LatestRoomVideoFrame>();
        presentation = std::make_unique<Presentation>(sink->presentation);
        sink->presentationReady = presentation->readyEvent();
    }
    auto waitUntil = [&](Clock::time_point until) {
        if (!presentation) { std::this_thread::sleep_until(until); return; }
        do { presentation->Pump(); presentation->WaitForFrame(); } while (Clock::now() < until);
    };
    WindowsRoomRuntimeOptions runtime;
    runtime.preferHardwareDecoding = hardwareDecode;
    runtime.capture.sourceType = screenshare::CaptureSourceType::Window;
    runtime.capture.windowHandle = scene ? uint64_t(scene->window()) : 0;
    runtime.capture.targetWidth = 1920; runtime.capture.targetHeight = 1080; runtime.capture.targetFps = 60;
    runtime.preferences.resolution = ResolutionMode::Fixed;
    runtime.preferences.width = 1920; runtime.preferences.height = 1080;
    runtime.preferences.fps = 60; runtime.preferences.fpsMode = SettingMode::Manual;
    runtime.preferences.bitrateMode = SettingMode::Manual; runtime.preferences.bitrateLimitBps = 12000000;
    runtime.audioEndpoints = PcmEndpointFactories{[] { return std::make_unique<SilentPcmCapture>(); },
        [] { return std::make_unique<DiscardPcmPlayout>(); }};
    runtime.frames = sink; // No OS input sink, audio capture or speakers.
    RoomSession room(WindowsRoomRuntimeFactory(std::move(runtime)));
    RoomOptions options; options.origin = origin; options.host = host; options.publicRoom = false; options.viewerLimit = viewers;
    options.name = "1080p hardware acceptance"; options.nickname = host ? "LoadHost" : "LoadViewer";
    if (!host) options.roomId = invitation.toStdString();
    auto start = room.Start(options); Check(Get(start).error == RoomError::None);
    if (host) {
        Check(!QFile::exists(invitation)); QSaveFile ready(invitation); Check(ready.open(QIODevice::WriteOnly));
        const auto bytes = QJsonDocument(QJsonObject{{"roomId", QString::fromStdString(room.Status().roomId)}}).toJson(QJsonDocument::Compact);
        Check(ready.write(bytes) == bytes.size() && ready.commit());
        const auto deadline = Clock::now() + 60s;
        while (room.Status().activePeers != viewers) { Check(Clock::now() < deadline && room.Status().phase == RoomPhase::Active); std::this_thread::sleep_for(10ms); }
    } else {
        try { Wait([&] { if (presentation) presentation->Pump(); return sink->Read()["freshFrames"].toInt() >= 60; }); }
        catch (...) {
            auto evidence = sink->Read();
            evidence["passed"] = false; evidence["role"] = "viewer";
            evidence["failedStage"] = "initial-fresh-images";
            auto stop = room.Stop(); Get(stop);
            evidence["runtimeReleased"] = true;
            return evidence;
        }
    }
    // Let rate/telemetry settle before collecting load samples.
    waitUntil(Clock::now() + 5s);
    const auto presentationBefore = presentation ? presentation->Read()["presented"].toInteger() : 0;
    const auto handlesBefore = HandleTypes();
    sink->Reset(); const auto began = Clock::now(); const double cpu = CpuSeconds();
    QJsonArray samples; bool hardwareEncoder = false, hardwareDecoder = false, hardwareOnly = hardwareDecode;
    bool decoderObserved = false, decoderMatched = true, encoderHardwareOnly = true, sessionHealthy = true;
    const auto deadline = began + std::chrono::seconds(seconds + 30);
    unsigned measured = 0;
    do {
        waitUntil(began + std::chrono::seconds(++measured));
        const auto state = room.Status();
        sessionHealthy = state.phase == RoomPhase::Active && state.failedPeers == 0;
        if (host && measured <= unsigned(seconds) && state.activePeers != viewers) sessionHealthy = false;
        auto sample = Resources(); sample["second"] = int(measured); sample["activePeers"] = int(state.activePeers);
        sample["roomPhase"] = int(state.phase); sample["failedPeers"] = int(state.failedPeers);
        sample["roomError"] = int(state.error); sample["pendingPeers"] = int(state.pendingPeers);
        if (host) {
            QJsonArray peers;
            sample["hardwareFrames"] = qint64(state.stream.codec.hardwareFrames);
            sample["softwareFallbacks"] = qint64(state.stream.codec.softwareFallbacks);
            hardwareOnly = hardwareOnly && state.stream.codec.softwareFallbacks == 0;
            encoderHardwareOnly = encoderHardwareOnly && state.stream.codec.softwareFallbacks == 0;
            for (const auto& peer : state.stream.peers) {
                QJsonObject observation{{"encoder", CodecImplementationName(peer.sender.encoder)}};
                sample["encoder"] = CodecImplementationName(peer.sender.encoder);
                if (peer.sender.encoder == CodecImplementation::MfH264Hardware) hardwareEncoder = true;
                else { hardwareOnly = false; encoderHardwareOnly = false; }
                if (peer.receiver.observation && !peer.receiver.stale) {
                    observation["decoder"] = CodecImplementationName(peer.receiver.observation->decoder);
                    observation["decodedFrames"] = int(peer.receiver.observation->framesDecoded);
                    sample["decoder"] = CodecImplementationName(peer.receiver.observation->decoder);
                    sample["receiverWidth"] = int(peer.receiver.observation->width);
                    sample["receiverHeight"] = int(peer.receiver.observation->height);
                    sample["decodedFrames"] = int(peer.receiver.observation->framesDecoded);
                    if (peer.receiver.observation->decoder == CodecImplementation::MfH264Hardware) hardwareDecoder = true;
                    else hardwareOnly = false;
                    decoderObserved = true;
                    decoderMatched = decoderMatched && peer.receiver.observation->decoder ==
                        (hardwareDecode ? CodecImplementation::MfH264Hardware : CodecImplementation::MfH264Software);
                }
                peers.append(observation);
            }
            sample["peers"] = peers;
        } else {
            sample["video"] = sink->Read();
            if (presentation) sample["presentation"] = presentation->Read();
        }
        samples.append(sample);
        if (!sessionHealthy) break; // Preserve measured evidence on disconnect/failure.
        if (host && state.activePeers == 0) break;
        Check(Clock::now() < deadline);
    } while (host || measured < unsigned(seconds));
    const double elapsed = std::chrono::duration<double>(Clock::now() - began).count();
    auto result = sink->Read(); result["role"] = host ? "host" : "viewer";
    result["measuredSeconds"] = elapsed; result["cpuCorePercent"] = 100 * (CpuSeconds() - cpu) / elapsed;
    result["samples"] = samples; result["hardwareEncoderObserved"] = hardwareEncoder;
    result["hardwareDecoderObserved"] = hardwareDecoder;
    result["hardwareOnly"] = hardwareOnly;
    result["decoderMode"] = hardwareDecode ? "hardware" : "software";
    result["consumerMode"] = present ? "presentation" : "pixels";
    bool presentationPassed = true;
    if (presentation) {
        auto stats = presentation->Read();
        const auto count = stats["presented"].toInteger() - presentationBefore;
        stats["measuredPresented"] = count; stats["presentedFps"] = count / elapsed;
        result["presentation"] = stats;
        presentationPassed = count / elapsed >= 45 && stats["errors"].toInteger() == 0 &&
            stats["maximumFrameLatency"].toInt() == 1 && stats["hardwareAccelerated"].toBool();
    }
    result["decoderObserved"] = decoderObserved;
    result["expectedViewers"] = int(viewers);
    result["freshFps"] = result["freshFrames"].toInt() / elapsed;
    result["width"] = 1920; result["height"] = 1080; result["fpsLimit"] = 60; result["bitrateLimitBps"] = 12000000;
    result["physicalInput"] = false; result["audibleOutput"] = false; result["externalLatencyVerified"] = false;
    if (presentation && sessionHealthy) {
        result["presentationRecovery"] = presentation->ExerciseRecovery();
        result["postRecoveryVideo"] = sink->Read();
        presentationPassed = presentationPassed && result["presentationRecovery"].toObject()["passed"].toBool() &&
            result["postRecoveryVideo"].toObject()["invalidFrames"].toInt() == 0 &&
            result["postRecoveryVideo"].toObject()["freshFrames"].toInt() > result["freshFrames"].toInt();
    }
    result["handleTypesBefore"] = handlesBefore; result["handleTypesAfter"] = HandleTypes();
    auto stop = room.Stop(); Get(stop); scene.reset(); presentation.reset(); result["afterStopResources"] = Resources();
    result["handleTypesStopped"] = HandleTypes();
    result["runtimeReleased"] = true;
    result["sessionHealthy"] = sessionHealthy;
    result["passed"] = sessionHealthy && (host ? hardwareEncoder && decoderObserved && decoderMatched &&
        encoderHardwareOnly && measured >= unsigned(seconds) :
        result["freshFps"].toDouble() >= 45 && result["invalidFrames"].toInt() == 0 && presentationPassed);
    return result;
}
}
