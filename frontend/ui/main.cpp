#include "core/WindowsMediaRuntime.h"
#include "ui/AppShellWindow.h"
#include "ui/UpdateManager.h"
#ifdef SCREENSHARE_HAS_ROOM_V2_UI
#include "ui/RoomBrowserWindow.h"
#include "ui/RoomSessionWindow.h"
#include "shared/RoomLaunch.h"
#endif
#include <QApplication>
#include <QFile>
#include <QIcon>
#include <QTimer>
#include <QSvgRenderer>
#include <stdexcept>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    app.setWindowIcon(QIcon(":/screenshare/brand/screenshare-mark.svg"));
    const auto arguments = app.arguments().mid(1);
    if (arguments == QStringList{"--self-test"} || arguments == QStringList{"--gui-smoke-test"}) {
        // Exercise relocated Qt plugins/resources without network or devices.
        if (app.windowIcon().isNull()) return 2;
        for (const auto* icon : {"share", "watch"}) {
            QFile resource(QString(":/screenshare/ui/icons/%1.svg").arg(icon));
            if (!resource.open(QIODevice::ReadOnly)) return 2;
            auto svg = resource.readAll(); svg.replace("currentColor", "#ffffff");
            if (!QSvgRenderer(svg).isValid()) return 2;
        }
#ifdef SCREENSHARE_HAS_ROOM_V2_UI
        try { ParseRoomHomeLaunch({}); } catch (...) { return 2; }
        return 0;
#else
        return 2;
#endif
    }
    screenshare::WindowsMediaRuntime mediaRuntime;
    if (FAILED(mediaRuntime.result())) {
        qCritical("Failed to initialize the Windows media runtime: 0x%08lx",
            static_cast<unsigned long>(mediaRuntime.result()));
        return 1;
    }
#ifdef SCREENSHARE_HAS_ROOM_V2_UI
    try {
        if (!arguments.isEmpty() && arguments.front() == "--room-v2") {
            if (arguments.size() != 2) throw std::invalid_argument("Usage: ScreenShareUi --room-v2 CONFIG.json");
            return RunRoomSessionWindow(arguments[1]);
        }
        // Keep explicit diagnostic shortcuts; normal startup uses the same shell.
        if (!arguments.isEmpty() && arguments.front() == "--room-v2-browser") {
            if (arguments.size() != 2) throw std::invalid_argument("Usage: ScreenShareUi --room-v2-browser HTTPS_ORIGIN");
            return RunRoomBrowserWindow(ParseRoomHomeLaunch({"--signal-server", arguments[1]}));
        }
        return RunRoomBrowserWindow(ParseRoomHomeLaunch(arguments), true, [](AppShellWindow& shell) {
            auto* updater = new UpdateManager(&shell, &shell);
            QTimer::singleShot(1500, updater, [updater] { updater->checkForUpdates(); });
        });
    } catch (const std::exception& error) {
        qCritical("Room launch: %s", error.what());
        return 1;
    }
#else
    qCritical("This application requires the modular room runtime. Build with the pinned WebRTC SDK.");
    return 1;
#endif
}
