#include "media/SignalingExecutor.h"
#include "rtc_base/thread.h"
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace screenshare::media {
struct SignalingExecutor::State {
    struct Command {
        std::uint64_t id;
        std::function<void()> task;
        std::promise<ExecutorResult> completion;
    };
    std::unique_ptr<webrtc::Thread> thread = webrtc::Thread::Create();
    std::mutex mutex, joinMutex;
    std::deque<Command> commands;
    std::uint64_t next = 0;
    bool scheduled = false, stopping = false, joined = false;
    std::promise<void> drained;
    std::shared_future<void> drainComplete = drained.get_future().share();

    void ScheduleLocked() {
        if (scheduled) return;
        scheduled = true;
        thread->PostTask([this] { Drain(); });
    }
    void Drain() {
        Command command;
        std::deque<Command> cancelled;
        bool finish = false;
        {
            std::lock_guard lock(mutex);
            if (stopping) {
                cancelled.swap(commands);
                finish = true;
            } else {
                command = std::move(commands.front());
                commands.pop_front();
            }
        }
        if (finish) {
            for (auto& item : cancelled) {
                item.task = {};
                item.completion.set_value({item.id, ExecutorError::Cancelled});
            }
            drained.set_value();
            return;
        }
        auto error = ExecutorError::None;
        try { command.task(); }
        catch (...) { error = ExecutorError::TaskFailed; }
        command.task = {};
        command.completion.set_value({command.id, error});
        std::lock_guard lock(mutex);
        scheduled = false;
        if (stopping || !commands.empty()) ScheduleLocked();
    }
};

SignalingExecutor::SignalingExecutor() : state_(std::make_unique<State>()) {
    state_->thread->SetName("screenshare-signaling", nullptr);
    if (!state_->thread->Start()) throw std::runtime_error("Signaling executor startup failed");
}
SignalingExecutor::~SignalingExecutor() { Stop(); }
bool SignalingExecutor::IsCurrent() const noexcept { return state_->thread->IsCurrent(); }
std::future<ExecutorResult> SignalingExecutor::Post(std::function<void()> task) {
    std::unique_lock lock(state_->mutex);
    State::Command command{++state_->next, std::move(task), {}};
    auto future = command.completion.get_future();
    auto error = ExecutorError::None;
    if (state_->stopping) error = ExecutorError::Closed;
    else if (!command.task) error = ExecutorError::Invalid;
    else if (state_->commands.size() >= 64) error = ExecutorError::Capacity;
    if (error != ExecutorError::None) command.completion.set_value({command.id, error});
    else {
        state_->commands.push_back(std::move(command));
        state_->ScheduleLocked();
    }
    lock.unlock(); // Rejected closure destruction may call back into the owner.
    return future;
}
void SignalingExecutor::RequestStop() {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping) return;
    state_->stopping = true;
    state_->ScheduleLocked();
}
void SignalingExecutor::Stop() {
    if (IsCurrent()) throw std::logic_error("Signaling executor cannot join itself");
    std::lock_guard join(state_->joinMutex);
    if (state_->joined) return;
    RequestStop();
    state_->drainComplete.wait();
    state_->thread->Stop();
    state_->joined = true;
}
}
