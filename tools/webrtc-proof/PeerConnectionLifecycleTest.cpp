#include "media/PeerConnectionLifecycle.h"
#include <iostream>
#include <stdexcept>
using namespace screenshare::media;
using namespace std::chrono_literals;
void Check(bool ok) { if (!ok) throw std::runtime_error("Peer lifecycle invariant failed"); }
int main() try {
    const PeerConnectionLifecycle::Time zero{};
    PeerConnectionLifecycle timeout(1, zero);
    Check(timeout.Tick(zero + 19999ms) == PeerLifecycleAction::None);
    Check(timeout.Tick(zero + 20s) == PeerLifecycleAction::Close);
    Check(timeout.failure() == PeerLifecycleFailure::DirectConnectTimeout);
    Check(!timeout.Connected(1, zero + 21s));
    PeerConnectionLifecycle late(2, zero);
    Check(!late.Connected(2, zero + 20s));
    PeerConnectionLifecycle peer(3, zero), healthy(4, zero);
    Check(peer.Connected(3, zero) && healthy.Connected(4, zero));
    Check(!peer.Disconnected(2, zero));
    peer.Disconnected(3, zero);
    peer.Disconnected(3, zero + 400ms);
    Check(peer.Tick(zero + 499ms) == PeerLifecycleAction::None);
    Check(peer.Tick(zero + 500ms) == PeerLifecycleAction::RestartIce);
    Check(peer.Tick(zero + 501ms) == PeerLifecycleAction::None && peer.restartRevision() == 1);
    peer.Connected(3, zero + 600ms);
    peer.RequestRestart(3, zero + 1s);
    Check(peer.Tick(zero + 1999ms) == PeerLifecycleAction::None);
    Check(peer.Tick(zero + 2s) == PeerLifecycleAction::RestartIce);
    peer.Connected(3, zero + 3s); peer.Disconnected(3, zero + 3s);
    Check(peer.Tick(zero + 5s) == PeerLifecycleAction::RestartIce);
    peer.Connected(3, zero + 6s); peer.Disconnected(3, zero + 6s);
    Check(peer.Tick(zero + 6s) == PeerLifecycleAction::Close && peer.failure() == PeerLifecycleFailure::RestartLimit);
    Check(healthy.state() == PeerLifecycleState::Connected);
    PeerConnectionLifecycle recovery(5, zero);
    recovery.Connected(5, zero); recovery.Disconnected(5, zero);
    recovery.Tick(zero + 500ms);
    Check(recovery.Tick(zero + 20500ms) == PeerLifecycleAction::None && recovery.state() == PeerLifecycleState::Backoff);
    Check(recovery.Tick(zero + 21500ms) == PeerLifecycleAction::RestartIce);
    recovery.Connected(5, zero + 22s);
    recovery.Disconnected(5, zero + 82s);
    Check(recovery.Tick(zero + 82500ms) == PeerLifecycleAction::RestartIce);
    recovery.Close();
    Check(!recovery.Connected(5, zero + 83s) && recovery.Tick(zero + 200s) == PeerLifecycleAction::None);
    PeerConnectionLifecycle transient(6, zero);
    transient.Connected(6, zero); transient.Disconnected(6, zero);
    transient.Connected(6, zero + 100ms);
    Check(transient.Tick(zero + 500ms) == PeerLifecycleAction::None && transient.restartRevision() == 0);
    Check(!transient.RemoteClosed(5) && transient.RemoteClosed(6));
    Check(transient.Tick(zero) == PeerLifecycleAction::Close);
    std::cout << "{\"passed\":true,\"mode\":\"peer-connection-lifecycle\",\"deadline_seconds\":20,\"restart_limit_per_minute\":3}\n";
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
