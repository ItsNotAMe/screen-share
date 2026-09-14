#include "media/webrtc/HardwareFrameWait.h"
#include <iostream>

namespace {
void Require(bool value) { if (!value) throw std::runtime_error("Hardware wait contract failed"); }
void Run() {
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
    std::cout << "Hardware wait: one submission, 500 ms missing-output deadline, cancellation and timestamp rejection passed.\n";
}
}
int main() {
    try { Run(); return 0; } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
