#pragma once

#include <QtCore/QObject>
#include <QtCore/QProcess>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <functional>
#include <utility>

class ProcessSessionBackend final : public QObject {
public:
    struct StartRequest {
        QString program;
        QStringList arguments;
        bool enableControlFile = false;
    };

    struct FinishInfo {
        int exitCode = 0;
        QProcess::ExitStatus exitStatus = QProcess::NormalExit;
        QString remainingOutput;
        bool stopRequested = false;
        bool forcedStop = false;
    };

    explicit ProcessSessionBackend(QObject* parent = nullptr);

    bool isRunning() const;
    bool stopRequested() const;
    QString controlFilePath() const;

    void setOutputHandler(std::function<void(const QString&)> handler);
    void setMessageHandler(std::function<void(const QString&)> handler);
    void setStartedHandler(std::function<void()> handler);
    void setErrorHandler(std::function<void(const QString&)> handler);
    void setFinishedHandler(std::function<void(const FinishInfo&)> handler);

    bool start(const StartRequest& request, QString* errorMessage = nullptr);
    void stop();

private:
    void cleanupStopFile();
    void cleanupControlFile();

    QProcess process_;
    std::function<void(const QString&)> outputHandler_;
    std::function<void(const QString&)> messageHandler_;
    std::function<void()> startedHandler_;
    std::function<void(const QString&)> errorHandler_;
    std::function<void(const FinishInfo&)> finishedHandler_;
    bool stopRequested_ = false;
    bool forcedStop_ = false;
    quint64 runSerial_ = 0;
    QString stopFilePath_;
    QString controlFilePath_;
};
