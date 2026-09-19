#include "media/capture/CaptureDistributor.h"
#include "media/capture/SyntheticCaptureSource.h"
#include <atomic>
#include <future>
#include <iostream>
#include <thread>

using namespace screenshare::media;
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class Predicate> void Wait(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!predicate()) {
        Require(std::chrono::steady_clock::now() < deadline, "Distributor scenario timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
int main() try {
    std::atomic<unsigned> fast{0}, slow{0};
    CaptureDistributor distributor(7);
    distributor.Add(1, [&](auto sample) {
        Require(sample.session == 7, "Wrong session delivered"); ++fast;
    });
    distributor.Add(2, [&](auto) { ++slow; std::this_thread::sleep_for(std::chrono::milliseconds(60)); });
    distributor.Add(3, [](auto) { throw std::runtime_error("Isolated viewer failure"); });
    distributor.Add(4, [](auto sample) { Require(sample.resource != nullptr, "Frame ownership lost"); });
    CaptureSession source(7, [] { return std::make_unique<SyntheticCaptureSource>(64, 36, 120); },
                          [&](auto sample) { distributor.Publish(std::move(sample)); });
    source.EnableDelivery();
    Wait([&] { return distributor.stats(1).delivered >= 100; });
    auto fastStats = distributor.stats(1), slowStats = distributor.stats(2);
    distributor.Remove(2); // Join a slow consumer while capture keeps publishing.
    const auto afterRemoval = fast.load();
    Wait([&] { return fast >= afterRemoval + 20; });
    source.Stop(); distributor.Stop(); distributor.Stop();
    Require(fastStats.delivered >= 100 && slowStats.replaced > 20 && slowStats.delivered < fastStats.delivered / 2,
            "Slow consumer delayed the healthy viewer or accumulated a queue");
    Require(distributor.stats(3).failed && !fastStats.failed && !slowStats.failed,
            "Consumer failure escaped its viewer");

    // Deterministic blocked consumer: only the newest generation may remain
    // pending, and obsolete session/device/sequence callbacks must be rejected.
    CaptureDistributor generations(8);
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<unsigned> entered{0}, replacement{0};
    std::atomic<uint64_t> lastSequence{0}, lastGeneration{0};
    // Unblock and join before callback state destruction on assertion failure.
    struct ReleaseOnExit {
        std::promise<void>& value;
        CaptureDistributor& owner;
        ~ReleaseOnExit() { try { value.set_value(); } catch (...) {} owner.Stop(); }
    } cleanup{release, generations};
    generations.Add(1, [&](auto sample) {
        if (++entered == 1) released.wait();
        lastSequence = sample.sequence; lastGeneration = sample.generation;
    });
    auto resource = std::make_shared<SyntheticCaptureResource>();
    auto publish = [&](uint64_t session, uint64_t generation, uint64_t sequence) {
        generations.Publish({resource, std::chrono::steady_clock::now(), session, generation, sequence});
    };
    publish(8, 1, 1); Wait([&] { return entered == 1; });
    publish(8, 1, 2); publish(8, 2, 3);
    publish(7, 2, 4); publish(8, 1, 5); publish(8, 2, 3);
    Require(generations.stats(1).rejected == 3 && generations.stats(1).replaced == 1,
            "Old session/generation/sequence accepted or pending queue grew");
    release.set_value();
    Wait([&] { return generations.stats(1).delivered == 2; });
    Require(lastSequence == 3 && lastGeneration == 2, "Obsolete pending frame delivered");
    generations.Remove(1); generations.Remove(1);
    generations.Add(1, [&](auto sample) { Require(sample.sequence == 4, "Old subscription frame revived"); ++replacement; });
    publish(8, 2, 4); Wait([&] { return replacement == 1; });
    generations.Stop();
    bool refused = false;
    try { generations.Add(2, [](auto) {}); } catch (const std::invalid_argument&) { refused = true; }
    Require(refused, "Stopped distributor accepted a viewer");
    std::cout << "{\"passed\":true,\"mode\":\"headless-capture-distribution\",\"viewers\":4,"
              << "\"fast_frames\":" << fastStats.delivered << ",\"slow_frames\":" << slowStats.delivered
              << ",\"slow_replacements\":" << slowStats.replaced
              << ",\"fast_max_handoff_ms\":" << std::chrono::duration<double, std::milli>(fastStats.maxHandoffAge).count()
              << ",\"stale_rejections\":3}\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
