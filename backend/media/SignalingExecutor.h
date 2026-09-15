#pragma once
#include <cstdint>
#include <functional>
#include <future>
#include <memory>

namespace screenshare::media {
enum class ExecutorError { None, Capacity, Closed, Cancelled, Invalid, TaskFailed };
struct ExecutorResult {
    std::uint64_t operation = 0;
    ExecutorError error = ExecutorError::None;
};

// One owned WebRTC event loop; no native headers cross this boundary.
// Tasks initiate asynchronous work and return. Never wait here for another
// queued command. Captured objects must support destruction on rejection at
// the caller; accepted closures are released on the signaling thread.
class SignalingExecutor final {
public:
    SignalingExecutor();
    ~SignalingExecutor();
    SignalingExecutor(const SignalingExecutor&) = delete;
    SignalingExecutor& operator=(const SignalingExecutor&) = delete;
    std::future<ExecutorResult> Post(std::function<void()> task);
    bool IsCurrent() const noexcept;
    void RequestStop(); // May be called inside a task; cancels pending commands.
    // Join from an external owner only. Close/destroy peers on this executor
    // before stopping it. Destruction from its own thread is not supported.
    void Stop();
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
