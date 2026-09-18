#include "api/RoomSession.h"
#include "media/webrtc/WindowsRoomRuntime.h"
#include "core/WindowsMediaRuntime.h"
#include "runtime/ScreenShareSessionRunner.h"
#include "media/audio/SilentPcmCapture.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/win32_socket_init.h"
#include "rtc_base/logging.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QSaveFile>
#include <psapi.h>
#include <iphlpapi.h>
#include <timeapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <future>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>

using namespace screenshare;
using namespace screenshare::media;
using namespace screenshare::v2;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr int Width = 1920, Height = 1080;
double NowMs() { return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count(); }
void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> auto Await(F& value) { Require(value.wait_for(25s) == std::future_status::ready, "Operation timed out"); return value.get(); }
template<class F> void Wait(F condition) {
    const auto deadline = Clock::now() + 25s;
    while (!condition()) { Require(Clock::now() < deadline, "Readiness timed out"); std::this_thread::sleep_for(10ms); }
}

// Only this owned window is captured. Marker generation never injects input.
// Source time and received marker use the same process clock; no remote clock
// subtraction or physical display latency is claimed.
class Scene {
public:
    explicit Scene(std::string mode) : mode_(std::move(mode)) {
        timeBeginPeriod(1);
        std::promise<HWND> ready; auto future = ready.get_future();
        thread_ = std::thread([this, ready = std::move(ready)]() mutable {
            WNDCLASSW type{}; type.lpfnWndProc = Procedure; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"BackendComparisonScene";
            RegisterClassW(&type);
            HWND window = CreateWindowW(type.lpszClassName, L"ScreenShare comparison — generated content only", WS_POPUP,
                40, 40, Width, Height, nullptr, nullptr, type.hInstance, this);
            if (window) { ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window); SetTimer(window, 1, 1, nullptr); }
            ready.set_value(window);
            if (!window) return;
            MSG message; while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        });
        window_ = future.get();
        if (!window_) { thread_.join(); timeEndPeriod(1); throw std::runtime_error("Scene creation failed"); }
    }
    ~Scene() { PostMessageW(window_, WM_CLOSE, 0, 0); thread_.join(); timeEndPeriod(1); }
    HWND window() const { return window_; }
    double Time(unsigned id) { std::lock_guard lock(mutex_); return times_[id]; }
    unsigned count() const { return count_.load(); }
    double CpuSeconds() {
        FILETIME created{}, ended{}, kernel{}, user{};
        Require(GetThreadTimes(thread_.native_handle(), &created, &ended, &kernel, &user), "Scene CPU timing failed");
        auto value = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        return double(value(kernel) + value(user)) / 1e7;
    }
    int Shade(unsigned id, int x, int y) const {
        if (mode_ == "static") return ((x / 20 + y / 20) % 2) ? 180 : 60;
        if (mode_ == "scroll") return (((x + int(id) * 4) / 12 + y / 20) % 2) ? 200 : 40;
        const unsigned hash = (unsigned(x / 8) * 73856093u) ^ (unsigned(y / 8) * 19349663u) ^ (id * 83492791u);
        return 30 + int((hash ^ (hash >> 13)) % 196);
    }
private:
    void Paint(HWND window) {
        const auto time = NowMs(); const unsigned id = ++count_;
        Require(id < times_.size(), "Scene marker exhausted");
        std::vector<uint32_t> pixels(Width * Height);
        for (int y = 0; y < Height; ++y) for (int x = 0; x < Width; ++x) {
            int shade = Shade(id, x, y);
            const int markerX = x * 640 / Width, markerY = y * 360 / Height;
            if (markerY >= 160 && markerY < 208 && markerX >= 32 && markerX < 608) {
                bool bit = (id >> ((markerX - 32) / 36)) & 1;
                if (markerY >= 184) bit = !bit;
                shade = bit ? 230 : 25;
            }
            pixels[y * Width + x] = uint32_t(shade) * 0x010101u;
        }
        { std::lock_guard lock(mutex_); times_[id] = time; }
        PAINTSTRUCT paint; const HDC dc = BeginPaint(window, &paint);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = Width;
        info.bmiHeader.biHeight = -Height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        SetDIBitsToDevice(dc, 0, 0, Width, Height, 0, 0, 0, Height, pixels.data(), &info, DIB_RGB_COLORS);
        EndPaint(window, &paint); GdiFlush();
    }
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        if (message == WM_NCCREATE) SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        auto* self = reinterpret_cast<Scene*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_TIMER && self) {
            const double now = NowMs();
            if (now >= self->nextPaint_) { self->nextPaint_ = std::max(self->nextPaint_ + 1000.0 / 60, now); InvalidateRect(window, nullptr, FALSE); }
            return 0;
        }
        if (message == WM_PAINT && self) { self->Paint(window); return 0; }
        if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(window, message, w, l);
    }
    std::string mode_; HWND window_{}; std::thread thread_; std::mutex mutex_;
    double nextPaint_ = 0;
    std::vector<double> times_ = std::vector<double>(65536); std::atomic<unsigned> count_{0};
};

