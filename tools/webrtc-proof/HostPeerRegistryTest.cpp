#include "media/HostPeerRegistry.h"
#include "media/capture/SyntheticCaptureSource.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace screenshare::media;
using namespace std::chrono_literals;
void Check(bool ok) { if (!ok) throw std::runtime_error("Peer owner invariant failed"); }
struct Evidence { int closed = 0, destroyed = 0, restarts = 0; bool reject = false; };
struct TestPeer final : IMediaPeer {
    Evidence& evidence;
    PeerConnectionLifecycle policy;
    TestPeer(Evidence& value, uint64_t generation) : evidence(value), policy(generation, {}) {}
    ~TestPeer() override { ++evidence.destroyed; }
    PeerConnectionLifecycle& lifecycle() noexcept override { return policy; }
    bool RequestIceRestart(uint64_t revision) override {
        Check(revision == uint64_t(++evidence.restarts));
        if (evidence.reject) throw std::runtime_error("Negotiation dispatch failed");
        return true;
    }
    void Close() noexcept override { ++evidence.closed; }
};
template<class Predicate> void Wait(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (!predicate()) {
        Check(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}
void CaptureCleanup() {
    Evidence failed, healthy;
    HostMediaSession capture;
    const auto session = capture.Start([] { return std::make_unique<SyntheticCaptureSource>(64, 36, 120); }).get().generation;
    std::promise<void> release;
    const auto released = release.get_future().share();
    std::atomic<bool> entered{false};
    auto frames = std::make_shared<std::atomic<int>>(0);
    HostPeerRegistry peers(capture, session);
    struct Unblock { std::promise<void>& release; ~Unblock() { try { release.set_value(); } catch (...) {} } } unblock{release};
    auto one = std::make_unique<TestPeer>(failed, 1);
    auto two = std::make_unique<TestPeer>(healthy, 2);
    auto* failingPolicy = &one->policy;
    one->policy.Connected(1, {}); two->policy.Connected(2, {});
    Check(peers.Add(1, std::move(one)) && peers.Add(2, std::move(two)));
    Check(capture.AddViewer(session, 1, 1, [&](auto) { entered = true; released.wait(); }).get().error == HostOperationError::None);
    Check(capture.AddViewer(session, 2, 2, [frames](auto) { ++*frames; }).get().error == HostOperationError::None);
    Wait([&] { return entered.load() && *frames >= 2; });
    failingPolicy->RemoteClosed(1);
    peers.Tick({});
    Check(failed.closed == 1 && peers.snapshot(1)->captureCleanupPending);
    const auto before = frames->load();
    // Tick must remain callable while a delivery callback is blocked, and
    // another viewer must continue receiving its independent source handoff.
    Wait([&] { peers.Tick({}); return *frames >= before + 3; });
    Check(peers.snapshot(1)->captureCleanupPending && healthy.closed == 0);
    // Saturate the capture command queue behind the blocked removal. Rejected
    // cleanup must remain pending and retry after the queue drains.
    std::vector<std::future<HostOperationResult>> pressure;
    for (int i = 0; i < 80; ++i) pressure.push_back(capture.RemoveViewer(session, 999, 1));
    Check(peers.Remove(2, 2));
    peers.Tick({});
    Check(peers.snapshot(2)->captureCleanupPending &&
          peers.snapshot(2)->captureCleanupError == HostOperationError::Capacity);
    release.set_value();
    Wait([&] { peers.Tick({}); return !peers.snapshot(1)->captureCleanupPending && !peers.snapshot(2); });
    Check(capture.snapshot().viewerCount == 0 && failed.closed == 1);
    Check(peers.snapshot(1)->failure == PeerLifecycleFailure::RemoteClosed);
    Check(peers.Remove(1, 1) && !peers.snapshot(1));
    for (auto& command : pressure) {
        const auto error = command.get().error;
        Check(error == HostOperationError::None || error == HostOperationError::Capacity);
    }
    Check(capture.snapshot().viewerCount == 0 && healthy.closed == 1);
    peers.Stop(); peers.Stop();
    Check(capture.snapshot().state == HostMediaState::Stopped);
}
int main() try {
    const PeerConnectionLifecycle::Time zero{};
    Evidence first, second, replacement, rejected;
    {
        HostPeerRegistry peers;
        auto one = std::make_unique<TestPeer>(first, 1);
        auto two = std::make_unique<TestPeer>(second, 2);
        one->policy.Connected(1, zero); two->policy.Connected(2, zero);
        Check(peers.Add(1, std::move(one)) && peers.Add(2, std::move(two)));
        Check(!peers.RequestRestart(1, 9, zero));
        Check(peers.RequestRestart(1, 1, zero));
        peers.Tick(zero + 500ms); peers.Tick(zero + 501ms);
        Check(first.restarts == 1 && second.restarts == 0);
        Check(!peers.Remove(1, 9) && first.closed == 0);
        Check(peers.Remove(1, 1) && first.closed == 1 && first.destroyed == 1);
        auto next = std::make_unique<TestPeer>(replacement, 3);
        next->policy.Connected(3, zero); replacement.reject = true;
        Check(peers.Add(1, std::move(next)));
        Check(!peers.RequestRestart(1, 1, zero));
        peers.RequestRestart(1, 3, zero); peers.Tick(zero + 500ms); peers.Tick(zero + 1s);
        Check(replacement.closed == 1 && peers.snapshot(1)->restartDispatchFailed);
        Check(peers.snapshot(2)->state == PeerLifecycleState::Connected);
        Check(!peers.Add(3, std::make_unique<TestPeer>(rejected, 2)) && rejected.closed == 1 && rejected.destroyed == 1);
        peers.Stop(); peers.Stop();
        Check(second.closed == 1 && second.destroyed == 1 && replacement.closed == 1 && replacement.destroyed == 1);
    }
    Evidence timeout;
    { HostPeerRegistry peers;
      Check(peers.Add(1, std::make_unique<TestPeer>(timeout, 1)));
      peers.Tick(zero + 20s); peers.Tick(zero + 21s);
      Check(timeout.closed == 1 && peers.snapshot(1)->failure == PeerLifecycleFailure::DirectConnectTimeout);
    }
    Check(timeout.closed == 1 && timeout.destroyed == 1);
    CaptureCleanup();
    std::cout << "{\"passed\":true,\"mode\":\"host-peer-ownership\"}\n";
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
