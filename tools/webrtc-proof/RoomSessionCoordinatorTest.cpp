#include "room/qt/RoomSessionCoordinator.h"
#include <QCoreApplication>
#include <atomic>
#include <iostream>
#include <thread>
using namespace screenshare::room::qt;
using namespace screenshare::media;
using namespace std::chrono_literals;
void Check(bool ok) { if (!ok) throw std::runtime_error("Room coordinator invariant failed"); }
template<class Predicate> void Wait(Predicate predicate) {
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) { Check(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(1ms); }
}
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        RoomNetwork network;
        SignalingExecutor executor;
        std::unique_ptr<RoomSessionCoordinator> coordinator;
        auto run = [&](auto work) { Check(executor.Post(work).get().error == ExecutorError::None); };
        std::atomic<unsigned> advances{0}, errors{0}, transportErrors{0};
        std::atomic<bool> correctThread{true};
        bool replied = false; // Signaling executor only.
        std::future<bool> opened;
        try {
            run([&] {
                coordinator = std::make_unique<RoomSessionCoordinator>(executor, network, [&] { ++advances; });
                opened = coordinator->Open(1, {}, [&](const auto& event) {
                    if (!executor.IsCurrent()) correctThread = false;
                    if (event.kind == RoomSocket::EventKind::Error) ++errors;
                    if (event.error == RoomSocket::Error::Transport) {
                        ++transportErrors;
                        if (!replied) { replied = true; Check(coordinator->Send(1, "{}")); }
                    }
                });
            });
            Check(opened.wait_for(3s) == std::future_status::ready && !opened.get());
            Wait([&] { return errors > 0 && advances > 0; });
            auto previousErrors = errors.load();
            run([&] { Check(coordinator->Send(1, "{}")); });
            Wait([&] { return transportErrors >= 2; }); // Late failure and callback-queued response are delivered.
            std::shared_future<void> closed;
            run([&] {
                Check(!coordinator->Send(1, QByteArray(512 * 1024 + 1, 'x')));
                for (int i = 0; i < 128; ++i) Check(coordinator->Send(1, "{}"));
                Check(!coordinator->Send(1, "{}"));
                Check(coordinator->stats().peakSendOperations == 128);
                closed = coordinator->Close(1);
            });
            closed.get();
            previousErrors = errors.load();
            std::this_thread::sleep_for(30ms);
            Check(errors == previousErrors && correctThread);
            std::shared_future<void> stopped;
            run([&] {
                opened = coordinator->Open(2, {}, [](const auto&) {});
                stopped = coordinator->Stop(); // Cancels an unresolved open deterministically.
            });
            Check(!opened.get()); stopped.get();
            const auto before = advances.load();
            run([&] { coordinator.reset(); });
            std::this_thread::sleep_for(30ms); Check(advances == before);
            executor.Stop();
        } catch (...) {
            run([&] { coordinator.reset(); }); executor.Stop(); throw;
        }
        std::cout << "Autonomous room dispatch, pressure, late failure and cancellation passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
