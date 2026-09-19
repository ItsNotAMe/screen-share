#include "core/WindowsMediaRuntime.h"
#include <iostream>
#ifdef SCREENSHARE_HAS_ROOM_V2_CLI
#include "cli/RoomCli.h"
#include <string_view>
#endif

int main(int argc, char** argv)
{
    screenshare::WindowsMediaRuntime mediaRuntime;
    if (FAILED(mediaRuntime.result())) {
        std::cerr << "Failed to initialize the Windows media runtime: 0x"
                  << std::hex << mediaRuntime.result() << '\n';
        return 1;
    }
#ifdef SCREENSHARE_HAS_ROOM_V2_CLI
    return RunRoomCli(argc, argv);
#else
    std::cerr << "This application requires the modular room runtime. Build with the pinned WebRTC SDK.\n";
    return 1;
#endif
}
