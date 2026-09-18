#pragma once
#include "../backend-comparison/ComparisonScene.h"
#include "media/webrtc/WindowsRoomRuntime.h"
#include "core/WindowsMediaRuntime.h"
#include "media/audio/SilentPcmCapture.h"
#include "media/audio/DiscardPcmPlayout.h"
#include "media/webrtc/D3dVideoFrameBuffer.h"
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
    void OnFrame(const webrtc::VideoFrame& frame) override {
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
    for (const char* mode : {"idle", "raw-upload", "upload", "upload-readback"}) {
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
inline QJsonObject Run(bool host, const std::string& origin, const QString& invitation, int seconds) {
    Check(seconds >= 10 && seconds <= 300);
    screenshare::WindowsMediaRuntime media;
    std::unique_ptr<proof1080::Scene> scene;
    if (host) scene = std::make_unique<proof1080::Scene>("motion");
    auto sink = std::make_shared<Sink>();
    WindowsRoomRuntimeOptions runtime;
    runtime.capture.sourceType = screenshare::CaptureSourceType::Window;
    runtime.capture.windowHandle = scene ? uint64_t(scene->window()) : 0;
    runtime.capture.targetWidth = 1920; runtime.capture.targetHeight = 1080; runtime.capture.targetFps = 60;
    runtime.preferences.resolution = ResolutionMode::Fixed;
    runtime.preferences.width = 1920; runtime.preferences.height = 1080;
    runtime.preferences.fps = 60; runtime.preferences.fpsMode = SettingMode::Manual;
    runtime.preferences.bitrateMode = SettingMode::Manual; runtime.preferences.bitrateLimitBps = 12000000;
    runtime.audioEndpoints = PcmEndpointFactories{[] { return std::make_unique<SilentPcmCapture>(); },
        [] { return std::make_unique<DiscardPcmPlayout>(); }};
    runtime.frames = sink; // No OS input sink, audio capture, speakers or viewer window.
    RoomSession room(WindowsRoomRuntimeFactory(std::move(runtime)));
    RoomOptions options; options.origin = origin; options.host = host; options.publicRoom = false; options.viewerLimit = 1;
    options.name = "1080p hardware acceptance"; options.nickname = host ? "LoadHost" : "LoadViewer";
    if (!host) options.roomId = invitation.toStdString();
    auto start = room.Start(options); Check(Get(start).error == RoomError::None);
    if (host) {
        Check(!QFile::exists(invitation)); QSaveFile ready(invitation); Check(ready.open(QIODevice::WriteOnly));
        const auto bytes = QJsonDocument(QJsonObject{{"roomId", QString::fromStdString(room.Status().roomId)}}).toJson(QJsonDocument::Compact);
        Check(ready.write(bytes) == bytes.size() && ready.commit());
        const auto deadline = Clock::now() + 60s;
        while (room.Status().activePeers != 1) { Check(Clock::now() < deadline && room.Status().phase == RoomPhase::Active); std::this_thread::sleep_for(10ms); }
    } else Wait([&] { return sink->Read()["freshFrames"].toInt() >= 60; });
    // Let rate/telemetry settle before collecting load samples.
    std::this_thread::sleep_for(5s);
    const auto handlesBefore = HandleTypes();
    sink->Reset(); const auto began = Clock::now(); const double cpu = CpuSeconds();
    QJsonArray samples; bool hardwareEncoder = false, hardwareDecoder = false, hardwareOnly = true;
    const auto deadline = began + std::chrono::seconds(seconds + 30);
    unsigned measured = 0;
    do {
        std::this_thread::sleep_until(began + std::chrono::seconds(++measured));
        const auto state = room.Status(); Check(state.phase == RoomPhase::Active && state.failedPeers == 0);
        auto sample = Resources(); sample["second"] = int(measured); sample["activePeers"] = int(state.activePeers);
        if (host) {
            sample["hardwareFrames"] = qint64(state.stream.codec.hardwareFrames);
            sample["softwareFallbacks"] = qint64(state.stream.codec.softwareFallbacks);
            hardwareOnly = hardwareOnly && state.stream.codec.softwareFallbacks == 0;
            for (const auto& peer : state.stream.peers) {
                sample["encoder"] = CodecImplementationName(peer.sender.encoder);
                if (peer.sender.encoder == CodecImplementation::MfH264Hardware) hardwareEncoder = true;
                else hardwareOnly = false;
                if (peer.receiver.observation && !peer.receiver.stale) {
                    sample["decoder"] = CodecImplementationName(peer.receiver.observation->decoder);
                    sample["receiverWidth"] = int(peer.receiver.observation->width);
                    sample["receiverHeight"] = int(peer.receiver.observation->height);
                    sample["decodedFrames"] = int(peer.receiver.observation->framesDecoded);
                    if (peer.receiver.observation->decoder == CodecImplementation::MfH264Hardware) hardwareDecoder = true;
                    else hardwareOnly = false;
                }
            }
        } else sample["video"] = sink->Read();
        samples.append(sample);
        if (host && state.activePeers == 0) break;
        Check(Clock::now() < deadline);
    } while (host || measured < unsigned(seconds));
    const double elapsed = std::chrono::duration<double>(Clock::now() - began).count();
    auto result = sink->Read(); result["role"] = host ? "host" : "viewer";
    result["measuredSeconds"] = elapsed; result["cpuCorePercent"] = 100 * (CpuSeconds() - cpu) / elapsed;
    result["samples"] = samples; result["hardwareEncoderObserved"] = hardwareEncoder;
    result["hardwareDecoderObserved"] = hardwareDecoder;
    result["hardwareOnly"] = hardwareOnly;
    result["freshFps"] = result["freshFrames"].toInt() / elapsed;
    result["width"] = 1920; result["height"] = 1080; result["fpsLimit"] = 60; result["bitrateLimitBps"] = 12000000;
    result["physicalInput"] = false; result["audibleOutput"] = false; result["externalLatencyVerified"] = false;
    result["handleTypesBefore"] = handlesBefore; result["handleTypesAfter"] = HandleTypes();
    auto stop = room.Stop(); Get(stop); scene.reset(); result["afterStopResources"] = Resources();
    result["handleTypesStopped"] = HandleTypes();
    result["runtimeReleased"] = true;
    result["passed"] = host ? hardwareOnly && hardwareEncoder && hardwareDecoder && measured >= unsigned(seconds) :
        result["freshFps"].toDouble() >= 45 && result["invalidFrames"].toInt() == 0;
    return result;
}
}
