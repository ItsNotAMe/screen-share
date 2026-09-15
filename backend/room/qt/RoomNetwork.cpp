#include "RoomNetwork.h"
#include <QJsonDocument>
#include <QThread>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace screenshare::room::qt {
struct RoomNetwork::Impl {
    QThread thread;
    QObject* worker = new QObject;
    std::unique_ptr<RoomAdmission> admission;
    std::unique_ptr<QTimer> admissionTimer;
    std::future<RoomAdmission::Result> pendingAdmission;
    std::shared_ptr<std::promise<RoomAdmission::Result>> admissionResult;
    std::map<Socket, std::unique_ptr<RoomSocket>> sockets;
    std::mutex mutex;
    std::vector<Event> events;
    size_t eventBytes = 0, commandBytes = 0, commands = 0;
    bool overflow = false;
    Socket lastSocket = 0;
    std::set<Socket> stops;
    bool stopAll = false;
    std::shared_ptr<std::promise<void>> stopPromise;
    std::shared_future<void> stopFuture;
    const bool loopback;
    explicit Impl(bool diagnostic) : loopback(diagnostic) {
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.start();
        QMetaObject::invokeMethod(worker, [this] {
            admission = std::make_unique<RoomAdmission>(loopback);
            admissionTimer = std::make_unique<QTimer>();
            QObject::connect(admissionTimer.get(), &QTimer::timeout, worker, [this] { CompleteAdmission(); });
        }, Qt::QueuedConnection);
    }
    ~Impl() {
        // This barrier runs after all accepted commands. Cancellation resolves
        // outstanding admission promises before the networking loop is joined.
        QMetaObject::invokeMethod(worker, [this] {
            admission->Cancel(); CompleteAdmission();
            admissionTimer.reset(); sockets.clear(); admission.reset();
        }, Qt::BlockingQueuedConnection);
        thread.quit(); thread.wait();
    }
    bool Enqueue(size_t bytes, std::function<void()> action) {
        std::lock_guard lock(mutex);
        // Do not enqueue a new open/admission between coalesced stop calls:
        // each stop completion must cover all earlier accepted commands.
        if (stopPromise || commands >= 128 || bytes > 512 * 1024 - commandBytes) return false;
        ++commands; commandBytes += bytes;
        // Keep publication ordered with Stop across concurrent producers.
        QMetaObject::invokeMethod(worker, [this, bytes, action = std::move(action)] {
            { std::lock_guard lock(mutex); --commands; commandBytes -= bytes; }
            action();
        }, Qt::QueuedConnection);
        return true;
    }
    void CompleteAdmission() {
        if (!admissionResult || pendingAdmission.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        admissionTimer->stop();
        admissionResult->set_value(pendingAdmission.get()); admissionResult.reset();
    }
    void Publish(Socket id, const RoomSocket::Event& event) {
        const auto bytes = static_cast<size_t>(QJsonDocument(event.value).toJson(QJsonDocument::Compact).size()) + 128;
        std::lock_guard lock(mutex);
        if (overflow) return;
        if (events.size() < 256 && bytes <= 512 * 1024 - eventBytes) {
            events.push_back({id, event}); eventBytes += bytes; return;
        }
        // Never drop signaling silently or reenter RoomSocket from its callback.
        // A single terminal marker replaces the bounded queue; retire all sockets.
        overflow = true; events.clear(); eventBytes = 0;
        events.push_back({0, {RoomSocket::EventKind::Error, 0, RoomSocket::Error::Backpressure, {}}});
        QMetaObject::invokeMethod(worker, [this] {
            // Destruction's queued barrier may already have retired admission.
            if (admission) { admission->Cancel(); CompleteAdmission(); }
            sockets.clear();
        }, Qt::QueuedConnection);
    }
};
RoomNetwork::RoomNetwork(bool loopback) : impl_(std::make_unique<Impl>(loopback)) {}
RoomNetwork::~RoomNetwork() = default;
std::future<RoomAdmission::Result> RoomNetwork::Admit(RoomAdmission::Request request) {
    auto promise = std::make_shared<std::promise<RoomAdmission::Result>>(); auto future = promise->get_future();
    const size_t bytes = (request.origin.toString().size() + request.roomId.size() + request.nickname.size() + request.name.size() + request.password.size() + request.visibility.size()) * sizeof(QChar) + 128;
    if (!impl_->Enqueue(bytes, [state = impl_.get(), promise, request = std::move(request)]() mutable {
        if (state->overflow) { promise->set_value({0, RoomAdmission::Error::Closed}); return; }
        if (state->admissionResult) { promise->set_value({0, RoomAdmission::Error::Busy}); return; }
        state->admissionResult = promise; state->pendingAdmission = state->admission->Start(std::move(request));
        state->admissionTimer->start(5); state->CompleteAdmission();
    })) promise->set_value({0, RoomAdmission::Error::Busy});
    return future;
}
std::future<bool> RoomNetwork::Open(Socket id, RoomSocket::Config config) {
    auto promise = std::make_shared<std::promise<bool>>(); auto future = promise->get_future();
    const size_t bytes = (config.origin.toString().size() + config.roomId.size() + config.selfPeerId.size() + config.expectedRole.size()) * sizeof(QChar) + config.token.size() + 128;
    if (!impl_->Enqueue(bytes, [state = impl_.get(), id, promise, config = std::move(config)]() mutable {
        if (id <= state->lastSocket || state->sockets.size() >= 64 || state->overflow) { promise->set_value(false); return; }
        state->lastSocket = id;
        auto socket = std::make_unique<RoomSocket>([state, id](const auto& e) { state->Publish(id, e); }, state->loopback);
        const bool started = socket->Start(std::move(config));
        if (started) state->sockets.emplace(id, std::move(socket));
        promise->set_value(started);
    })) promise->set_value(false);
    return future;
}
std::future<RoomSocket::SendResult> RoomNetwork::Send(Socket id, QByteArray command) {
    auto promise = std::make_shared<std::promise<RoomSocket::SendResult>>(); auto future = promise->get_future();
    const auto bytes = static_cast<size_t>(command.size()) + 128;
    if (!impl_->Enqueue(bytes, [state = impl_.get(), id, promise, command = std::move(command)] {
        const auto it = state->sockets.find(id);
        promise->set_value(it == state->sockets.end() ? RoomSocket::SendResult::NotReady : it->second->Send(command));
    })) promise->set_value(RoomSocket::SendResult::Backpressure);
    return future;
}
std::shared_future<void> RoomNetwork::Stop(Socket id) {
    std::lock_guard lock(impl_->mutex);
    if (!id) impl_->stopAll = true;
    else if (impl_->stops.size() < 64) impl_->stops.insert(id);
    else impl_->stopAll = true;
    if (!impl_->stopPromise) {
        impl_->stopPromise = std::make_shared<std::promise<void>>();
        impl_->stopFuture = impl_->stopPromise->get_future().share();
        QMetaObject::invokeMethod(impl_->worker, [state = impl_.get()] {
            std::set<Socket> ids; bool all; std::shared_ptr<std::promise<void>> completion;
            { std::lock_guard lock(state->mutex); ids.swap(state->stops); all = state->stopAll; state->stopAll = false; completion = std::move(state->stopPromise); }
            if (all) { state->admission->Cancel(); state->CompleteAdmission(); state->sockets.clear(); }
            else for (auto id : ids) state->sockets.erase(id);
            completion->set_value();
        }, Qt::QueuedConnection);
    }
    return impl_->stopFuture;
}
std::vector<RoomNetwork::Event> RoomNetwork::Drain() {
    std::lock_guard lock(impl_->mutex);
    std::vector<Event> result; result.swap(impl_->events); impl_->eventBytes = 0;
    return result;
}
}
