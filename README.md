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
- Optional: FFmpeg for inspecting generated MP4 files

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

Capture and encode a short full-resolution H.264 MP4:

```powershell
.\build\debug\ScreenShare.exe --display 0 --fps 60 --seconds 15 --record native.mp4
```

Capture and encode a downscaled H.264 MP4:

```powershell
.\build\debug\ScreenShare.exe --display 0 --width 1920 --height 1080 --fps 60 --seconds 15 --record out.mp4 --bitrate-mbps 16
```

Capture and run the streamable H.264 packet encoder:

```powershell
.\build\debug\ScreenShare.exe --display 0 --width 1280 --height 720 --fps 60 --seconds 15 --stream-encode --bitrate-mbps 8
```

Capture, stream-encode, and send H.264 packets over UDP:

```powershell
.\build\debug\ScreenShare.exe --display 0 --width 1280 --height 720 --fps 60 --seconds 15 --udp-send 127.0.0.1:5000 --bitrate-mbps 8
```

Inspect a recording with FFmpeg:

```powershell
ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height,avg_frame_rate,duration -of default=noprint_wrappers=1 out.mp4
ffmpeg -y -ss 00:00:01 -i out.mp4 -frames:v 1 out-frame.png
```

The current executable prints capture stats, performs GPU scaling, can write an H.264 MP4 through
Media Foundation, and can produce streamable H.264 packets through the Microsoft H.264 encoder MFT.
Omit `--width` and `--height` to record or stream-encode at the selected display's native
resolution. Provide them only when you want to downscale. If `--bitrate-mbps` is omitted, the app
chooses a resolution-aware default; increase it manually if text or motion looks soft. The H.264
validation encoders request High Profile for better quality than the default baseline profile. The
encoder paths are validation paths: they still use CPU readback after scaling. The MP4 path accounts
for Media Foundation RGB row orientation so recordings play upright. The future streaming encoder
should consume GPU textures directly.

The `--stream-encode` path is CPU-heavy at native high resolutions because it currently converts
BGRA to NV12 on the CPU. Use `--width`/`--height` for that validation path until GPU color
conversion and real hardware encoding are added.

The `--udp-send` path is sender-only for now. It fragments each encoded H.264 packet into
MTU-friendly UDP datagrams with a small header. A native receiver/decoder is a future milestone.

Desktop Duplication is event-driven: Windows returns a fresh frame when the desktop changes.
The stats therefore report both paced output frames and actual desktop update frames. A still
desktop can show near-zero `desktop_update_fps` while `output_fps` stays near the requested FPS.

## Architecture

Planned pipeline:

```text
DXGI/Windows Graphics Capture
 -> GPU scaling
 -> Media Foundation H.264 file encode for validation
 -> Microsoft H.264 MFT packet encode for transport validation
 -> future real-time hardware encode path
 -> native real-time transport
 -> decode and Direct3D render on receiver
```
An app to share your screen with others
