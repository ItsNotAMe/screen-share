#include "LiveCaptureSource.h"
#include "media/capture/CaptureRecovery.h"
#include "CaptureTestWindow.h"
#include <iostream>
#include <mutex>
#include <string_view>

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Predicate> void Wait(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!predicate()) {
        Require(std::chrono::steady_clock::now() < deadline, "Capture recovery timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
int main(int argc, char** argv) try {
    using namespace screenshare::media;
    CaptureRecovery policy;
    auto now = CaptureRecovery::Clock::now();
    int retired = 0, rebuilt = 0;
    for (int i = 0; i < 3; ++i) {
        policy.Lost([&] { ++retired; }, now);
        Require(!policy.Poll([&] { ++rebuilt; }, now), "Backoff skipped");
        now += std::chrono::milliseconds(250);
        Require(policy.Poll([&] { ++rebuilt; }, now), "Rebuild did not run");
        Require(policy.generation() == uint64_t(i + 2), "Generation did not advance");
    }
    bool exhausted = false;
    try { policy.Lost([&] { ++retired; }, now); } catch (const std::runtime_error&) { exhausted = true; }
    Require(exhausted && retired == 4 && rebuilt == 3, "Unbounded recovery or live terminal device");
    bool resumed = false;
    try { policy.Poll([] {}); resumed = true; } catch (const std::runtime_error&) {}
    Require(!resumed, "Terminal policy resumed");
    if (argc > 1 && std::string_view(argv[1]) == "--live") {
        proof::TestWindow window;
        std::mutex mutex;
        webrtc::scoped_refptr<D3dVideoFrameBuffer> latest;
        proof::LiveCaptureSource source(window.handle(), [&](auto sample) {
            auto resource = std::static_pointer_cast<WindowsCaptureResource>(sample.resource);
            std::lock_guard lock(mutex); latest = resource->buffer;
        });
        source.StartDelivery();
        Wait([&] { return source.frames > 2; });
        for (int i = 0; i < 3; ++i) {
            webrtc::scoped_refptr<D3dVideoFrameBuffer> old;
            { std::lock_guard lock(mutex); old = latest; }
            Require(old && old->ToI420(), "Current generation pixels unavailable");
            source.InjectDeviceLoss();
            Wait([&] { return source.generation == uint64_t(i + 2) && !source.HasFailed(); });
            Require(old->retired() && !old->ToI420(), "Retired generation exposed cached pixels");
            Wait([&] { std::lock_guard lock(mutex); return latest && !latest->retired(); });
        }
        source.InjectDeviceLoss();
        Wait([&] { return source.HasFailed(); });
        source.Stop(); source.Stop();
        Require(source.generation == 4, "Terminal failure rebuilt again");
        // Stop during backoff must not wait for the 250 ms retry timer.
        proof::LiveCaptureSource stopping(window.handle(), [](auto) {});
        stopping.InjectDeviceLoss();
        Wait([&] { return stopping.device()->retired(); });
        const auto before = std::chrono::steady_clock::now();
        stopping.Stop();
        Require(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(200), "Stop waited for retry backoff");
    }
    std::cout << "Capture recovery policy/generation checks passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
