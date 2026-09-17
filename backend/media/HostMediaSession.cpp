#include "HostMediaSession.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace screenshare::media {
struct HostMediaSession::Impl {
    struct Command {
        uint64_t operation;
        std::function<HostOperationError()> run;
        std::promise<HostOperationResult> reply;
    };
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Command> queue;
    HostMediaSnapshot published;
    uint64_t nextOperation = 0;
    bool closing = false;
    // The following state is control-worker only.
    HostMediaSnapshot current;
    std::unique_ptr<CaptureDistributor> distribution;
    std::unique_ptr<CaptureSession> capture;
    std::map<uint64_t, uint64_t> viewers;
    uint64_t lastConnectionGeneration = 0;
    std::thread worker;

    Impl() : worker([this] { Run(); }) {}
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            closing = true;
        }
        wake.notify_one();
        worker.join();
    }
    void Publish() { std::lock_guard lock(mutex); published = current; }
    std::future<HostOperationResult> Submit(std::function<HostOperationError()> action,
                                           std::optional<uint64_t> stopGeneration = std::nullopt) {
        std::lock_guard lock(mutex);
        Command command{++nextOperation, std::move(action), {}};
        auto result = command.reply.get_future();
        if (stopGeneration && *stopGeneration != published.generation) {
            command.reply.set_value({command.operation, published.generation, HostOperationError::StaleGeneration});
            return result;
        }
        // Cancellation cannot be starved by a full settings/membership queue.
        // A stale stop must not cancel commands belonging to a newer session.
        if (stopGeneration && !closing) {
            for (auto& pending : queue)
                pending.reply.set_value({pending.operation, published.generation, HostOperationError::Cancelled});
            queue.clear();
        }
        if (closing || queue.size() >= 64) {
            command.reply.set_value({command.operation, published.generation,
                closing ? HostOperationError::Cancelled : HostOperationError::Capacity});
        } else {
            queue.push_back(std::move(command)); wake.notify_one();
        }
        return result;
    }
    void Teardown() {
        if (capture) capture->Stop();
        if (distribution) distribution->Stop();
        capture.reset(); distribution.reset(); viewers.clear();
        current.viewerCount = 0;
        current.viewers.clear();
    }
    void Refresh() {
        if (!capture) return;
        const auto status = capture->status();
        current.sourceGeneration = status.generation;
        current.captureFailure = status.failure;
        current.captureSource = status.source;
        switch (status.state) {
        case CaptureState::Starting: current.state = HostMediaState::Starting; break;
        case CaptureState::Recovering: current.state = HostMediaState::Recovering; break;
        case CaptureState::Running:
            current.state = viewers.empty() ? HostMediaState::WaitingForViewers : HostMediaState::Running; break;
        case CaptureState::Minimized: current.state = HostMediaState::Minimized; break;
        case CaptureState::Closed:
            Teardown(); current.state = HostMediaState::SourceClosed; break;
        case CaptureState::Stopped:
            Teardown(); current.state = HostMediaState::Stopped; break;
        case CaptureState::Failed:
            Teardown(); current.state = HostMediaState::Failed; break;
        }
        // A consumer failure affects its subscription only; remove it without
        // failing capture or the other viewers.
        if (distribution) {
            current.viewers.clear();
            for (auto it = viewers.begin(); it != viewers.end();) {
                const auto stats = distribution->stats(it->first);
                if (stats.failed) {
                    current.lastFailedViewer = it->first;
                    current.lastFailedConnectionGeneration = it->second;
                    distribution->Remove(it->first); it = viewers.erase(it);
                } else { current.viewers.push_back({it->first, stats, it->second}); ++it; }
            }
            current.viewerCount = viewers.size();
            if (current.state == HostMediaState::Running && viewers.empty()) current.state = HostMediaState::WaitingForViewers;
        }
        Publish();
    }
    void Run() noexcept {
        for (;;) {
            std::optional<Command> command;
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(10), [&] { return closing || !queue.empty(); });
                if (closing) {
                    for (auto& pending : queue)
                        pending.reply.set_value({pending.operation, current.generation, HostOperationError::Cancelled});
                    queue.clear();
                    break;
                }
                if (!queue.empty()) { command.emplace(std::move(queue.front())); queue.pop_front(); }
            }
            HostOperationError error = HostOperationError::None;
            current.activeOperation = command ? command->operation : 0;
            Publish();
            try {
                Refresh();
                if (command) error = command->run();
                Refresh();
            } catch (...) {
                Teardown(); current.state = HostMediaState::Failed;
                error = HostOperationError::Internal;
            }
            current.activeOperation = 0;
            Publish();
            if (command) command->reply.set_value({command->operation, current.generation, error});
        }
        current.state = HostMediaState::Stopping; Publish();
        Teardown(); current.state = HostMediaState::Stopped; Publish();
    }
};
HostMediaSession::HostMediaSession() : impl_(std::make_unique<Impl>()) {}
HostMediaSession::~HostMediaSession() = default;
std::future<HostOperationResult> HostMediaSession::Start(CaptureSession::Factory factory) {
    return impl_->Submit([owner = impl_.get(), factory = std::move(factory)]() mutable {
        if (!factory || owner->capture) return HostOperationError::InvalidState;
        const auto generation = owner->current.generation + 1;
        owner->current = {HostMediaState::Starting, generation};
        owner->lastConnectionGeneration = 0;
        owner->Publish();
        owner->distribution = std::make_unique<CaptureDistributor>(generation);
        auto* distribution = owner->distribution.get();
        owner->capture = std::make_unique<CaptureSession>(generation, std::move(factory),
            [distribution](auto sample) { distribution->Publish(std::move(sample)); });
        owner->capture->EnableDelivery();
        return HostOperationError::None;
    });
}
std::future<HostOperationResult> HostMediaSession::AddViewer(uint64_t generation, uint64_t viewer,
                                                         uint64_t connectionGeneration, CaptureSession::Deliver deliver) {
    return impl_->Submit([owner = impl_.get(), generation, viewer, connectionGeneration, deliver = std::move(deliver)]() mutable {
        if (generation != owner->current.generation) return HostOperationError::StaleGeneration;
        if (!owner->capture) return HostOperationError::InvalidState;
        if (!viewer || !deliver || owner->viewers.contains(viewer)) return HostOperationError::InvalidViewer;
        if (!connectionGeneration || connectionGeneration <= owner->lastConnectionGeneration)
            return HostOperationError::StaleGeneration;
        if (owner->viewers.size() >= 63) return HostOperationError::Capacity;
        owner->distribution->Add(viewer, std::move(deliver));
        owner->viewers.emplace(viewer, connectionGeneration);
        owner->lastConnectionGeneration = connectionGeneration;
        owner->current.viewerCount = owner->viewers.size();
        return HostOperationError::None;
    });
}
std::future<HostOperationResult> HostMediaSession::RemoveViewer(uint64_t generation, uint64_t viewer,
                                                            uint64_t connectionGeneration) {
    return impl_->Submit([owner = impl_.get(), generation, viewer, connectionGeneration] {
        if (generation != owner->current.generation) return HostOperationError::StaleGeneration;
        const auto existing = owner->viewers.find(viewer);
        if (existing != owner->viewers.end() && existing->second != connectionGeneration)
            return HostOperationError::StaleGeneration;
        if (owner->distribution) owner->distribution->Remove(viewer);
        owner->viewers.erase(viewer); owner->current.viewerCount = owner->viewers.size();
        return HostOperationError::None;
    });
}
std::future<HostOperationResult> HostMediaSession::Stop(uint64_t generation) {
    return impl_->Submit([owner = impl_.get(), generation] {
        if (generation != owner->current.generation) return HostOperationError::StaleGeneration;
        owner->current.state = HostMediaState::Stopping; owner->Publish();
        owner->Teardown(); owner->current.state = HostMediaState::Stopped;
        return HostOperationError::None;
    }, generation);
}
HostMediaSnapshot HostMediaSession::snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->published; }
}
