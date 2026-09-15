#include "cli/ScreenShareCLI.h"
#include "core/WindowsMediaRuntime.h"
#include <iostream>

int main(int argc, char** argv)
{
    screenshare::WindowsMediaRuntime mediaRuntime;
    if (FAILED(mediaRuntime.result())) {
        std::cerr << "Failed to initialize the Windows media runtime: 0x"
                  << std::hex << mediaRuntime.result() << '\n';
        return 1;
    }
    return RunScreenShareCli(argc, argv);
}
