#include "media/HostPeerRegistry.h"
#include <iostream>
#include <stdexcept>
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
    std::cout << "{\"passed\":true,\"mode\":\"host-peer-ownership\"}\n";
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
