#include "app/AppSessionBackend.h"

#include "app/ScreenShareApp.h"
#include "core/SessionCommand.h"

#include <exception>
#include <string_view>
#include <utility>

namespace screenshare {
namespace {

std::vector<std::string> AddExecutableName(std::vector<std::string> arguments, std::string executablePath)
{
    arguments.insert(arguments.begin(), std::move(executablePath));
    return arguments;
}

RuntimeResolutionRequest BuildRuntimeResolutionRequest(const StreamSettings& settings)
{
    RuntimeResolutionRequest request;
    if (settings.outputResolution &&
        settings.outputResolution->width > 0 &&
        settings.outputResolution->height > 0) {
        request.mode = RuntimeResolutionMode::Fixed;
        request.width = settings.outputResolution->width;
        request.height = settings.outputResolution->height;
    } else if (!settings.adaptResolution) {
        request.mode = RuntimeResolutionMode::Native;
    }
    return request;
}

SessionEvent BuildErrorEvent(SessionRole role, SessionState state, std::string message)
{
    SessionEvent event;
    event.type = SessionEventType::Error;
    event.status.role = role;
    event.status.state = state;
    event.status.summary = message;
    event.message = std::move(message);
    return event;
}

} // namespace

AppSessionBackend::~AppSessionBackend()
{
    Shutdown();
}

void AppSessionBackend::StartShare(const ShareSessionConfig& config, ISessionObserver& observer)
{
    StartArguments(SessionRole::Share, BuildShareRoomArguments(config), observer);
}

void AppSessionBackend::StartWatch(const WatchSessionConfig& config, ISessionObserver& observer)
{
    StartArguments(SessionRole::Watch, BuildWatchRoomArguments(config), observer);
}

void AppSessionBackend::Stop()
{
    bool shouldNotify = false;
    {
        std::scoped_lock lock(mutex_);
        if (state_ == SessionState::Idle || IsTerminalSessionState(state_)) {
            return;
        }
        state_ = SessionState::Stopping;
        summary_ = "Stopping session";
        stopRequested_ = true;
        shouldNotify = true;
    }

    runtimeControl_.RequestStop();
    if (shouldNotify) {
        Notify(SessionEventType::StateChanged, SessionState::Stopping, "Stopping session");
    }
}

void AppSessionBackend::Shutdown()
{
    DetachObserver();
    Stop();
    JoinWorker();
}

void AppSessionBackend::ApplyStreamSettings(const StreamSettings& settings)
{
    runtimeControl_.RequestResolution(BuildRuntimeResolutionRequest(settings));

    SessionState currentState = SessionState::Idle;
    {
        std::scoped_lock lock(mutex_);
        currentState = state_;
    }
    Notify(SessionEventType::SettingsChanged, currentState, "Stream settings queued");
}

void AppSessionBackend::StartArguments(
    SessionRole role,
    std::vector<std::string> arguments,
    ISessionObserver& observer,
    std::string executablePath)
{
    JoinFinishedWorker();

    bool alreadyRunning = false;
    SessionState currentState = SessionState::Idle;
    {
        std::scoped_lock lock(mutex_);
        if (worker_.joinable() && !IsTerminalSessionState(state_)) {
            alreadyRunning = true;
            currentState = state_;
        } else {
            observer_ = &observer;
            role_ = role;
            state_ = SessionState::Starting;
            summary_ = "Starting session";
            stopRequested_ = false;
            runtimeControl_.Reset();
        }
    }

    if (alreadyRunning) {
        observer.OnSessionEvent(BuildErrorEvent(role, currentState, "A session is already running."));
        return;
    }

    Notify(SessionEventType::StateChanged, SessionState::Starting, "Starting session");

    worker_ = std::thread([this, arguments = AddExecutableName(std::move(arguments), std::move(executablePath))]() mutable {
        Notify(SessionEventType::StateChanged, SessionState::Connecting, "Connecting session");

        ScreenShareAppRunContext context;
        context.runtimeControl = &runtimeControl_;
        context.outputHandler = [this](std::string_view text) {
            Notify(SessionEventType::LogLine, SessionState::Idle, std::string(text));
        };

        int exitCode = 1;
        try {
            exitCode = RunScreenShareApp(arguments, context);
        } catch (const std::exception& error) {
            Notify(SessionEventType::Error, SessionState::Failed, error.what());
            return;
        } catch (...) {
            Notify(SessionEventType::Error, SessionState::Failed, "Session failed with an unknown error.");
            return;
        }

        bool stopRequested = false;
        {
            std::scoped_lock lock(mutex_);
            stopRequested = stopRequested_;
        }

        if (exitCode == 0 || stopRequested) {
            Notify(SessionEventType::StateChanged, SessionState::Stopped, "Session stopped");
        } else {
            Notify(
                SessionEventType::Error,
                SessionState::Failed,
                "Session failed with exit code " + std::to_string(exitCode));
        }
    });
}

void AppSessionBackend::DetachObserver()
{
    std::scoped_lock lock(mutex_);
    observer_ = nullptr;
}

void AppSessionBackend::JoinWorker()
{
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

void AppSessionBackend::JoinFinishedWorker()
{
    bool shouldJoin = false;
    {
        std::scoped_lock lock(mutex_);
        shouldJoin = worker_.joinable() && IsTerminalSessionState(state_);
    }
    if (shouldJoin) {
        JoinWorker();
    }
}

void AppSessionBackend::Notify(SessionEventType type, SessionState state, std::string message)
{
    ISessionObserver* observer = nullptr;
    SessionEvent event;
    {
        std::scoped_lock lock(mutex_);
        if (type == SessionEventType::StateChanged || type == SessionEventType::Error) {
            state_ = state;
            summary_ = message;
        }
        observer = observer_;
        event.type = type;
        event.status = BuildStatusLocked();
        event.message = std::move(message);
    }

    if (observer != nullptr) {
        observer->OnSessionEvent(event);
    }
}

SessionStatus AppSessionBackend::BuildStatusLocked() const
{
    SessionStatus status;
    status.role = role_;
    status.state = state_;
    status.summary = summary_;
    return status;
}

} // namespace screenshare
