#include "api/RoomSession.h"
#include "RoomMediaSession.h"
#include "RoomSessionCoordinator.h"
#include "RoomSignalCodec.h"
#include "rtc_base/thread.h"
#include "api/units/time_delta.h"
#include <mutex>
#include <deque>
#include <QJsonDocument>

namespace screenshare::v2 {
using namespace room::qt;
using namespace std::chrono_literals;
struct RoomSession::Impl {
    RoomNetwork network;
    media::SignalingExecutor executor;
    struct State : std::enable_shared_from_this<State> {
        Impl& owner;
        RoomRuntimeFactory factory;
        mutable std::mutex mutex;
        RoomStatus status;
        bool started = false, stopQueued = false, scheduled = false, stopping = false;
        bool settingsQueued = false;
        std::promise<void> stoppedPromise;
        std::shared_future<void> stopped = stoppedPromise.get_future().share();
        std::shared_ptr<std::promise<RoomResult>> startReply;
        std::future<RoomAdmission::Result> admission;
        std::future<bool> opening;
        std::shared_future<void> networkStopped, mediaStopped;
        std::unique_ptr<RoomSessionCoordinator> coordinator;
        std::unique_ptr<RoomMediaSession> session;
        std::unique_ptr<RoomRuntime> runtime;
        RoomSocket::Config membership;
        bool connected = false, terminal = false, admittedSnapshot = false, leaving = false, leaveDone = false;
        std::chrono::steady_clock::time_point leaveDeadline;
        std::optional<std::chrono::steady_clock::time_point> recoveryDeadline;
        std::deque<std::chrono::steady_clock::time_point> reconnects;
        RoomError terminalError = RoomError::Transport;
        State(Impl& o, RoomRuntimeFactory f) : owner(o), factory(std::move(f)) {}
        void Phase(RoomPhase value, RoomError error = RoomError::None) {
            std::lock_guard lock(mutex); status.phase = value; status.error = error;
        }
        void Reply(RoomError error, bool unconfirmed = false) {
            if (startReply) { startReply->set_value({error, unconfirmed}); startReply.reset(); }
        }
        void BeginStop(RoomError error = RoomError::None) {
            if (stopping) return;
            stopping = true;
            Phase(error == RoomError::None ? RoomPhase::Stopping : RoomPhase::Failed, error);
            Reply(error == RoomError::None ? RoomError::Cancelled : error, admission.valid());
            if (session) session->Stop();
            try { if (runtime) mediaStopped = runtime->BeginStop(); }
            catch (...) { Phase(RoomPhase::Failed, RoomError::Media); }
            if (error == RoomError::None && admittedSnapshot && !terminal) {
                leaving = coordinator->Send(1, QJsonDocument(QJsonObject{{"v", 2}, {"type", "peer.leave"},
                    {"roomId", membership.roomId}, {"requestId", "session_leave"}, {"payload", QJsonObject{}}}).toJson(QJsonDocument::Compact));
                leaveDeadline = std::chrono::steady_clock::now() + 1s;
            }
            if (!leaving) networkStopped = coordinator ? coordinator->Stop() : owner.network.StopAll();
            Schedule();
        }
        void Schedule() {
            if (scheduled) return;
            scheduled = true;
            webrtc::Thread::Current()->PostDelayedTask([weak = weak_from_this()] {
                if (auto state = weak.lock()) {
                    state->scheduled = false;
                    try { state->Tick(); }
                    catch (...) { state->BeginStop(RoomError::Media); }
                }
            }, webrtc::TimeDelta::Millis(5));
        }
        void Advance() {
            if (!runtime || stopping) return;
            try {
                runtime->Advance();
                for (const auto& peer : runtime->FailedPeers()) session->FailPeer(peer);
                session->Advance();
            } catch (...) { terminal = true; terminalError = RoomError::Media; Schedule(); return; }
            const auto current = session->status();
            const auto stream = runtime->StreamSettings();
            {
                std::lock_guard lock(mutex);
                status.activePeers = current.activePeers; status.failedPeers = current.failedPeers;
                status.pendingPeers = current.pendingPeers; status.generation = current.generation;
                status.stream = stream;
            }
            if (current.state == RoomMediaSession::State::Failed) { terminal = true; terminalError = RoomError::Media; Schedule(); }
        }
        void Event(const RoomSocket::Event& event) {
            if (stopping) {
                if (event.kind == RoomSocket::EventKind::Closed || event.kind == RoomSocket::EventKind::Error ||
                    (event.kind == RoomSocket::EventKind::Result && event.value["requestId"] == "session_leave")) leaveDone = true;
                return;
            }
            session->OnEvent(event);
            if (event.kind == RoomSocket::EventKind::Snapshot) { admittedSnapshot = true; connected = true; recoveryDeadline.reset(); Phase(RoomPhase::Active); if (startReply) Schedule(); }
            if (event.kind == RoomSocket::EventKind::Reconnecting) {
                const auto now = std::chrono::steady_clock::now();
                while (!reconnects.empty() && now - reconnects.front() >= 1min) reconnects.pop_front();
                reconnects.push_back(now);
                if (reconnects.size() > 3) terminal = true;
                Phase(RoomPhase::Reconnecting); Schedule();
            }
            if (event.kind == RoomSocket::EventKind::Error) {
                if (event.error == RoomSocket::Error::Transport || event.error == RoomSocket::Error::Timeout) {
                    if (!recoveryDeadline) recoveryDeadline = std::chrono::steady_clock::now() + 35s;
                    Phase(RoomPhase::Reconnecting);
                    Schedule();
                }
                else { terminal = true; Schedule(); }
            }
            if (event.kind == RoomSocket::EventKind::Closed) { terminal = true; terminalError = RoomError::None; Schedule(); }
        }
        void Tick() {
            if (stopping) {
                // Runtime keeps advancing retirement while transport is stopped.
                try { if (runtime) runtime->Advance(); } catch (...) { Phase(RoomPhase::Failed, RoomError::Media); }
                if (leaving && !networkStopped.valid()) {
                    if (!leaveDone && std::chrono::steady_clock::now() < leaveDeadline) { Schedule(); return; }
                    networkStopped = coordinator->Stop();
                }
                if (networkStopped.valid() && networkStopped.wait_for(0ms) != std::future_status::ready) { Schedule(); return; }
                if (mediaStopped.valid() && mediaStopped.wait_for(0ms) != std::future_status::ready) { Schedule(); return; }
                try {
                    if (networkStopped.valid()) networkStopped.get();
                    if (mediaStopped.valid()) mediaStopped.get();
                } catch (...) { Phase(RoomPhase::Failed, RoomError::Media); }
                session.reset(); runtime.reset(); coordinator.reset(); membership = {};
                { std::lock_guard lock(mutex); if (status.error == RoomError::None) status.phase = RoomPhase::Stopped;
                  status.activePeers = status.failedPeers = status.pendingPeers = 0;
                  status.stream.peers.clear(); }
                stoppedPromise.set_value(); return;
            }
            if (terminal) { BeginStop(terminalError); return; }
            if ((coordinator && coordinator->failed()) || (recoveryDeadline && std::chrono::steady_clock::now() >= *recoveryDeadline)) {
                BeginStop(RoomError::Transport); return;
            }
            if (admission.valid() && admission.wait_for(0ms) == std::future_status::ready) {
                auto result = admission.get();
                if (result.error != RoomAdmission::Error::None || !result.membership) {
                    Reply(RoomError::Admission, result.outcomeUnconfirmed); BeginStop(RoomError::Admission); return;
                }
                membership = std::move(*result.membership);
                RoomIdentity identity{membership.expectedRole == "host", membership.roomId.toStdString(), membership.selfPeerId.toStdString()};
                { std::lock_guard lock(mutex); status.roomId = identity.roomId; status.peerId = identity.peerId; }
                runtime = factory(identity, [weak = weak_from_this()](const auto& target, auto signal) {
                    auto state = weak.lock();
                    return state && !state->stopping && state->coordinator->Send(1,
                        EncodeRoomSignal(signal, state->membership.roomId, QString::fromStdString(target)));
                });
                if (!runtime) { BeginStop(RoomError::Media); return; }
                session = std::make_unique<RoomMediaSession>(owner.executor, identity.host, identity.peerId,
                    [this](const auto& peer) { return runtime->Add(peer); },
                    [this](const auto& peer) { runtime->Remove(peer); },
                    [this](const auto& peer, auto signal) { return runtime->Receive(peer, std::move(signal)); },
                    [this](const auto& peer) { return runtime->Ready(peer); });
                Phase(RoomPhase::Connecting);
                opening = coordinator->Open(1, membership, [this](const auto& event) { Event(event); });
            }
            if (opening.valid() && opening.wait_for(0ms) == std::future_status::ready && !opening.get()) {
                BeginStop(RoomError::Transport); return;
            }
            if (connected) { Reply(RoomError::None); connected = false; }
            if (admission.valid() || opening.valid() || startReply || recoveryDeadline) Schedule();
        }
    };
    std::shared_ptr<State> state;
    Impl(RoomRuntimeFactory factory, bool loopback) : network(loopback) {
        state = std::make_shared<State>(*this, std::move(factory));
        const auto result = executor.Post([this] {
            state->coordinator = std::make_unique<RoomSessionCoordinator>(executor, network, [weak = std::weak_ptr<State>(state)] {
                if (auto value = weak.lock()) value->Advance();
            });
        }).get();
        if (result.error != media::ExecutorError::None) throw std::runtime_error("Session initialization failed");
    }
};
RoomSession::RoomSession(RoomRuntimeFactory factory, bool loopback) {
    if (!factory) throw std::invalid_argument("Media runtime factory is required");
    impl_ = std::make_unique<Impl>(std::move(factory), loopback);
}
RoomSession::~RoomSession() {
    if (impl_->executor.IsCurrent()) std::terminate();
    Stop().wait(); impl_->executor.Stop(); impl_->state.reset();
}
std::future<RoomResult> RoomSession::Start(RoomOptions options) {
    auto reply = std::make_shared<std::promise<RoomResult>>(); auto future = reply->get_future();
    auto state = impl_->state;
    {
        std::lock_guard lock(state->mutex);
        if (state->started || state->stopQueued) { reply->set_value({RoomError::Busy}); return future; }
        state->started = true; state->status.phase = RoomPhase::Admitting;
        // Publication is ordered with concurrent Stop under the same mutex.
        impl_->executor.Post([state, reply, options = std::move(options)] {
            state->startReply = reply;
            try {
            RoomAdmission::Request request;
            request.origin = QUrl(QString::fromStdString(options.origin)); request.create = options.host;
            request.roomId = QString::fromStdString(options.roomId); request.nickname = QString::fromStdString(options.nickname);
            request.name = QString::fromStdString(options.name); request.password = QString::fromStdString(options.password);
            request.visibility = options.publicRoom ? "public" : "unlisted"; request.viewerLimit = options.viewerLimit;
            state->admission = state->owner.network.Admit(std::move(request)); state->Schedule();
            } catch (...) { state->BeginStop(RoomError::Admission); }
        });
    }
    return future;
}
std::future<StreamUpdateResult> RoomSession::UpdateStreamPreferences(media::StreamPreferences preferences) {
    auto reply = std::make_shared<std::promise<StreamUpdateResult>>(); auto future = reply->get_future();
    try { media::ValidateStreamPreferences(preferences); }
    catch (const std::invalid_argument&) { reply->set_value({StreamUpdateError::Invalid}); return future; }
    auto state = impl_->state;
    std::lock_guard lock(state->mutex);
    if (state->stopQueued || state->status.phase != RoomPhase::Active) {
        reply->set_value({StreamUpdateError::Unavailable}); return future;
    }
    if (state->settingsQueued) { reply->set_value({StreamUpdateError::Busy}); return future; }
    state->settingsQueued = true;
    // One bounded settings command plus Start/Stop cannot exhaust the executor.
    impl_->executor.Post([state, reply, preferences] {
        StreamUpdateResult result{StreamUpdateError::Unavailable};
        StreamStatus stream;
        try {
            if (!state->stopping && state->runtime) {
                result = state->runtime->UpdateStreamPreferences(preferences);
                stream = state->runtime->StreamSettings();
            }
        } catch (...) { result.error = StreamUpdateError::Rejected; }
        { std::lock_guard lock(state->mutex); state->settingsQueued = false;
          if (result.error == StreamUpdateError::None) state->status.stream = std::move(stream); }
        reply->set_value(result);
    });
    return future;
}
std::shared_future<void> RoomSession::Stop() {
    auto state = impl_->state;
    std::lock_guard lock(state->mutex);
    if (!state->stopQueued) {
        state->stopQueued = true;
        impl_->executor.Post([state] { state->BeginStop(); });
    }
    return state->stopped;
}
RoomStatus RoomSession::Status() const {
    std::lock_guard lock(impl_->state->mutex); return impl_->state->status;
}
}
