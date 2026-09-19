#include "room/qt/RoomMediaSession.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <iostream>
#include <set>
using namespace screenshare::room::qt;
using namespace screenshare::media;
void Check(bool ok) { if (!ok) throw std::runtime_error("Room media session invariant failed"); }
RoomSocket::Event Snapshot(uint64_t generation, uint64_t revision, std::initializer_list<const char*> viewers) {
    QJsonArray members{QJsonObject{{"peerId", "host"}, {"role", "host"}, {"status", "connected"}}};
    for (auto id : viewers) members.append(QJsonObject{{"peerId", id}, {"role", "viewer"}, {"status", "connected"}});
    return {RoomSocket::EventKind::Snapshot, generation, RoomSocket::Error::None, {{"members", members}}, revision};
}
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        SignalingExecutor executor;
        std::exception_ptr failure;
        auto result = executor.Post([&] {
            try {
                std::set<std::string> peers;
                size_t adds = 0, removes = 0, receivedSignals = 0;
                bool canAdd = true;
                RoomMediaSession session(executor, true, "host",
                    [&](const auto& peer) { ++adds; return peers.insert(peer).second; },
                    [&](const auto& peer) { ++removes; peers.erase(peer); },
                    [&](const auto&, auto) { ++receivedSignals; return true; },
                    [&](const auto&) { return canAdd; });
                session.OnEvent(Snapshot(1, 1, {"a", "b"})); session.Advance();
                Check(session.activeCount() == 2 && adds == 2);
                session.OnEvent(Snapshot(1, 2, {"a", "b"})); session.Advance();
                Check(adds == 2 && removes == 0);
                // Invalid authenticated payload retires only its sender. Room
                // profile revisions cannot recreate that failed incarnation.
                session.OnEvent({RoomSocket::EventKind::Signal, 1, RoomSocket::Error::None, {{"fromPeerId", "a"}}});
                session.Advance();
                Check(session.activeCount() == 1 && session.failedCount() == 1 && removes == 1 && receivedSignals == 0);
                session.OnEvent(Snapshot(1, 3, {"a", "b"})); session.Advance();
                Check(adds == 2);
                // A rapid leave/rejoin waits for asynchronous capture retirement.
                canAdd = false;
                session.OnEvent(Snapshot(1, 4, {"b"}));
                session.OnEvent(Snapshot(1, 5, {"a", "b"})); session.Advance();
                Check(session.status().pendingPeers == 1 && session.failedCount() == 0 && adds == 2);
                for (int i = 0; i < 20; ++i) session.Advance();
                Check(adds == 2);
                canAdd = true; session.Advance();
                Check(adds == 3 && session.activeCount() == 2);
                session.OnEvent({RoomSocket::EventKind::Reconnecting, 2}); session.Advance();
                Check(session.activeCount() == 0 && session.status().state == RoomMediaSession::State::Suspended);
                session.OnEvent(Snapshot(1, 100, {"a"}));
                session.OnEvent(Snapshot(2, 100, {"a"})); session.Advance();
                Check(session.activeCount() == 0);
                session.OnEvent(Snapshot(3, 1, {"a"})); session.Advance();
                Check(session.activeCount() == 1 && adds == 4);
                session.OnEvent({RoomSocket::EventKind::Signal, 1, RoomSocket::Error::None, {{"fromPeerId", "a"}}});
                session.OnEvent({RoomSocket::EventKind::Signal, 3, RoomSocket::Error::None, {{"fromPeerId", "outsider"}}});
                session.Advance(); Check(session.activeCount() == 1);
                session.OnEvent({RoomSocket::EventKind::Closed, 3}); session.Advance();
                Check(session.status().state == RoomMediaSession::State::Stopped);
                session.Stop(); session.Stop();
                session.OnEvent(Snapshot(4, 1, {"a"})); session.Advance();
                Check(session.activeCount() == 0 && session.status().state == RoomMediaSession::State::Stopped);
                {
                    RoomMediaSession viewer(executor, false, "a", [](const auto&) { return true; }, [](const auto&) {}, [](const auto&, auto) { return true; });
                    viewer.OnEvent(Snapshot(1, 1, {"a", "b"})); viewer.Advance();
                    Check(viewer.activeCount() == 1);
                    for (int i = 0; i < 257; ++i) viewer.OnEvent(Snapshot(1, 2, {"a"}));
                    Check(viewer.status().state == RoomMediaSession::State::Failed && viewer.activeCount() == 0 && viewer.status().queuedBytes == 0);
                    viewer.OnEvent(Snapshot(2, 1, {"a"})); viewer.Advance();
                    Check(viewer.status().state == RoomMediaSession::State::Failed);
                }
                {
                    RoomMediaSession bounded(executor, true, "host", [](const auto&) { return true; }, [](const auto&) {}, [](const auto&, auto) { return true; });
                    auto event = Snapshot(1, 1, {"a"});
                    event.value["oversized"] = QString(512 * 1024, 'x');
                    bounded.OnEvent(event);
                    Check(bounded.status().state == RoomMediaSession::State::Failed && bounded.status().queuedEvents == 0);
                }
                {
                    bool ready = false;
                    unsigned creations = 0;
                    RoomMediaSession pending(executor, true, "host", [&](const auto&) { ++creations; return true; },
                        [](const auto&) {}, [](const auto&, auto) { return true; }, [&](const auto&) { return ready; });
                    pending.OnEvent(Snapshot(1, 1, {"a"})); pending.Advance();
                    Check(pending.status().pendingPeers == 1);
                    pending.OnEvent(Snapshot(1, 2, {})); ready = true; pending.Advance();
                    Check(creations == 0 && pending.status().pendingPeers == 0);
                }
                {
                    RoomMediaSession opening(executor, true, "host", [](const auto&) { return true; },
                        [](const auto&) {}, [](const auto&, auto) { return true; });
                    opening.OnEvent({RoomSocket::EventKind::Error, 0, RoomSocket::Error::Configuration});
                    opening.Advance();
                    Check(opening.status().state == RoomMediaSession::State::Failed);
                }
            } catch (...) { failure = std::current_exception(); }
        });
        Check(result.get().error == ExecutorError::None);
        executor.Stop();
        if (failure) std::rethrow_exception(failure);
        std::cout << "{\"passed\":true,\"room_media_session\":true}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
