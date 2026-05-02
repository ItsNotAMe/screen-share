# Screen Share

Native C++ screen-sharing prototype for Windows.

The first milestone is a pure C++ capture foundation:

- Enumerate displays.
- Capture a selected monitor through DXGI Desktop Duplication.
- Run at a requested target FPS.
- GPU-scale captured frames to the requested output resolution.

Streaming, hardware encoding, audio, and friend pairing are the next milestones.

## Build

Requirements:

- Windows 10/11
- MSYS2/MinGW-w64 or Visual Studio with the Desktop development with C++ workload
- CMake 3.24+

```powershell
cmake --preset debug
cmake --build --preset debug
```

## Run

List monitors:

```powershell
.\build\debug\ScreenShare.exe --list
```

Capture monitor 0 at a 60 FPS target and request 1080p output:

```powershell
.\build\debug\ScreenShare.exe --display 0 --width 1920 --height 1080 --fps 60 --seconds 15
```

Capture and encode a short H.264 MP4:

```powershell
.\build\debug\ScreenShare.exe --display 0 --width 1920 --height 1080 --fps 60 --seconds 15 --record out.mp4 --bitrate-mbps 16
```

The current executable prints capture stats, performs GPU scaling, and can write an H.264 MP4
through Media Foundation. The recording path is a validation path: it avoids forcing hardware
transforms and still uses CPU readback after scaling. The future streaming encoder should consume
GPU textures directly.

Desktop Duplication is event-driven: Windows returns a fresh frame when the desktop changes.
The stats therefore report both paced output frames and actual desktop update frames. A still
desktop can show near-zero `desktop_update_fps` while `output_fps` stays near the requested FPS.

## Architecture

Planned pipeline:

```text
DXGI/Windows Graphics Capture
 -> GPU scaling
 -> Media Foundation H.264 file encode for validation
 -> future real-time hardware encode path
 -> native real-time transport
 -> decode and Direct3D render on receiver
```
An app to share your screen with others
