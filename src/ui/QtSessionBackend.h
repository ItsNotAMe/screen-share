#pragma once

#include "api/ScreenShareAPI.h"

#include <QtCore/QObject>
#include <QtCore/QString>

#include <functional>
#include <vector>

class QtSessionBackend final : public QObject, private screenshare::ISessionEventSink {
public:
    struct FinishInfo {
        int exitCode = 0;
        QString remainingOutput;
        bool stopRequested = false;
        bool failed = false;
    };

    explicit QtSessionBackend(QObject* parent = nullptr);
    ~QtSessionBackend() override;

    bool isRunning() const;
    bool stopRequested() const;

    void setOutputHandler(std::function<void(const QString&)> handler);
    void setMessageHandler(std::function<void(const QString&)> handler);
    void setStartedHandler(std::function<void()> handler);
    void setErrorHandler(std::function<void(const QString&)> handler);
    void setFinishedHandler(std::function<void(const FinishInfo&)> handler);
    void setStatusHandler(std::function<void(const screenshare::SessionEvent&)> handler);

    bool startShare(
        const screenshare::ShareSessionConfig& config,
        const QString& executablePath,
        QString* errorMessage = nullptr);
    bool startWatch(
        const screenshare::WatchSessionConfig& config,
        const QString& executablePath,
        QString* errorMessage = nullptr);
    void stop();
    void applyStreamSettings(const screenshare::StreamSettings& settings);
    screenshare::SessionStatus currentStatus() const;
    std::vector<screenshare::SessionDisplayInfo> listDisplays(QString* errorMessage = nullptr);
    std::vector<screenshare::SessionAudioDeviceInfo> listAudioDevices(QString* errorMessage = nullptr);

private:
    bool prepareStart(QString* errorMessage);
    bool finishStartWithError(const std::exception& error, QString* errorMessage);
    void notifyStarted();
    void OnSessionEvent(const screenshare::SessionEvent& event) override;
    void handleSessionEvent(const screenshare::SessionEvent& event);
    void finish(bool failed);

    screenshare::ScreenShareSession session_;
    std::function<void(const QString&)> outputHandler_;
    std::function<void(const QString&)> messageHandler_;
    std::function<void()> startedHandler_;
    std::function<void(const QString&)> errorHandler_;
    std::function<void(const FinishInfo&)> finishedHandler_;
    std::function<void(const screenshare::SessionEvent&)> statusHandler_;
    bool running_ = false;
    bool stopRequested_ = false;
    bool finished_ = false;
};