class Sink final : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    explicit Sink(Scene& scene) : scene_(scene) {}
    void OnFrame(const webrtc::VideoFrame& frame) override {
        const auto pixels = frame.video_frame_buffer()->ToI420();
        if (pixels) Frame(frame.width(), frame.height(), pixels->DataY(), pixels->StrideY());
    }
    void Legacy(SessionEvent::VideoFrame frame) {
        const auto pixels = frame.pixels();
        if (pixels.size() >= size_t(frame.width) * frame.height) Frame(frame.width, frame.height, pixels.data(), frame.width);
    }
    void Begin() { std::lock_guard lock(mutex_); measuring_ = true; frames_ = invalid_ = 0; ages_.clear(); ids_.clear(); squared_ = 0; qualitySamples_ = 0; }
    QJsonObject End(double seconds) {
        std::lock_guard lock(mutex_); measuring_ = false;
        std::sort(ages_.begin(), ages_.end());
        auto quantile = [&](double q) -> QJsonValue { return ages_.empty() ? QJsonValue(QJsonValue::Null) : QJsonValue(ages_[size_t(std::ceil(q * ages_.size())) - 1]); };
        const double mse = qualitySamples_ ? squared_ / qualitySamples_ : 0;
        return {{"frames", int(frames_)}, {"fps", frames_ / seconds}, {"uniqueMarkers", int(ages_.size())},
            {"uniqueMarkerFps", ages_.size() / seconds}, {"invalidMarkers", int(invalid_)},
            {"imageAgeP50Ms", quantile(.50)}, {"imageAgeP95Ms", quantile(.95)}, {"imageAgeP99Ms", quantile(.99)},
            {"lumaSamples", qint64(qualitySamples_)}, {"lumaMse", mse},
            {"sampledLumaPsnrDb", mse > 0 ? QJsonValue(10 * std::log10(255.0 * 255 / mse)) : QJsonValue(QJsonValue::Null)}};
    }
    std::atomic<unsigned> received{0};
