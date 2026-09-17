#include "cli/ScreenShareCLI.h"
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
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--backend" || std::string_view(argv[i]) == "--create-room" ||
            std::string_view(argv[i]) == "--join-room") return RunRoomCli(argc, argv);
    if (argc > 1 && std::string_view(argv[1]) == "--room-v2") return RunRoomCli(argc, argv);
#endif
    return RunScreenShareCli(argc, argv);
}
