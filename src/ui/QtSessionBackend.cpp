#include "ui/QtSessionBackend.h"

#include <QtCore/QMetaObject>

#include <exception>
#include <string>
#include <utility>

namespace {

QString ToQString(const std::string& text)
{
    return QString::fromStdString(text);
}

} // namespace

QtSessionBackend::QtSessionBackend(QObject* parent) : QObject(parent)
{
}

QtSessionBackend::~QtSessionBackend()
{
    backend_.Shutdown();
}

bool QtSessionBackend::isRunning() const
{
    return running_;
}

bool QtSessionBackend::stopRequested() const
{
    return stopRequested_;
}

void QtSessionBackend::setOutputHandler(std::function<void(const QString&)> handler)
{
    outputHandler_ = std::move(handler);
}

void QtSessionBackend::setMessageHandler(std::function<void(const QString&)> handler)
{
    messageHandler_ = std::move(handler);
}

void QtSessionBackend::setStartedHandler(std::function<void()> handler)
{
    startedHandler_ = std::move(handler);
}

void QtSessionBackend::setErrorHandler(std::function<void(const QString&)> handler)
{
    errorHandler_ = std::move(handler);
}

void QtSessionBackend::setFinishedHandler(std::function<void(const FinishInfo&)> handler)
{
    finishedHandler_ = std::move(handler);
}

void QtSessionBackend::setStatusHandler(std::function<void(const screenshare::SessionEvent&)> handler)
{
    statusHandler_ = std::move(handler);
}

bool QtSessionBackend::prepareStart(QString* errorMessage)
{
    if (running_) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("A session is already running.");
        }
        return false;
    }

    running_ = true;
    stopRequested_ = false;
    finished_ = false;
    return true;
}

bool QtSessionBackend::finishStartWithError(const std::exception& error, QString* errorMessage)
{
    running_ = false;
    if (errorMessage != nullptr) {
        *errorMessage = ToQString(error.what());
    }
    return false;
}

void QtSessionBackend::notifyStarted()
{
    if (startedHandler_) {
        startedHandler_();
    }
}

bool QtSessionBackend::startShare(
    const screenshare::ShareSessionConfig& config,
    const QString& executablePath,
    QString* errorMessage)
{
    if (!prepareStart(errorMessage)) {
        return false;
    }

    try {
        backend_.StartShare(
            config,
            *this,
            executablePath.isEmpty() ?
                std::string("ScreenShare") :
                executablePath.toStdString());
    } catch (const std::exception& error) {
        return finishStartWithError(error, errorMessage);
    }

    notifyStarted();
    return true;
}

bool QtSessionBackend::startWatch(
    const screenshare::WatchSessionConfig& config,
    const QString& executablePath,
    QString* errorMessage)
{
    if (!prepareStart(errorMessage)) {
        return false;
    }

    try {
        backend_.StartWatch(
            config,
            *this,
            executablePath.isEmpty() ?
                std::string("ScreenShare") :
                executablePath.toStdString());
    } catch (const std::exception& error) {
        return finishStartWithError(error, errorMessage);
    }

    notifyStarted();
    return true;
}

void QtSessionBackend::stop()
{
    if (!running_ || stopRequested_) {
        return;
    }

    stopRequested_ = true;
    backend_.Stop();
}

void QtSessionBackend::applyStreamSettings(const screenshare::StreamSettings& settings)
{
    if (running_) {
        backend_.ApplyStreamSettings(settings);
    }
}

void QtSessionBackend::OnSessionEvent(const screenshare::SessionEvent& event)
{
    QMetaObject::invokeMethod(
        this,
        [this, event] {
            handleSessionEvent(event);
        },
        Qt::QueuedConnection);
}

void QtSessionBackend::handleSessionEvent(const screenshare::SessionEvent& event)
{
    if (event.type == screenshare::SessionEventType::LogLine) {
        if (outputHandler_) {
            outputHandler_(ToQString(event.message));
        }
        return;
    }

    if (statusHandler_) {
        statusHandler_(event);
    }

    if (event.type == screenshare::SessionEventType::Error) {
        const QString message = ToQString(event.message);
        if (errorHandler_) {
            errorHandler_(message);
        }
        if (screenshare::IsTerminalSessionState(event.status.state)) {
            finish(true);
        }
        return;
    }

    if (event.type == screenshare::SessionEventType::StateChanged &&
        screenshare::IsTerminalSessionState(event.status.state)) {
        finish(false);
        return;
    }

    if (event.type == screenshare::SessionEventType::SettingsChanged && messageHandler_) {
        messageHandler_(ToQString(event.message) + QStringLiteral("\n"));
    }
}

void QtSessionBackend::finish(bool failed)
{
    if (finished_) {
        return;
    }

    FinishInfo info;
    info.exitCode = failed ? 1 : 0;
    info.stopRequested = stopRequested_;
    info.failed = failed;

    running_ = false;
    stopRequested_ = false;
    finished_ = true;

    if (finishedHandler_) {
        finishedHandler_(info);
    }
}
