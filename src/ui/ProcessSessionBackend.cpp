#include "ui/ProcessSessionBackend.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>

ProcessSessionBackend::ProcessSessionBackend(QObject* parent) : QObject(parent)
{
    process_.setProcessChannelMode(QProcess::MergedChannels);

    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
        if (outputHandler_) {
            outputHandler_(QString::fromLocal8Bit(process_.readAllStandardOutput()));
        }
    });

    connect(&process_, &QProcess::started, this, [this] {
        if (startedHandler_) {
            startedHandler_();
        }
    });

    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        Q_UNUSED(error);
        cleanupStopFile();
        cleanupControlFile();
        if (errorHandler_) {
            errorHandler_(process_.errorString());
        }
    });

    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus status) {
        FinishInfo info;
        info.exitCode = code;
        info.exitStatus = status;
        info.remainingOutput = QString::fromLocal8Bit(process_.readAllStandardOutput());
        info.stopRequested = stopRequested_;
        info.forcedStop = forcedStop_;

        cleanupStopFile();
        cleanupControlFile();
        stopRequested_ = false;
        forcedStop_ = false;

        if (finishedHandler_) {
            finishedHandler_(info);
        }
    });
}

bool ProcessSessionBackend::isRunning() const
{
    return process_.state() != QProcess::NotRunning;
}

bool ProcessSessionBackend::stopRequested() const
{
    return stopRequested_;
}

QString ProcessSessionBackend::controlFilePath() const
{
    return controlFilePath_;
}

void ProcessSessionBackend::setOutputHandler(std::function<void(const QString&)> handler)
{
    outputHandler_ = std::move(handler);
}

void ProcessSessionBackend::setMessageHandler(std::function<void(const QString&)> handler)
{
    messageHandler_ = std::move(handler);
}

void ProcessSessionBackend::setStartedHandler(std::function<void()> handler)
{
    startedHandler_ = std::move(handler);
}

void ProcessSessionBackend::setErrorHandler(std::function<void(const QString&)> handler)
{
    errorHandler_ = std::move(handler);
}

void ProcessSessionBackend::setFinishedHandler(std::function<void(const FinishInfo&)> handler)
{
    finishedHandler_ = std::move(handler);
}

bool ProcessSessionBackend::start(const StartRequest& request, QString* errorMessage)
{
    if (isRunning()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("A session is already running.");
        }
        return false;
    }
    if (request.program.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("The engine path is empty.");
        }
        return false;
    }

    stopRequested_ = false;
    forcedStop_ = false;
    cleanupStopFile();
    cleanupControlFile();

    stopFilePath_ = QDir::temp().filePath(
        "ScreenShare-stop-" +
        QString::number(QCoreApplication::applicationPid()) +
        "-" +
        QString::number(++runSerial_) +
        ".signal");
    QFile::remove(stopFilePath_);

    if (request.enableControlFile) {
        controlFilePath_ = QDir::temp().filePath(
            "ScreenShare-control-" +
            QString::number(QCoreApplication::applicationPid()) +
            "-" +
            QString::number(runSerial_) +
            ".txt");
        QFile::remove(controlFilePath_);
    }

    QStringList arguments = request.arguments;
    arguments << "--stop-file" << stopFilePath_;
    if (!controlFilePath_.isEmpty()) {
        arguments << "--control-file" << controlFilePath_;
    }

    process_.setProgram(request.program);
    process_.setArguments(arguments);
    process_.setWorkingDirectory(QFileInfo(request.program).absolutePath());
    process_.start();
    return true;
}

void ProcessSessionBackend::stop()
{
    if (!isRunning() || stopRequested_) {
        return;
    }

    constexpr int kForceStopDelayMs = 1500;
    stopRequested_ = true;

    QFile stopFile(stopFilePath_);
    if (stopFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        stopFile.write("stop\n");
        stopFile.close();
    } else if (messageHandler_) {
        messageHandler_(QStringLiteral("Could not write graceful stop signal; forcing close if needed\n"));
    }

    const quint64 runSerialAtStop = runSerial_;
    QTimer::singleShot(kForceStopDelayMs, this, [this, runSerialAtStop] {
        if (!stopRequested_ ||
            runSerialAtStop != runSerial_ ||
            process_.state() == QProcess::NotRunning) {
            return;
        }
        if (messageHandler_) {
            messageHandler_(QStringLiteral("Stop cleanup timed out; forcing process closed...\n"));
        }
        forcedStop_ = true;
        process_.kill();
    });
}

void ProcessSessionBackend::cleanupStopFile()
{
    if (!stopFilePath_.isEmpty()) {
        QFile::remove(stopFilePath_);
        stopFilePath_.clear();
    }
}

void ProcessSessionBackend::cleanupControlFile()
{
    if (!controlFilePath_.isEmpty()) {
        QFile::remove(controlFilePath_);
        controlFilePath_.clear();
    }
}
