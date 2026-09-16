#include "api/RoomSession.h"
#include "RoomMediaSession.h"
#include "RoomSessionCoordinator.h"
#include "RoomSignalCodec.h"
#include "rtc_base/thread.h"
#include "api/units/time_delta.h"
#include <mutex>
#include <deque>
#include <QJsonDocument>
#include <QJsonArray>
#include "room/protocol/RoomProtocol.h"

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
        bool sourceQueued = false;
        std::shared_ptr<std::promise<media::CaptureUpdateResult>> sourceReply;
        std::future<media::CaptureUpdateResult> sourceUpdate;
        bool audioQueued = false;
        std::shared_ptr<std::promise<media::AudioUpdateResult>> audioReply;
        std::future<media::AudioUpdateResult> audioUpdate;
        bool playbackQueued = false;
        std::shared_ptr<std::promise<media::AudioUpdateResult>> playbackReply;
        std::future<media::AudioUpdateResult> playbackUpdate;
        bool mutationQueued = false;
        uint64_t mutationSequence = 0;
        QString mutationId;
        std::shared_ptr<std::promise<RoomUpdateResult>> mutationReply;
        std::chrono::steady_clock::time_point mutationDeadline;
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
        void FinishMutation(RoomUpdateResult result) {
            if (!mutationReply) return;
            auto reply = std::move(mutationReply); mutationId.clear();
            { std::lock_guard lock(mutex); mutationQueued = false; }
            reply->set_value(result);
        }
        void BeginStop(RoomError error = RoomError::None) {
            if (stopping) return;
            stopping = true;
            if (playbackReply) {
                playbackReply->set_value({media::AudioUpdateError::Cancelled}); playbackReply.reset();
                std::lock_guard lock(mutex); playbackQueued = false;
            }
            if (audioReply) {
                audioReply->set_value({media::AudioUpdateError::Cancelled}); audioReply.reset();
                std::lock_guard lock(mutex); audioQueued = false;
            }
            if (sourceReply) {
                sourceReply->set_value({media::CaptureUpdateError::Cancelled}); sourceReply.reset();
                std::lock_guard lock(mutex); sourceQueued = false;
            }
            FinishMutation({RoomUpdateError::Unconfirmed});
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
                status.capture = runtime->CaptureSelection();
                status.audio = runtime->AudioSelection();
                status.playback = runtime->Playback();
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
            if (event.kind == RoomSocket::EventKind::Snapshot) {
                const auto payload = event.value;
                const auto policy = payload["policy"].toObject();
                std::lock_guard lock(mutex);
                status.revision = event.revision.value_or(0);
                status.policy = {policy["name"].toString().toStdString(), policy["visibility"] == "public", policy["viewerLimit"].toInt()};
                status.members.clear();
                for (const auto& item : payload["members"].toArray()) {
                    const auto member = item.toObject();
                    status.members.push_back({member["peerId"].toString().toStdString(), member["nickname"].toString().toStdString(), member["role"] == "host"});
                }
            }
            if (event.kind == RoomSocket::EventKind::Result && mutationReply && event.value["requestId"].toString() == mutationId) {
                const auto payload = event.value["payload"].toObject();
                RoomUpdateResult result;
                if (payload["status"] == "conflict") {
                    result.error = RoomUpdateError::Conflict;
                    result.currentRevision = uint64_t(payload["currentRevision"].toDouble());
                } else if (payload["status"] != "ok")
                    result.error = payload["code"] == "forbidden" ? RoomUpdateError::Forbidden : RoomUpdateError::Rejected;
                FinishMutation(result);
            }
            if (event.kind == RoomSocket::EventKind::Reconnecting || event.kind == RoomSocket::EventKind::Error || event.kind == RoomSocket::EventKind::Closed)
                FinishMutation({RoomUpdateError::Unconfirmed});
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
            if (playbackReply && playbackUpdate.valid() && playbackUpdate.wait_for(0ms) == std::future_status::ready) {
                auto result = playbackUpdate.get();
                { std::lock_guard lock(mutex); playbackQueued = false; status.playback = runtime->Playback(); }
                playbackReply->set_value(result); playbackReply.reset();
            }
            if (audioReply && audioUpdate.valid() && audioUpdate.wait_for(0ms) == std::future_status::ready) {
                auto result = audioUpdate.get();
                { std::lock_guard lock(mutex); audioQueued = false; status.audio = runtime->AudioSelection(); }
                audioReply->set_value(result); audioReply.reset();
            }
            if (sourceReply && sourceUpdate.valid() && sourceUpdate.wait_for(0ms) == std::future_status::ready) {
                auto result = sourceUpdate.get();
                { std::lock_guard lock(mutex); sourceQueued = false; status.capture = runtime->CaptureSelection(); }
                sourceReply->set_value(result); sourceReply.reset();
            }
            if (mutationReply && std::chrono::steady_clock::now() >= mutationDeadline) FinishMutation({RoomUpdateError::Unconfirmed});
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
            if (admission.valid() || opening.valid() || startReply || recoveryDeadline || mutationReply || sourceReply || audioReply || playbackReply) Schedule();
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
std::future<RoomUpdateResult> RoomSession::UpdateNickname(std::string nickname, uint64_t revision) {
    return SubmitUpdate(std::move(nickname), {}, revision);
}
std::future<media::CaptureUpdateResult> RoomSession::SwitchCaptureSource(media::CaptureSelection selection) {
    try { media::ValidateCaptureSelection(selection); }
    catch (...) { return media::CaptureUpdateReady(media::CaptureUpdateError::Invalid); }
    auto state = impl_->state;
    std::lock_guard lock(state->mutex);
    if (state->stopQueued || state->status.phase != RoomPhase::Active) return media::CaptureUpdateReady(media::CaptureUpdateError::Unavailable);
    if (state->sourceQueued) return media::CaptureUpdateReady(media::CaptureUpdateError::Busy);
    state->sourceQueued = true;
    auto reply = std::make_shared<std::promise<media::CaptureUpdateResult>>(); auto future = reply->get_future();
    impl_->executor.Post([state, reply, selection] {
        if (state->stopping || !state->runtime) {
            { std::lock_guard lock(state->mutex); state->sourceQueued = false; }
            reply->set_value({media::CaptureUpdateError::Unavailable}); return;
        }
        state->sourceReply = reply;
        try { state->sourceUpdate = state->runtime->SwitchCaptureSource(selection); }
        catch (...) { state->sourceUpdate = media::CaptureUpdateReady(media::CaptureUpdateError::Failed); }
        if (!state->sourceUpdate.valid()) state->sourceUpdate = media::CaptureUpdateReady(media::CaptureUpdateError::Failed);
        state->Schedule();
    });
    return future;
}
std::future<RoomUpdateResult> RoomSession::UpdateRoomPolicy(RoomPolicy policy, uint64_t revision) {
    return SubmitUpdate({}, std::move(policy), revision);
}
std::future<media::AudioUpdateResult> RoomSession::SwitchAudioSource(media::AudioSelection selection) {
    try { media::ValidateAudioSelection(selection); }
    catch (...) { return media::CaptureUpdateReady(media::AudioUpdateError::Invalid); }
    auto state = impl_->state;
    std::lock_guard lock(state->mutex);
    if (state->stopQueued || state->status.phase != RoomPhase::Active) return media::CaptureUpdateReady(media::AudioUpdateError::Unavailable);
    if (state->audioQueued) return media::CaptureUpdateReady(media::AudioUpdateError::Busy);
    state->audioQueued = true;
    auto reply = std::make_shared<std::promise<media::AudioUpdateResult>>(); auto future = reply->get_future();
    impl_->executor.Post([state, reply, selection = std::move(selection)] {
        if (state->stopping || !state->runtime) {
            { std::lock_guard lock(state->mutex); state->audioQueued = false; }
            reply->set_value({media::AudioUpdateError::Unavailable}); return;
        }
        state->audioReply = reply;
        try { state->audioUpdate = state->runtime->SwitchAudioSource(selection); }
        catch (...) { state->audioUpdate = media::CaptureUpdateReady(media::AudioUpdateError::Failed); }
        if (!state->audioUpdate.valid()) state->audioUpdate = media::CaptureUpdateReady(media::AudioUpdateError::Failed);
        state->Schedule();
    });
    return future;
}
std::future<media::AudioUpdateResult> RoomSession::UpdatePlayback(media::PlaybackSelection selection) {
    try { media::ValidatePlaybackSelection(selection); }
    catch (...) { return media::CaptureUpdateReady(media::AudioUpdateError::Invalid); }
    auto state = impl_->state;
    std::lock_guard lock(state->mutex);
    if (state->stopQueued || state->status.phase != RoomPhase::Active) return media::CaptureUpdateReady(media::AudioUpdateError::Unavailable);
    if (state->playbackQueued) return media::CaptureUpdateReady(media::AudioUpdateError::Busy);
    state->playbackQueued = true;
    auto reply = std::make_shared<std::promise<media::AudioUpdateResult>>(); auto future = reply->get_future();
    impl_->executor.Post([state, reply, selection = std::move(selection)] {
        if (state->stopping || !state->runtime) {
            { std::lock_guard lock(state->mutex); state->playbackQueued = false; }
            reply->set_value({media::AudioUpdateError::Unavailable}); return;
        }
        state->playbackReply = reply;
        try { state->playbackUpdate = state->runtime->UpdatePlayback(selection); }
        catch (...) { state->playbackUpdate = media::CaptureUpdateReady(media::AudioUpdateError::Failed); }
        if (!state->playbackUpdate.valid()) state->playbackUpdate = media::CaptureUpdateReady(media::AudioUpdateError::Failed);
        state->Schedule();
    });
    return future;
}
std::future<RoomUpdateResult> RoomSession::SubmitUpdate(std::optional<std::string> nickname, std::optional<RoomPolicy> policy, uint64_t revision) {
    auto reply = std::make_shared<std::promise<RoomUpdateResult>>(); auto future = reply->get_future();
    // Bound allocations and reject revisions before converting to JSON doubles.
    if (revision > 9007199254740991ULL || (nickname && nickname->size() > 1024) || (policy && policy->name.size() > 1024)) {
        reply->set_value({RoomUpdateError::Invalid}); return future;
    }
    if ((nickname && QString::fromStdString(*nickname).toStdString() != *nickname) ||
        (policy && QString::fromStdString(policy->name).toStdString() != policy->name)) {
        reply->set_value({RoomUpdateError::Invalid}); return future;
    }
    QJsonObject payload{{"expectedRevision", double(revision)}};
    if (nickname) payload["nickname"] = QString::fromStdString(*nickname);
    if (policy) {
        payload["name"] = QString::fromStdString(policy->name);
        payload["visibility"] = policy->publicRoom ? "public" : "unlisted";
        payload["viewerLimit"] = policy->viewerLimit;
    }
    auto state = impl_->state;
    std::lock_guard lock(state->mutex);
    if (state->stopQueued || state->status.phase != RoomPhase::Active) {
        reply->set_value({RoomUpdateError::Unavailable}); return future;
    }
    bool host = false;
    for (const auto& member : state->status.members) if (member.peerId == state->status.peerId) host = member.host;
    if (policy && !host) { reply->set_value({RoomUpdateError::Forbidden}); return future; }
    if (state->mutationQueued) { reply->set_value({RoomUpdateError::Busy}); return future; }
    const auto id = QString("mutation_%1").arg(++state->mutationSequence);
    const auto validation = room::wire::ValidateClientCommand(QJsonDocument(QJsonObject{
        {"v", 2}, {"type", nickname ? "profile.update" : "room.update"}, {"roomId", QString::fromStdString(state->status.roomId)},
        {"requestId", id}, {"payload", payload}}).toJson(QJsonDocument::Compact));
    if (!validation.ok) { reply->set_value({RoomUpdateError::Invalid}); return future; }
    state->mutationQueued = true;
    impl_->executor.Post([state, reply, id, bytes = QJsonDocument(validation.message).toJson(QJsonDocument::Compact)] {
        state->mutationReply = reply; state->mutationId = id;
        try {
            if (state->stopping || !state->coordinator->Send(1, bytes)) {
                state->FinishMutation({RoomUpdateError::Unavailable}); return;
            }
            state->mutationDeadline = std::chrono::steady_clock::now() + 10s;
            state->Schedule();
        } catch (...) { state->FinishMutation({RoomUpdateError::Unconfirmed}); }
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
