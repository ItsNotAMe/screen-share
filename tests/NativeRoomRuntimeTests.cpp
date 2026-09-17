#include "api/RoomSession.h"
#include "media/webrtc/WindowsRoomRuntime.h"
#include "media/ReceiverTelemetry.h"
#include <QCoreApplication>
#include <iostream>
// Links the complete shipped Windows runtime through the application core.
// No capture/audio devices are opened before authenticated admission.
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        using namespace screenshare::media;
        using namespace std::chrono_literals;
        auto require = [](bool value) { if (!value) throw std::runtime_error("Receiver telemetry protocol check failed"); };
        const auto now = ReceiverTelemetryInbox::Clock::now();
        ReceiverTelemetryMessage message{"media_1", 1, {320, 180, 42, 30000}};
        const auto wire = EncodeReceiverTelemetry(message);
        require(wire.size() == 33);
        auto decoded = DecodeReceiverTelemetry(wire);
        require(decoded && decoded->video.framesDecoded == 42 && decoded->video.fpsMilli == 30000);
        for (size_t length = 0; length < wire.size(); ++length) require(!DecodeReceiverTelemetry(std::span(wire).first(length)));
        auto bad = wire; bad.push_back(0); require(!DecodeReceiverTelemetry(bad));
        bad = wire; bad[3] = 2; require(!DecodeReceiverTelemetry(bad));
        bad = wire; bad[5] = 2; require(!DecodeReceiverTelemetry(bad));
        bad = wire; bad[5] = 0; require(!DecodeReceiverTelemetry(bad));
        bad = wire; bad[14] = 0; bad[15] = 1; require(!DecodeReceiverTelemetry(bad));
        message.video.fpsMilli = 240001; require(EncodeReceiverTelemetry(message).empty());
        message.video.fpsMilli = 0; auto zero = DecodeReceiverTelemetry(EncodeReceiverTelemetry(message));
        require(zero && zero->video.fpsMilli == 0);
        message.video.fpsMilli.reset(); require(!DecodeReceiverTelemetry(EncodeReceiverTelemetry(message))->video.fpsMilli);
        ReceiverTelemetryInbox inbox; inbox.Bind("media_1");
        require(!inbox.Read(now).observation && !inbox.Read(now).stale);
        require(inbox.Receive(wire, now));
        require(!inbox.Receive(wire, now + 1s));
        require(inbox.Read(now + 2999ms).observation.has_value());
        require(inbox.Read(now + 3s).stale && !inbox.Read(now + 3s).observation);
        inbox.Bind("media_1_restart_2"); require(!inbox.Read(now + 3s).stale);
        require(!inbox.Receive(wire, now + 3s));
        message.connection = "media_1_restart_2"; message.sequence = 1;
        require(inbox.Receive(EncodeReceiverTelemetry(message), now + 4s));
        message.sequence = 2; require(inbox.Receive(EncodeReceiverTelemetry(message), now + 4s));
        message.sequence = 1; require(!inbox.Receive(EncodeReceiverTelemetry(message), now + 4s));
        message.sequence = 3; require(inbox.Receive(EncodeReceiverTelemetry(message), now + 4s));
        message.sequence = 4; require(!inbox.Receive(EncodeReceiverTelemetry(message), now + 4s));
        require(inbox.Receive(EncodeReceiverTelemetry(message), now + 5s));
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
