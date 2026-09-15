#include "media/SignalingExecutor.h"
#include "rtc_base/thread.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace screenshare::media;
void Check(bool condition) { if (!condition) throw std::runtime_error("Executor assertion failed"); }
int main() {
    try {
        SignalingExecutor executor;
        Check(!executor.IsCurrent());
        Check(executor.Post({}).get().error == ExecutorError::Invalid);
        std::promise<void> nativeCallback;
        Check(executor.Post([&] {
            Check(executor.IsCurrent());
            webrtc::Thread::Current()->PostTask([&] { nativeCallback.set_value(); });
        }).get().error == ExecutorError::None);
        nativeCallback.get_future().get(); // No caller message pumping.
        Check(executor.Post([] { throw std::runtime_error("private detail"); }).get().error == ExecutorError::TaskFailed);
        std::vector<int> order;
        std::vector<std::future<ExecutorResult>> ordered;
        for (int i = 0; i < 32; ++i) ordered.push_back(executor.Post([&, i] { order.push_back(i); }));
        std::uint64_t previous = 0;
        for (auto& future : ordered) {
            const auto result = future.get();
            Check(result.error == ExecutorError::None && result.operation > previous);
            previous = result.operation;
        }
        for (int i = 0; i < 32; ++i) Check(order[i] == i);

        std::promise<void> entered, release;
        auto ready = release.get_future();
        auto active = executor.Post([&] { entered.set_value(); ready.wait(); });
        entered.get_future().get();
        std::atomic<int> destroyed = 0, ran = 0;
        struct Affine {
            SignalingExecutor& executor;
            std::atomic<int>& destroyed;
            ~Affine() { if (!executor.IsCurrent()) std::terminate(); ++destroyed; }
        };
        std::vector<std::future<ExecutorResult>> pending;
        for (int i = 0; i < 64; ++i) {
            auto resource = std::shared_ptr<Affine>(new Affine{executor, destroyed});
            pending.push_back(executor.Post([&, resource] { ++ran; }));
        }
        Check(executor.Post([] {}).get().error == ExecutorError::Capacity);
        executor.RequestStop();
        Check(executor.Post([] {}).get().error == ExecutorError::Closed);
        release.set_value();
        Check(active.get().error == ExecutorError::None);
        for (auto& future : pending) Check(future.get().error == ExecutorError::Cancelled);
        executor.Stop();
        executor.Stop();
        Check(destroyed == 64 && ran == 0);
        SignalingExecutor selfStop;
        Check(selfStop.Post([&] {
            bool rejected = false;
            try { selfStop.Stop(); } catch (const std::logic_error&) { rejected = true; }
            Check(rejected);
            selfStop.RequestStop();
        }).get().error == ExecutorError::None);
        selfStop.Stop();
        std::cout << "{\"passed\":true,\"mode\":\"signaling-executor\",\"cancelled_commands\":64}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