private:
    void Frame(int width, int height, const uint8_t* y, int stride) {
        ++received;
        const double now = NowMs();
        std::lock_guard lock(mutex_); if (!measuring_) return;
        ++frames_;
        if (width != Width || height != Height) { ++invalid_; return; }
        unsigned id = 0;
        for (int bit = 0; bit < 16; ++bit) {
            int a = 0, b = 0;
            for (int dx = -2; dx <= 2; ++dx) { a += y[(Height * 172 / 360) * stride + Width * (50 + bit * 36) / 640 + dx]; b += y[(Height * 196 / 360) * stride + Width * (50 + bit * 36) / 640 + dx]; }
            if ((a > 640) == (b > 640)) { ++invalid_; return; }
            if (a > 640) id |= 1u << bit;
        }
        const double painted = scene_.Time(id);
        if (!painted || painted > now || now - painted > 10000) { ++invalid_; return; }
        if (!ids_.insert(id).second) return;
        ages_.push_back(now - painted);
        for (int row = 10; row < Height; row += 20) for (int col = 10; col < Width; col += 20) {
            if (row * 360 / Height >= 150 && row * 360 / Height < 220) continue;
            const double reference = 16 + 219.0 * scene_.Shade(id, col, row) / 255;
            const double difference = y[row * stride + col] - reference;
            squared_ += difference * difference; ++qualitySamples_;
        }
    }
    Scene& scene_; std::mutex mutex_; bool measuring_ = false; unsigned frames_ = 0, invalid_ = 0;
    std::vector<double> ages_; std::set<unsigned> ids_; double squared_ = 0; uint64_t qualitySamples_ = 0;
};
class DiscardAudio final : public PcmPlayoutEndpoint {
public: void Start() override {} void Write(const PcmBlock&, std::stop_token) override {}
    uint32_t DelayMs() const override { return 0; } uint32_t BufferFrames() const override { return 0; }
};
double CpuSeconds() {
    FILETIME created{}, ended{}, kernel{}, user{}; Require(GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user), "Process timing failed");
    auto value = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return double(value(kernel) + value(user)) / 1e7;
}
QJsonObject Resources() {
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory); DWORD handles{};
    Require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) && GetProcessHandleCount(GetCurrentProcess(), &handles), "Resource sampling failed");
    return {{"privateBytes", qint64(memory.PrivateUsage)}, {"workingSetBytes", qint64(memory.WorkingSetSize)}, {"handles", int(handles)}};
}
bool Listening(unsigned port) {
    ULONG bytes = 0;
    GetExtendedUdpTable(nullptr, &bytes, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    if (!bytes || bytes > 1024 * 1024) return false;
    std::vector<uint8_t> storage(bytes);
    if (GetExtendedUdpTable(storage.data(), &bytes, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) != NO_ERROR) return false;
    const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(storage.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i)
        if (table->table[i].dwOwningPid == GetCurrentProcessId() && ntohs(USHORT(table->table[i].dwLocalPort)) == port) return true;
    return false;
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QJsonObject result{{"schema", 1}, {"passed", false}, {"externalLatencyVerified", false}, {"physicalInput", false}, {"audibleOutput", false}};
    if (argc != 8) { std::cerr << "backend origin scene viewers seconds port output.json\n"; return 2; }
    const QString output = argv[7];
    try {
        const std::string backend = argv[1], mode = argv[3]; const int viewers = std::stoi(argv[4]), seconds = std::stoi(argv[5]), port = std::stoi(argv[6]);
        Require((backend == "legacy" || backend == "v2") && (mode == "static" || mode == "scroll" || mode == "motion") &&
            (viewers == 1 || viewers == 4) && seconds >= 5 && seconds <= 120 && port >= 1024 && port <= 65000, "Invalid comparison configuration");
        WindowsMediaRuntime runtime; Require(SUCCEEDED(runtime.result()), "Windows media startup failed");
        webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false); webrtc::InitializeLogging(std::move(logging));
        webrtc::WinsockInitializer winsock; Require(!winsock.error() && webrtc::InitializeSSL(), "WebRTC network startup failed");
        Scene scene(mode);
        std::vector<std::shared_ptr<Sink>> sinks; for (int i = 0; i < viewers; ++i) sinks.push_back(std::make_shared<Sink>(scene));
        MemorySessionRuntimeControl stop;
        std::vector<std::unique_ptr<RoomSession>> rooms;
        std::vector<std::future<int>> legacy;
        // Stop before futures/sessions are destroyed, including on a failed assertion.
        struct StopGuard { MemorySessionRuntimeControl& stop; ~StopGuard() { stop.RequestStop(); } } guard{stop};
        const auto startup = Clock::now();
        if (backend == "legacy") {
            for (int i = 0; i < viewers; ++i) legacy.push_back(std::async(std::launch::async, [&, i] {
                WatchSessionConfig config; config.connectionMode = WatchConnectionMode::DirectListen; config.listenPort = uint16_t(port + i);
                config.udpAccessCode = "comparison-generated-scene-only"; config.playAudio = false; config.emitVideoFrames = true;
                ScreenShareRunContext context{&stop, {}, [sink = sinks[i]](auto frame) { sink->Legacy(std::move(frame)); }};
                return RunWatchSession(config, context);
            }));
            Wait([&] { for (int i = 0; i < viewers; ++i) if (!Listening(port + i)) return false; return true; });
            legacy.push_back(std::async(std::launch::async, [&] {
                ShareSessionConfig config; config.connectionMode = ShareConnectionMode::DirectTargets;
                config.captureSourceType = SessionCaptureSourceType::Window; config.windowHandle = uint64_t(scene.window()); config.windowProcessId = GetCurrentProcessId();
                config.udpAccessCode = "comparison-generated-scene-only"; config.captureSystemAudio = false;
                config.stream.outputResolution = SessionResolution{Width, Height}; config.stream.fps = 60; config.stream.bitrateBps = 12000000;
                config.stream.adaptBitrate = config.stream.adaptResolution = false;
                for (int i = 0; i < viewers; ++i) config.targets.push_back("127.0.0.1:" + std::to_string(port + i));
                return RunShareSession(config, {&stop, {}, {}});
            }));
        } else {
            for (int i = -1; i < viewers; ++i) {
                WindowsRoomRuntimeOptions options;
                options.capture.sourceType = CaptureSourceType::Window; options.capture.windowHandle = uint64_t(scene.window());
                options.capture.targetWidth = Width; options.capture.targetHeight = Height; options.capture.targetFps = 60;
                options.audioEndpoints = PcmEndpointFactories{[] { return std::make_unique<SilentPcmCapture>(); }, [] { return std::make_unique<DiscardAudio>(); }};
                options.preferences.resolution = ResolutionMode::Fixed; options.preferences.width = Width; options.preferences.height = Height;
                options.preferences.fps = 60; options.preferences.fpsMode = SettingMode::Manual;
                options.preferences.bitrateMode = SettingMode::Manual; options.preferences.bitrateLimitBps = 12000000;
                if (i >= 0) options.frames = sinks[i];
                rooms.push_back(std::make_unique<RoomSession>(WindowsRoomRuntimeFactory(options)));
                RoomOptions room; room.origin = argv[2]; room.host = i < 0; room.publicRoom = false; room.viewerLimit = viewers;
                room.name = "Matched backend workload"; room.nickname = i < 0 ? "Host" : "Viewer";
                if (i >= 0) room.roomId = rooms.front()->Status().roomId;
                auto start = rooms.back()->Start(room); Require(Await(start).error == RoomError::None, "V2 admission failed");
            }
        }
        Wait([&] { return std::all_of(sinks.begin(), sinks.end(), [](auto& sink) { return sink->received >= 30; }); });
        result["startupSeconds"] = std::chrono::duration<double>(Clock::now() - startup).count();
        std::this_thread::sleep_for(5s);
        const double cpu = CpuSeconds(), sourceCpu = scene.CpuSeconds(); const auto began = Clock::now(); const unsigned sourceBefore = scene.count();
        for (auto& sink : sinks) sink->Begin();
        QJsonArray resourceSamples;
        for (int i = 0; i < seconds; ++i) { std::this_thread::sleep_until(began + std::chrono::seconds(i + 1)); resourceSamples.append(Resources()); }
        const double elapsed = std::chrono::duration<double>(Clock::now() - began).count();
        result["cpuCorePercent"] = 100 * (CpuSeconds() - cpu) / elapsed;
        result["sourceCpuCorePercent"] = 100 * (scene.CpuSeconds() - sourceCpu) / elapsed;
        result["mediaCpuCorePercent"] = result["cpuCorePercent"].toDouble() - result["sourceCpuCorePercent"].toDouble();
        QJsonArray received;
        for (auto& sink : sinks) {
            auto metrics = sink->End(elapsed); received.append(metrics);
            Require(metrics["uniqueMarkers"].toInt() >= seconds * 5 && metrics["invalidMarkers"].toInt() <= metrics["frames"].toInt() / 20, "Insufficient valid generated-scene delivery");
        }
        result["receivers"] = received; result["resourceSamples"] = resourceSamples;
        result["sourceUpdates"] = int(scene.count() - sourceBefore); result["measuredSeconds"] = elapsed;
        if (!rooms.empty()) {
            const auto state = rooms.front()->Status();
            result["hardwareEncodedFrames"] = qint64(state.stream.codec.hardwareFrames);
            result["softwareFallbacks"] = qint64(state.stream.codec.softwareFallbacks);
            QJsonArray peers; for (const auto& peer : state.stream.peers) peers.append(QJsonObject{{"width", peer.width}, {"height", peer.height}, {"allocatedVideoBps", peer.allocatedVideoBitrateBps},
                {"encoder", CodecImplementationName(peer.sender.encoder)},
                {"decoder", peer.receiver.observation ? CodecImplementationName(peer.receiver.observation->decoder) : "unknown"},
                {"videoPayloadBps", peer.sender.payloadBps ? QJsonValue(qint64(*peer.sender.payloadBps)) : QJsonValue(QJsonValue::Null)},
                {"transportSendBps", peer.transportSendBps ? QJsonValue(qint64(*peer.transportSendBps)) : QJsonValue(QJsonValue::Null)}});
            result["senders"] = peers;
        }
        const auto stopping = Clock::now(); stop.RequestStop();
        for (auto& process : legacy) Require(Await(process) == 0, "Legacy runtime failed");
        for (auto it = rooms.rbegin(); it != rooms.rend(); ++it) { auto stopped = (*it)->Stop(); Await(stopped); }
        rooms.clear();
        result["teardownSeconds"] = std::chrono::duration<double>(Clock::now() - stopping).count(); result["afterStopResources"] = Resources();
        result["backend"] = QString::fromStdString(backend); result["scene"] = QString::fromStdString(mode); result["viewers"] = viewers;
        result["settings"] = QJsonObject{{"width", Width}, {"height", Height}, {"fps", 60}, {"bitrateLimitBps", 12000000}, {"audio", "disabled/silent"}, {"encryption", true}, {"warmupSeconds", 5}};
        result["scope"] = "same-process loopback; generated WGC window to CPU image consumer; source workload included in CPU; no physical presentation/input or network impairment";
        result["gpuUtilization"] = QJsonValue(QJsonValue::Null); result["passed"] = true;
    } catch (const std::exception& error) { result["error"] = error.what(); }
    QSaveFile file(output); if (!file.open(QIODevice::WriteOnly)) return 2;
    file.write(QJsonDocument(result).toJson(QJsonDocument::Indented)); if (!file.commit()) return 2;
    return result["passed"].toBool() ? 0 : 1;
}
