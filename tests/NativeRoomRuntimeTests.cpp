#include "api/RoomSession.h"
#include "media/webrtc/WindowsRoomRuntime.h"
#include <QCoreApplication>
#include <iostream>
// Links the complete shipped Windows runtime through the application core.
// No capture/audio devices are opened before authenticated admission.
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        screenshare::media::WindowsRoomRuntimeOptions options;
        screenshare::v2::RoomSession session(screenshare::media::WindowsRoomRuntimeFactory(options));
        auto stopped = session.Stop();
        if (stopped.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return 1;
        stopped.get();
        if (session.Status().phase != screenshare::v2::RoomPhase::Stopped) return 1;
        auto rejected = session.Start({});
        if (rejected.get().error != screenshare::v2::RoomError::Busy) return 1;
        std::cout << "{\"passed\":true,\"windows_runtime_linked\":true}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
