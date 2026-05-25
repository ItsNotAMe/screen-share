#pragma once

#include "app/AppSessionBackend.h"

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <functional>

class QtSessionBackend final : public QObject, private screenshare::ISessionObserver {
public:
    struct StartRequest {
        screenshare::SessionRole role = screenshare::SessionRole::Share;
        QStringList arguments;
        QString executablePath;
    };

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

    bool start(const StartRequest& request, QString* errorMessage = nullptr);
    void stop();
    void applyStreamSettings(const screenshare::StreamSettings& settings);

private:
    void OnSessionEvent(const screenshare::SessionEvent& event) override;
    void handleSessionEvent(const screenshare::SessionEvent& event);
    void finish(bool failed);

    screenshare::AppSessionBackend backend_;
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
