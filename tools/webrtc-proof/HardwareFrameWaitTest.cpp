#include "codec/HardwareFrameWait.h"
#include "media/audio/PcmBlockPacer.h"
#include <iostream>
#include <algorithm>
#include <thread>

namespace {
void Require(bool value) { if (!value) throw std::runtime_error("Hardware wait contract failed"); }
void Run() {
    using namespace screenshare;
    using namespace screenshare::media;
    unsigned submissions = 0, polls = 0;
    auto packet = WaitForHardwareFrame(99, [&] {
        ++polls;
        std::vector<screenshare::EncodedPacket> output;
        if (polls == 4) { output.emplace_back(); output.back().timestamp100ns = 99; }
        return output;
    }, [&] { ++submissions; return true; }, [] { return false; });
    Require(packet.timestamp100ns == 99 && submissions == 1);
    const auto start = std::chrono::steady_clock::now();
    bool timeout = false;
    submissions = 0;
    try {
        WaitForHardwareFrame(1, [] { return std::vector<screenshare::EncodedPacket>{}; },
            [&] { ++submissions; return true; }, [] { return false; });
    } catch (const HardwareFrameTimeout&) { timeout = true; }
    Require(timeout && submissions == 1 && std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    bool cancelled = false;
    try {
        WaitForHardwareFrame(1, [] { return std::vector<screenshare::EncodedPacket>{}; },
            [] { throw std::runtime_error("Submitted after cancellation"); return false; }, [] { return true; });
    } catch (const HardwareFrameCancelled&) { cancelled = true; }
    Require(cancelled);
    bool mismatch = false;
    polls = 0;
    try {
        WaitForHardwareFrame(1, [&] {
            std::vector<screenshare::EncodedPacket> output;
            if (++polls == 2) { output.emplace_back(); output.back().timestamp100ns = 2; }
            return output;
        }, [] { return true; }, [] { return false; });
    } catch (const std::runtime_error&) { mismatch = true; }
    Require(mismatch);
    // Reproduce Windows' background timer policy in this isolated test process.
    // A 1 ms media poll must not become the default ~15.6 ms sleep quantum.
    PROCESS_POWER_THROTTLING_STATE previous{};
    previous.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    Require(GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &previous, sizeof(previous)) != 0);
    struct Restore {
        PROCESS_POWER_THROTTLING_STATE state;
        ~Restore() { SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)); }
    } restore{previous};
    auto throttled = previous;
    throttled.ControlMask |= PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    throttled.StateMask |= PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    Require(SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttled, sizeof(throttled)) != 0);
    std::vector<double> delays;
    screenshare::ShortWait timer;
    for (int i = 0; i < 21; ++i) {
        const auto began = std::chrono::steady_clock::now(); timer.Wait();
        delays.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
    }
    std::sort(delays.begin(), delays.end());
    Require(delays[10] < 8);
    PcmBlockPacer pcm;
    pcm.Start();
    const auto audioStarted = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) Require(pcm.Wait({}));
    const auto audioMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - audioStarted).count();
    Require(audioMs >= 195 && audioMs < 280);
    std::stop_source stopped; stopped.request_stop();
    Require(!pcm.Wait(stopped.get_token()));
    DWORD before = 0, after = 0;
    Require(GetProcessHandleCount(GetCurrentProcess(), &before) != 0);
    for (int i = 0; i < 16; ++i) {
        std::thread worker([] { screenshare::ShortWait owned; owned.Wait(); });
        worker.join();
    }
    Require(GetProcessHandleCount(GetCurrentProcess(), &after) != 0 && after <= before + 1);
    std::cout << "Background poll median " << delays[10] << " ms; 20 PCM blocks " << audioMs
              << " ms; worker timer handles released.\n";
    std::cout << "Hardware wait: one submission, 500 ms missing-output deadline, cancellation and timestamp rejection passed.\n";
}
}
int main() {
    try { Run(); return 0; } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
