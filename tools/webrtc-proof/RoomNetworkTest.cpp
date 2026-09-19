#include "room/qt/RoomNetwork.h"
#include "media/RoomPeerRoster.h"
#include <QCoreApplication>
#include <QTcpServer>
#include <iostream>
#include <atomic>
#include <thread>
using namespace screenshare::room::qt;
void Check(bool value) { if (!value) throw std::runtime_error("Room network ownership invariant failed"); }
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        {
            using screenshare::media::RoomPeerRoster;
            unsigned added = 0, removed = 0;
            RoomPeerRoster roster([&](const auto& id) { ++added; return id != "failed"; }, [&](const auto&) { ++removed; });
            Check(roster.Apply(2, 1, {"a", "failed"}) == RoomPeerRoster::Result::Applied);
            Check(roster.activeCount() == 1 && roster.failedCount() == 1);
            Check(roster.Apply(2, 2, {"a", "failed"}) == RoomPeerRoster::Result::Applied && added == 2);
            Check(roster.Apply(2, 3, {"a", "a"}) == RoomPeerRoster::Result::Invalid && removed == 0);
            Check(roster.Apply(1, 99, {}) == RoomPeerRoster::Result::Ignored);
            Check(roster.Apply(2, 3, {"b"}) == RoomPeerRoster::Result::Applied && added == 3 && removed == 1);
            roster.TransportLost(3);
            Check(roster.activeCount() == 0 && removed == 2);
            Check(roster.Apply(3, 4, {"b"}) == RoomPeerRoster::Result::Ignored);
            Check(roster.Apply(4, 0, {"b"}) == RoomPeerRoster::Result::Applied && added == 4);
            roster.TransportLost(2); Check(roster.activeCount() == 1);
        }
        // Intentionally never call exec/processEvents on this caller thread.
        for (int cycle = 0; cycle < 20; ++cycle) {
            RoomNetwork network;
            Check(network.Admit({}).get().error == RoomAdmission::Error::InvalidRequest);
            Check(network.Send(1, QByteArray(512 * 1024, 'x')).get() == RoomSocket::SendResult::Backpressure);
            Check(network.Send(1, "{}").get() == RoomSocket::SendResult::NotReady);
            std::vector<std::shared_future<void>> stops;
            for (int i = 0; i < 1000; ++i) stops.push_back(network.Stop(1));
            for (auto& stopped : stops) stopped.get();
        }
        {
            RoomNetwork network;
            // A stalled consumer cannot silently lose signaling; overflow is
            // terminal, bounded and observable, even after draining the marker.
            for (int i = 0; i < 300; ++i) Check(!network.Open(i + 1, {}).get());
            auto events = network.Drain();
            Check(events.size() == 1 && events[0].socket == 0 && events[0].value.error == RoomSocket::Error::Backpressure);
            Check(network.Admit({}).get().error == RoomAdmission::Error::Closed);
            Check(network.Drain().empty());
        }
        QTcpServer silentServer;
        {
            RoomNetwork network;
            std::atomic<bool> valid{true};
            std::vector<std::thread> producers;
            for (int i = 0; i < 4; ++i) producers.emplace_back([&] {
                try {
                    for (int iteration = 0; iteration < 250; ++iteration) {
                        auto sent = network.Send(1, "{}");
                        auto stopped = network.StopAll();
                        const auto result = sent.get(); stopped.get();
                        if (result != RoomSocket::SendResult::NotReady && result != RoomSocket::SendResult::Backpressure) valid = false;
                    }
                } catch (...) { valid = false; }
            });
            for (auto& producer : producers) producer.join();
            Check(valid);
        }
        Check(silentServer.listen(QHostAddress::LocalHost, 0));
        std::future<RoomAdmission::Result> pending;
        {
            RoomNetwork network(true);
            RoomAdmission::Request request;
            request.origin = QUrl("http://127.0.0.1:" + QString::number(silentServer.serverPort()));
            request.create = true; request.nickname = "Owner"; request.name = "Cancellation";
            pending = network.Admit(request);
            network.Send(999, "{}").get(); // Serialized command barrier, not an event pump.
            network.StopAll().get();
            Check(pending.get().error == RoomAdmission::Error::Cancelled);
            pending = network.Admit(request);
            network.Send(999, "{}").get(); // Destruction must resolve this second request too.
        }
        Check(pending.get().error == RoomAdmission::Error::Cancelled);
        std::cout << "Owned room networking, cancellation and bounded queues passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
