#include "media/HostPeerOwner.h"
#include "media/capture/SyntheticCaptureSource.h"
#include <atomic>
#include <iostream>
#include <thread>
using namespace screenshare::media;
using namespace std::chrono_literals;
void Check(bool ok) { if (!ok) throw std::runtime_error("Scheduled peer owner invariant failed"); }
struct Evidence {
    std::atomic<int> closed = 0, destroyed = 0, restarts = 0, polls = 0;
    std::atomic<bool> fail = false;
};
struct TestPeer final : IMediaPeer {
    SignalingExecutor& executor;
    Evidence& evidence;
    PeerConnectionLifecycle policy;
    TestPeer(SignalingExecutor& executor, Evidence& evidence, uint64_t generation, bool expired = false)
        : executor(executor), evidence(evidence), policy(generation,
            PeerConnectionLifecycle::Clock::now() - (expired ? 21s : 0s)) {}
    ~TestPeer() override { if (!executor.IsCurrent()) std::terminate(); ++evidence.destroyed; }
    PeerConnectionLifecycle& lifecycle() noexcept override { return policy; }
    bool RequestIceRestart(uint64_t revision) override {
        Check(executor.IsCurrent() && revision == 1);
        ++evidence.restarts;
        return true;
    }
    bool Poll() override {
        Check(executor.IsCurrent());
        ++evidence.polls;
        if (evidence.fail) throw std::runtime_error("private operation error");
        return true;
    }
    void Close() noexcept override { if (!executor.IsCurrent()) std::terminate(); ++evidence.closed; }
};
template<class Predicate> void Wait(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (!predicate()) {
        Check(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(2ms);
    }
}
int main() {
    try {
        SignalingExecutor executor;
        HostMediaSession capture;
        std::unique_ptr<HostPeerOwner> owner;
        auto run = [&](std::function<void()> task) { Check(executor.Post(std::move(task)).get().error == ExecutorError::None); };
        Evidence failed, healthy, expired;
        std::atomic<int> frames = 0;
        try {
            auto started = capture.Start([] { return std::make_unique<SyntheticCaptureSource>(64, 36, 120); }).get();
            Check(started.error == HostOperationError::None);
            run([&] {
                owner = std::make_unique<HostPeerOwner>(executor, capture, started.generation);
                auto first = std::make_unique<TestPeer>(executor, failed, 1);
                first->policy.Connected(1, PeerConnectionLifecycle::Clock::now());
                Check(owner->Add(1, std::move(first)));
                auto second = std::make_unique<TestPeer>(executor, healthy, 2);
                second->policy.Connected(2, PeerConnectionLifecycle::Clock::now());
                Check(owner->Add(2, std::move(second)));
                Check(owner->Add(3, std::make_unique<TestPeer>(executor, expired, 3, true)));
            });
            Check(capture.AddViewer(started.generation, 1, 1, [](auto) {}).get().error == HostOperationError::None);
            Check(capture.AddViewer(started.generation, 2, 2, [&](auto) { ++frames; }).get().error == HostOperationError::None);
            Wait([&] { return expired.closed == 1 && frames > 5; });
            run([&] {
                Check(owner->snapshot(3)->failure == PeerLifecycleFailure::DirectConnectTimeout);
                Check(!owner->RequestRestart(2, 1));
                Check(owner->RequestRestart(2, 2));
            });
            // No explicit Tick or manual WebRTC message loop anywhere in this test.
            Wait([&] { return healthy.restarts == 1; });
            const auto before = frames.load();
            failed.fail = true;
            Wait([&] { return failed.closed == 1 && capture.snapshot().viewerCount == 1 && frames > before + 5; });
            run([&] {
                auto status = owner->snapshot(1);
                Check(status->peerClosed && status->operationFailed && status->state == PeerLifecycleState::Failed);
                Check(owner->Remove(1, 1));
                Check(owner->Remove(2, 2));
            });
            Wait([&] { return healthy.destroyed == 1; });
            run([&] { Check(!owner->snapshot(2)); owner->Stop(); owner->Stop(); owner.reset(); });
            Check(capture.snapshot().state == HostMediaState::Stopped);
            Check(failed.closed == 1 && failed.destroyed == 1 && expired.closed == 1 && expired.destroyed == 1);
            const auto polls = healthy.polls.load();
            // Restart owners while old weak timers are still queued.
            for (int i = 0; i < 25; ++i) {
                auto start = capture.Start([] { return std::make_unique<SyntheticCaptureSource>(64, 36, 30); }).get();
                Check(start.error == HostOperationError::None);
                run([&] {
                    owner = std::make_unique<HostPeerOwner>(executor, capture, start.generation);
                    owner.reset();
                });
            }
            Check(healthy.polls == polls);
        } catch (...) {
            run([&] { owner.reset(); });
            throw;
        }
        executor.Stop();
        std::cout << "{\"passed\":true,\"mode\":\"scheduled-peer-owner\",\"owner_restarts\":25}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
