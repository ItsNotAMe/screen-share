# Screen Share

Native Windows screen sharing in C++.

Screen Share captures a display or application window, encodes video with H.264, captures audio with
WASAPI/Opus, and sends media directly over UDP. The desktop UI uses the native backend in-process,
while `ScreenShare.exe` remains available for CLI diagnostics and automation.

## Current Capabilities

- Share a full display or selected application window.
- Capture system, microphone, or selected-process audio.
- Join rooms through the built-in Internet signaling Worker flow.
- Keep media direct over UDP; the Worker coordinates rooms and UDP candidates but does not relay media.
- Decode and preview video in the Qt UI with a Direct3D/NV12 render path.
- Support multiple viewers, room passwords, automatic UDP encryption keys, reports, and live stream settings.
- Build a portable Windows zip with the UI, CLI, runtime DLLs, README, and license.
- Check GitHub Releases for verified portable updates.

## Requirements

- Windows 10/11
- CMake 3.24+
- MSYS2/MinGW-w64 UCRT64 or Visual Studio with Desktop development with C++
- Windows SDK with C++/WinRT headers
- Opus development package
- Qt 6 Network, Svg, and Widgets for `ScreenShareUi.exe`
- Optional: FFmpeg for inspecting generated media files

For the common MSYS2 setup, run:

```powershell
.\scripts\install-dev-deps.ps1
```

Useful variants:

```powershell
.\scripts\install-dev-deps.ps1 -DryRun
.\scripts\install-dev-deps.ps1 -SkipQt -SkipFfmpeg
.\scripts\install-dev-deps.ps1 -WorkerOnly
.\scripts\install-dev-deps.ps1 -InstallWindowsSdk
```

## Build

Debug build:

```powershell
cmake --preset debug
cmake --build --preset debug
```

Release build and portable package:

```powershell
cmake --preset release
cmake --build --preset release
```

The release build writes a portable zip such as:

```text
build\release\ScreenShare-release-windows-x64.zip
```

Release builds enable compiler-supported LTO/IPO by default. Disable it only when comparing builds
or working around a toolchain issue:

```powershell
cmake --preset release -DSCREENSHARE_ENABLE_RELEASE_LTO=OFF
```

## Run

Start the desktop app:

```powershell
.\build\release\ScreenShareUi.exe
```

Typical flow:

1. On the host computer, choose **Create Room** and start sharing.
2. On the watcher computer, choose **Join Room** and select the room.
3. If the room is locked, enter the room password when prompted.
4. If direct UDP cannot connect, use the generated reports from both computers for diagnostics.

Reports are enabled by default in the UI and are the easiest way to debug connection, audio, video,
or performance issues.

## CLI Smoke Test

Generate an access code:

```powershell
.\build\release\ScreenShare.exe --generate-access-code
```

Start a watcher:

```powershell
.\build\release\ScreenShare.exe --watch 5000 --access-code CODE --log receiver.log
```

Start a sharer:

```powershell
.\build\release\ScreenShare.exe --share 192.168.1.127:5000 --access-code CODE --log sender.log
```

Use `--allow-plaintext` only for intentional unencrypted local tests.

## More Documentation

- [Detailed usage and CLI reference](docs/usage.md)
- [Release and update publishing](docs/release.md)
- [Signaling Worker](signaling-worker/README.md)
- [Assets and branding](assets/README.md)
- [Performance tracking](agents/performance.md) and [CSV results](agents/performance.csv)
- [Current TODO](agents/todo.md)

## Architecture

```text
Capture: Windows Graphics Capture / DXGI fallback
Video:   GPU scale + NV12 conversion -> H.264 encode -> UDP fanout
Audio:   WASAPI capture -> Opus encode -> UDP
Network: STUN + Worker signaling + direct UDP media
Watch:   UDP receive -> H.264 decode -> Direct3D NV12 preview + WASAPI playback
UI:      Qt desktop shell calling the native ScreenShare API in-process
```

The project is still a Windows-native prototype, but the main user path is the desktop UI.
