#include "runtime/AvSyncDiagnostics.h"
#include "runtime/DiagnosticParsing.h"

#include <chrono>
#include <cmath>
#include <iostream>

namespace {

bool Check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

screenshare::UdpCompletedFrame VideoFrame(
    uint64_t frameId,
    uint64_t timestamp100ns,
    uint64_t senderQpc100ns)
{
    screenshare::UdpCompletedFrame frame;
    frame.frameId = frameId;
    frame.timestamp100ns = timestamp100ns;
    frame.senderQpc100ns = senderQpc100ns;
    return frame;
}

screenshare::UdpCompletedAudioPacket AudioPacket(
    uint64_t packetId,
    uint64_t qpcPosition)
{
    screenshare::UdpCompletedAudioPacket packet;
    packet.packetId = packetId;
    packet.qpcPosition = qpcPosition;
    return packet;
}

} // namespace

int main()
{
    using namespace std::chrono_literals;
    using screenshare_runtime_internal::AvSyncDiagnostics;

    bool ok = true;
    AvSyncDiagnostics diagnostics;
    const AvSyncDiagnostics::TimePoint start{};

    diagnostics.ObserveVideoFrame(VideoFrame(10, 1'000'000, 5'000'000), start);
    diagnostics.ObserveAudioPacket(AudioPacket(20, 5'100'000), start);

    auto snapshot = diagnostics.snapshot(start + 500ms);
    ok &= Check(snapshot.ready, "fresh audio and video make A/V diagnostics ready");
    ok &= Check(snapshot.senderClockReady, "fresh sender clocks make sender diagnostics ready");
    ok &= Check(snapshot.videoFresh && snapshot.audioFresh, "freshness is reported for both media clocks");
    ok &= Check(std::abs(snapshot.initialAudioSenderOffsetMs - 10.0) < 0.001,
        "fractional A/V diagnostics preserve the sender offset");

    diagnostics.ObserveVideoFrame(VideoFrame(11, 1'160'000, 5'160'000), start + 16ms);
    diagnostics.ObserveAudioPacket(AudioPacket(21, 5'260'000), start + 16ms);
    snapshot = diagnostics.snapshot(start + 20ms);
    ok &= Check(std::abs(snapshot.audioAheadMs) < 0.001,
        "matching audio and video progress produces zero timeline drift");

    diagnostics.ObserveAudioPacket(AudioPacket(20, 5'100'000), start + 1s);
    snapshot = diagnostics.snapshot(start + 1s);
    ok &= Check(snapshot.latestAudioPacketId == 21,
        "an out-of-order audio packet cannot regress the diagnostic clock");
    ok &= Check(snapshot.ignoredAudioPackets == 1,
        "an out-of-order audio packet is counted as ignored");
    ok &= Check(snapshot.audioAgeMs >= 900,
        "an ignored packet cannot falsely refresh audio freshness");

    diagnostics.ObserveVideoFrame(
        VideoFrame(12, 17'160'000, 21'160'000),
        start + 1'600ms);
    snapshot = diagnostics.snapshot(start + 1'600ms);
    ok &= Check(snapshot.videoFresh, "continued video remains fresh");
    ok &= Check(!snapshot.audioFresh, "stopped audio becomes stale");
    ok &= Check(!snapshot.ready, "stale audio makes A/V diagnostics not ready");
    ok &= Check(snapshot.audioAgeMs == 1'584,
        "audio age is measured from the last accepted packet");

    diagnostics.ObserveAudioPacket(AudioPacket(22, 21'260'000), start + 1'610ms);
    snapshot = diagnostics.snapshot(start + 1'610ms);
    ok &= Check(snapshot.ready, "fresh audio restores A/V readiness");

    diagnostics.Clear();
    snapshot = diagnostics.snapshot(start + 2s);
    ok &= Check(!snapshot.hasVideo && !snapshot.hasAudio && !snapshot.ready,
        "clearing diagnostics removes all media state");

    const auto fractional =
        screenshare_runtime_internal::ParseDiagnosticDouble("-123456.75");
    ok &= Check(fractional && std::abs(*fractional + 123456.75) < 0.001,
        "fractional signed diagnostic fields parse without retaining an old integer value");
    ok &= Check(
        !screenshare_runtime_internal::ParseDiagnosticDouble("12.5ms"),
        "diagnostic doubles reject trailing text");
    ok &= Check(
        !screenshare_runtime_internal::ParseDiagnosticDouble("nan"),
        "diagnostic doubles reject non-finite values");

    if (!ok) {
        return 1;
    }
    std::cout << "A/V sync diagnostics tests passed\n";
    return 0;
}
