#pragma once
#include <QProcess>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

// Same production RoomSession/Runtime dependencies as the single-process proof,
// but each receiver has a separate address space, WebRTC engine and audio clock.
inline void ProcessViewer(const char* origin, const char* roomId) {
    auto evidence = std::make_shared<Evidence>();
    RoomSession viewer([evidence](auto identity, auto send) { return std::make_unique<Runtime>(identity, std::move(send), evidence); }, true);
    RoomOptions options; options.origin = origin; options.roomId = roomId; options.nickname = "ProcessViewer";
    auto joining = viewer.Start(options); Check(Get(joining).error == RoomError::None);
    Wait([&] { return evidence->frames >= 30 && evidence->audio->audibleBlocks >= 20; });
    const auto end = std::chrono::steady_clock::now() + 12s;
    while (std::chrono::steady_clock::now() < end) {
        Check(viewer.Status().phase == RoomPhase::Active);
        std::this_thread::sleep_for(20ms);
    }
    const auto frames = evidence->frames.load();
    const auto audio = evidence->audio->audibleBlocks.load();
    auto stop = viewer.Stop(); Get(stop);
    Check(frames >= 250 && audio >= 700 && !evidence->invalid && evidence->destroyed == 1);
    std::cout << QJsonDocument(QJsonObject{{"passed", true}, {"frames", int(frames)}, {"audioBlocks", qint64(audio)},
        {"pid", QCoreApplication::applicationPid()}, {"runtimeReleased", true}}).toJson(QJsonDocument::Compact).toStdString() << std::endl;
}

inline void SeparateProcessProof(const char* origin) {
    auto evidence = std::make_shared<Evidence>();
    RoomSession host([evidence](auto identity, auto send) { return std::make_unique<Runtime>(identity, std::move(send), evidence); }, true);
    RoomOptions options; options.origin = origin; options.host = true; options.nickname = "ProcessHost"; options.name = "Process isolation";
    auto starting = host.Start(options); Check(Get(starting).error == RoomError::None);
    std::array<QProcess, 4> children;
    auto stopChildren = [&] {
        for (auto& child : children) if (child.state() != QProcess::NotRunning) { child.kill(); child.waitForFinished(3000); }
    };
    try {
        for (auto& child : children) {
            child.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
            child.start(QCoreApplication::applicationFilePath(), {"--process-viewer", QString::fromUtf8(origin), QString::fromStdString(host.Status().roomId)});
            Check(child.waitForStarted(5000));
        }
        bool fourConnected = false;
        const auto deadline = std::chrono::steady_clock::now() + 35s;
        while (true) {
            QCoreApplication::processEvents();
            fourConnected |= host.Status().activePeers == 4;
            bool running = false;
            for (auto& child : children) {
                child.setReadChannel(QProcess::StandardError);
                Check(child.bytesAvailable() < 65536);
                child.setReadChannel(QProcess::StandardOutput);
                Check(child.bytesAvailable() < 65536);
                running |= child.state() != QProcess::NotRunning;
            }
            if (!running) break;
            Check(std::chrono::steady_clock::now() < deadline && host.Status().phase == RoomPhase::Active);
            std::this_thread::sleep_for(5ms);
        }
        Check(fourConnected);
        QJsonArray viewers;
        std::set<qint64> processes{QCoreApplication::applicationPid()};
        for (auto& child : children) {
            if (child.exitCode() || child.exitStatus() != QProcess::NormalExit) std::cerr << child.readAllStandardError().toStdString();
            Check(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0);
            QJsonParseError error;
            const auto result = QJsonDocument::fromJson(child.readAllStandardOutput(), &error).object();
            Check(error.error == QJsonParseError::NoError && result["passed"].toBool() && result["runtimeReleased"].toBool());
            Check(processes.insert(result["pid"].toInteger()).second); viewers.append(result);
        }
        auto stop = host.Stop(); Get(stop); Check(evidence->destroyed == 1);
        std::cout << QJsonDocument(QJsonObject{{"passed", true}, {"separateProcesses", 5}, {"viewers", viewers},
            {"externalLatencyVerified", false}, {"physicalInput", false}}).toJson(QJsonDocument::Compact).toStdString() << std::endl;
    } catch (...) { stopChildren(); throw; }
}
