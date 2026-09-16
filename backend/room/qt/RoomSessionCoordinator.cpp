#include "RoomSessionCoordinator.h"
#include "rtc_base/thread.h"
#include "api/units/time_delta.h"
#include <algorithm>
#include <deque>
#include <map>
#include <stdexcept>

namespace screenshare::room::qt {
struct RoomSessionCoordinator::State : std::enable_shared_from_this<State> {
    struct Entry {
        Notify notify;
        uint64_t generation = 0;
        std::future<bool> opening;
        std::shared_ptr<std::promise<bool>> opened;
    };
    struct SendOperation { RoomNetwork::Socket socket; uint64_t generation; size_t bytes; std::future<RoomSocket::SendResult> future; };
    media::SignalingExecutor& executor;
    RoomNetwork& network;
    Advance advance;
    std::map<RoomNetwork::Socket, Entry> entries;
    std::deque<SendOperation> sends;
    size_t bytes = 0;
    Stats stats;
    bool stopped = false, failure = false;
    std::shared_future<void> stopping;
    State(media::SignalingExecutor& e, RoomNetwork& n, Advance a) : executor(e), network(n), advance(std::move(a)) {}
    void Check() const { if (!executor.IsCurrent()) throw std::logic_error("Room coordinator requires signaling executor"); }
    void Error(RoomNetwork::Socket socket, RoomSocket::Error error) {
        auto found = entries.find(socket);
        if (found != entries.end()) found->second.notify({RoomSocket::EventKind::Error, found->second.generation, error, {}});
    }
    void CancelEntry(Entry& entry) {
        if (entry.opened) { entry.opened->set_value(false); entry.opened.reset(); }
    }
    void Stop() {
        if (stopped) return;
        stopped = true;
        for (auto& [id, entry] : entries) CancelEntry(entry);
        entries.clear(); sends.clear(); bytes = 0; advance = {};
        stopping = network.StopAll();
    }
    void Fail(RoomSocket::Error reason) {
        failure = true;
        // Publish terminal failure before invalidating hooks, so the media
        // owner can initiate retirement on this same signaling executor.
        for (auto& [id, entry] : entries) {
            try { entry.notify({RoomSocket::EventKind::Error, entry.generation, reason, {}}); } catch (...) {}
        }
        try { if (advance) advance(); } catch (...) {}
        Stop();
    }
    void Tick() {
        for (auto& [id, entry] : entries) if (entry.opened && entry.opening.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const bool accepted = entry.opening.get();
            entry.opened->set_value(accepted); entry.opened.reset();
            if (!accepted) Error(id, RoomSocket::Error::Configuration);
        }
        for (auto& event : network.Drain()) {
            if (!event.socket) { Fail(RoomSocket::Error::Backpressure); return; }
            auto found = entries.find(event.socket);
            if (found == entries.end() || event.value.generation < found->second.generation) continue;
            found->second.generation = event.value.generation;
            found->second.notify(event.value);
        }
        std::vector<std::pair<RoomNetwork::Socket, RoomSocket::Error>> sendErrors;
        for (auto it = sends.begin(); it != sends.end();) {
            if (it->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
            const auto result = it->future.get();
            const auto socket = it->socket;
            const auto generation = it->generation;
            bytes -= it->bytes; it = sends.erase(it);
            const auto entry = entries.find(socket);
            // The reconnect event already retired the old generation. Its late
            // send failure must not tear down a replacement connection.
            if (result != RoomSocket::SendResult::Sent && entry != entries.end() &&
                (!generation || generation == entry->second.generation))
                sendErrors.emplace_back(socket, result == RoomSocket::SendResult::Backpressure ? RoomSocket::Error::Backpressure : RoomSocket::Error::Transport);
        }
        // Notify outside deque iteration: media hooks may enqueue a response.
        for (const auto& [socket, error] : sendErrors) Error(socket, error);
        if (advance) advance();
    }
    void Schedule() {
        webrtc::Thread::Current()->PostDelayedTask([weak = weak_from_this()] {
            if (auto state = weak.lock(); state && !state->stopped) {
                try { state->Tick(); } catch (...) { state->Fail(RoomSocket::Error::Protocol); }
                if (!state->stopped) state->Schedule();
            }
        }, webrtc::TimeDelta::Millis(5));
    }
};
RoomSessionCoordinator::RoomSessionCoordinator(media::SignalingExecutor& executor, RoomNetwork& network, Advance advance) {
    if (!executor.IsCurrent()) throw std::logic_error("Room coordinator requires signaling executor");
    state_ = std::make_shared<State>(executor, network, std::move(advance)); state_->Schedule();
}
RoomSessionCoordinator::~RoomSessionCoordinator() { state_->Check(); state_->Stop(); }
std::future<bool> RoomSessionCoordinator::Open(RoomNetwork::Socket id, RoomSocket::Config config, Notify notify) {
    state_->Check();
    auto promise = std::make_shared<std::promise<bool>>(); auto future = promise->get_future();
    if (state_->stopped || !id || !notify || state_->entries.contains(id) || state_->entries.size() >= 64) {
        promise->set_value(false); return future;
    }
    auto opening = state_->network.Open(id, std::move(config));
    state_->entries.emplace(id, State::Entry{std::move(notify), 0, std::move(opening), promise});
    return future;
}
bool RoomSessionCoordinator::Send(RoomNetwork::Socket id, QByteArray command) {
    state_->Check();
    const auto bytes = static_cast<size_t>(command.size());
    if (state_->stopped || state_->failure || !state_->entries.contains(id) || state_->sends.size() >= 128 || bytes > 512 * 1024 - state_->bytes) return false;
    auto sent = state_->network.Send(id, std::move(command));
    state_->bytes += bytes; state_->sends.push_back({id, state_->entries.at(id).generation, bytes, std::move(sent)});
    state_->stats.peakSendBytes = std::max(state_->stats.peakSendBytes, state_->bytes);
    state_->stats.peakSendOperations = std::max(state_->stats.peakSendOperations, state_->sends.size());
    return true;
}
std::shared_future<void> RoomSessionCoordinator::Close(RoomNetwork::Socket id) {
    state_->Check();
    auto found = state_->entries.find(id);
    if (found != state_->entries.end()) { state_->CancelEntry(found->second); state_->entries.erase(found); }
    return state_->network.Stop(id);
}
std::shared_future<void> RoomSessionCoordinator::Stop() { state_->Check(); state_->Stop(); return state_->stopping; }
bool RoomSessionCoordinator::failed() const { state_->Check(); return state_->failure; }
RoomSessionCoordinator::Stats RoomSessionCoordinator::stats() const { state_->Check(); return state_->stats; }
}
