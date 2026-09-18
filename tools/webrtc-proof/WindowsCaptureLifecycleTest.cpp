#include "CaptureTestWindow.h"
#include "LifecycleDiagnostics.h"
#include "core/WindowsMediaRuntime.h"
#include "media/capture/WindowsCaptureSource.h"
#include <atomic>
#include <iostream>
#include <mutex>
#include <thread>
#include <rpc.h>

using namespace screenshare;
using namespace screenshare::media;
using namespace std::chrono_literals;
namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Evidence {
    std::atomic<int> living{0}, calls{0};
    std::atomic<bool> lose{false}, wrongThread{false}, invalid{false};
    std::mutex mutex;
    std::shared_ptr<CaptureResource> retained;
};
// Injection only requests the production recovery path; all capture, GPU
// ownership, metadata and teardown still use the Windows adapter and owner.
class Source final : public ICaptureSource {
    Evidence& evidence_;
    std::thread::id owner_ = std::this_thread::get_id();
    WindowsCaptureSource source_;
    void Check() const { if (owner_ != std::this_thread::get_id()) evidence_.wrongThread = true; }
public:
    Source(CaptureConfig config, std::shared_ptr<input::DesktopTargetState> target, Evidence& evidence)
        : evidence_(evidence), source_(config, std::move(target)) { ++evidence_.living; }
    ~Source() override { Check(); --evidence_.living; }
    void Start() override { Check(); source_.Start(); }
    std::optional<CaptureSample> Poll() override {
        Check(); if (evidence_.lose.exchange(false)) throw CaptureLost(); return source_.Poll();
    }
    bool Closed() const override { Check(); return source_.Closed(); }
    bool Minimized() const override { Check(); return source_.Minimized(); }
    CaptureSourceInfo Info() const override { Check(); return source_.Info(); }
    void Retire() noexcept override { Check(); source_.Retire(); }
    void Rebuild() override { Check(); source_.Rebuild(); }
};
void Cycle(uint64_t cycle) {
    proof::TestWindow window;
    Evidence evidence;
    auto target = std::make_shared<input::DesktopTargetState>();
    CaptureConfig config;
    config.sourceType = CaptureSourceType::Window;
    config.windowHandle = reinterpret_cast<uint64_t>(window.handle());
    config.targetWidth = 320; config.targetHeight = 180;
    CaptureSession session(cycle, [&] { return std::make_unique<Source>(config, target, evidence); },
        [&](CaptureSample sample) {
            const auto frame = std::dynamic_pointer_cast<WindowsCaptureResource>(sample.resource);
            if (sample.session != cycle || !sample.sequence || !frame || !frame->buffer ||
                frame->buffer->width() != 320 || frame->buffer->height() != 180)
                evidence.invalid = true;
            { std::lock_guard lock(evidence.mutex); evidence.retained = std::move(sample.resource); }
            ++evidence.calls;
        });
    auto wait = [&](auto predicate) {
        const auto end = std::chrono::steady_clock::now() + 8s;
        while (!predicate()) {
            Require(session.status().state != CaptureState::Failed, "Production capture owner failed");
            Require(std::chrono::steady_clock::now() < end, "Production capture lifecycle deadline exceeded");
            std::this_thread::sleep_for(2ms);
        }
    };
    session.EnableDelivery();
    wait([&] { return evidence.calls >= 2 && target->Read().generation; });
    const auto original = target->Read();
    const auto property = input::WindowIdentityProperty(original.target.source);
    window.Invoke([&] { ShowWindow(window.handle(), SW_MINIMIZE); });
    wait([&] { return session.status().state == CaptureState::Minimized; });
    Require(!target->Read().generation, "Minimized capture retained an input target");
    window.Invoke([&] { ShowWindow(window.handle(), SW_SHOWNOACTIVATE); });
    wait([&] { return target->Read().generation > original.generation; });
    auto previous = target->Read();
    window.Invoke([&] { SetWindowPos(window.handle(), nullptr, 0, 0, 800, 480, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER); });
    wait([&] { const auto current = target->Read(); return current.generation > previous.generation &&
        (current.target.width != previous.target.width || current.target.height != previous.target.height); });
    previous = target->Read();
    evidence.lose = true;
    wait([&] { return session.status().generation == 2 && target->Read().generation > previous.generation; });
    if (cycle % 2 == 0) {
        window.Close();
        wait([&] { return session.status().state == CaptureState::Closed; });
    }
    session.Stop(); session.Stop();
    Require(!evidence.living && !evidence.wrongThread && !evidence.invalid, "Capture ownership or frame contract failed");
    Require(!target->Read().generation, "Stopped capture retained input permission geometry");
    if (cycle % 2) Require(!GetPropW(window.handle(), property.c_str()), "Capture identity property survived source destruction");
    const auto calls = evidence.calls.load();
    std::weak_ptr<CaptureResource> last;
    {
        std::lock_guard lock(evidence.mutex);
        last = evidence.retained;
        const auto frame = std::dynamic_pointer_cast<WindowsCaptureResource>(evidence.retained);
        Require(frame && frame->buffer->ToI420(), "Owned frame did not survive normal source teardown");
        evidence.retained.reset();
    }
    Require(last.expired(), "Capture owner retained the last delivered resource after join");
    Require(evidence.calls == calls, "Callback survived capture owner join");
}
}
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(proof::ReportUnhandledException);
    try {
        int cycles = 3, idleSeconds = 0;
        bool cleanup = false, cyclesSet = false, idleSet = false;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--rpc-idle-cleanup") {
                Require(!cleanup, "Duplicate cleanup option"); cleanup = true; continue;
            }
            Require((option == "--cycles" || option == "--idle-seconds") && i + 1 < argc,
                "Usage: WindowsCaptureLifecycleTest [--cycles 1..1000] [--idle-seconds 1..600] [--rpc-idle-cleanup]");
            const std::string value = argv[++i];
            size_t used = 0; const int count = std::stoi(value, &used);
            Require(used == value.size() && count >= 1, "Invalid count");
            if (option == "--cycles") {
                Require(!cyclesSet && count <= 1000, "Invalid or duplicate cycle count");
                cyclesSet = true; cycles = count;
            } else {
                Require(!idleSet && count <= 600, "Invalid or duplicate idle duration");
                idleSet = true; idleSeconds = count;
            }
        }
        // Explicit diagnostic experiment, never enabled by the application or
        // ordinary acceptance runs. RPC cleanup is process-wide and irreversible.
        if (cleanup) Require(RpcMgmtEnableIdleCleanup() == RPC_S_OK, "RPC idle cleanup initialization failed");
        std::cerr << "CAPTURE_OPTIONS {\"rpcIdleCleanup\":" << (cleanup ? "true" : "false")
            << ",\"idleSeconds\":" << idleSeconds << "}\n";
        WindowsMediaRuntime runtime;
        Require(SUCCEEDED(runtime.result()), "MTA lifetime initialization failed");
        proof::LifecycleSample(0, 0);
        for (int cycle = 1; cycle <= cycles; ++cycle) {
            const auto start = std::chrono::steady_clock::now();
            Cycle(cycle);
            std::cerr << "Capture cycle " << cycle << " destroyed\n";
            proof::LifecycleSample(cycle, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        }
        // Observe delayed OS cleanup after all sources, capture threads and frames have
        // gone. These samples must never replace the immediate restart samples.
        if (idleSeconds) {
            const auto start = std::chrono::steady_clock::now();
            for (int second = 0; second <= idleSeconds; ++second) {
                std::this_thread::sleep_until(start + std::chrono::seconds(second));
                proof::LifecycleSample(second,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), "CAPTURE_IDLE");
            }
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
