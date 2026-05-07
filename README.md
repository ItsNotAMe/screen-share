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

Listen for UDP H.264 packet fragments and reassemble complete encoded frames:

```powershell
.\build\debug\ScreenShare.exe --udp-recv 5000 --seconds 15
```

Listen, reassemble, and dump the received H.264 elementary stream for inspection:

```powershell
.\build\debug\ScreenShare.exe --udp-recv 5000 --seconds 15 --dump-h264 build\debug\receiver.h264
```

Listen, reassemble, and decode received H.264 packets without displaying them yet:

```powershell
.\build\debug\ScreenShare.exe --udp-recv 5000 --seconds 15 --decode-h264
```

Listen, decode, and save the latest decoded frame as a BMP snapshot:

```powershell
.\build\debug\ScreenShare.exe --udp-recv 5000 --seconds 15 --dump-decoded-bmp build\debug\receiver.bmp
```

Inspect a recording with FFmpeg:

```powershell
ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height,avg_frame_rate,duration -of default=noprint_wrappers=1 out.mp4
ffmpeg -y -ss 00:00:01 -i out.mp4 -frames:v 1 out-frame.png
ffprobe -v error -f h264 -show_entries stream=codec_name,width,height -of default=noprint_wrappers=1 build\debug\receiver.h264
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

The `--udp-send` path fragments each encoded H.264 packet into MTU-friendly UDP datagrams with a
small header. The `--udp-recv` path binds a local UDP port, validates those datagrams, reassembles
complete encoded frames, and prints transport diagnostics. Add `--dump-h264 PATH` on the receiver
to write the reassembled H.264 elementary stream for FFmpeg inspection. It does not decode or
display video by default. Add `--decode-h264` to feed reassembled packets through the Microsoft
H.264 decoder MFT and print decoded frame diagnostics. Add `--dump-decoded-bmp PATH` to save the
latest decoded NV12 frame as a BMP snapshot. The raw `.h264` dump validates codec bytes and
dimensions, but it does not store transport timing. A native renderer is a future milestone.

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
 -> UDP sender/receiver transport diagnostics
 -> Media Foundation H.264 decode validation
 -> future real-time hardware encode path
 -> future native Direct3D receiver renderer
```
An app to share your screen with others
