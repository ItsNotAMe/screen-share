#include "media/capture/CaptureSession.h"
#include "media/capture/SyntheticCaptureSource.h"
#include "core/ShortWait.h"
#include <windows.h>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <stdexcept>

using namespace screenshare;
using namespace screenshare::media;
using namespace std::chrono_literals;
void Check(bool ok) { if (!ok) throw std::runtime_error("Capture demand benchmark invariant failed"); }
double CpuMs() {
    FILETIME created{}, exited{}, kernel{}, user{};
    Check(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user));
    auto ticks = [](FILETIME v) { return (uint64_t(v.dwHighDateTime) << 32) | v.dwLowDateTime; };
    return double(ticks(kernel) + ticks(user)) / 10000.;
}
int main() try {
    const auto demand = std::make_shared<std::atomic<bool>>(true);
    std::atomic<uint64_t> frames{0};
    CaptureSession capture(1, [demand] { return std::make_unique<SyntheticCaptureSource>(1920, 1080, 60, demand); },
        [&](CaptureSample sample) {
            const auto pixels = std::static_pointer_cast<SyntheticCaptureResource>(sample.resource);
            Check(pixels->width == 1920 && pixels->height == 1080 && pixels->luma.size() == 1920u * 1080
                && sample.session == 1 && sample.generation == 1 && pixels->luma.front() >= 50 && pixels->luma.back() < 150);
            ++frames;
        });
    capture.EnableDelivery();
    ShortWait wait;
    auto pump = [&](std::chrono::milliseconds duration) {
        const auto end = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < end) {
            Check(capture.status().state != CaptureState::Failed);
            wait.Wait(10ms);
        }
    };
    std::cout << std::fixed << std::setprecision(4);
    for (unsigned pair = 0; pair < 6; ++pair) {
        for (bool active : {pair % 2 == 0, pair % 2 != 0}) {
            demand->store(active, std::memory_order_relaxed); pump(500ms);
            const auto before = frames.load(); const auto cpu = CpuMs(); const auto began = std::chrono::steady_clock::now();
            pump(2000ms);
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
            const auto used = CpuMs() - cpu; const auto produced = frames.load() - before;
            Check(active ? produced >= 100 && produced <= 132 : produced >= 8 && produced <= 12);
            std::cout << "{\"pair\":" << pair << ",\"active\":" << (active ? "true" : "false")
                << ",\"cpuMs\":" << used << ",\"wallMs\":" << elapsed << ",\"samples\":" << produced << "}\n" << std::flush;
        }
    }
    capture.Stop(); Check(capture.status().state == CaptureState::Stopped);
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
