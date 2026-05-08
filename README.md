# Screen Share

Native C++ screen-sharing prototype for Windows.

The first milestone is a pure C++ capture foundation:

- Enumerate displays.
- Capture a selected monitor through Windows Graphics Capture, with DXGI Desktop Duplication fallback.
- Run at a requested target FPS.
- GPU-scale captured frames to the requested output resolution.
- Send and receive H.264 packets over UDP for local transport validation.
- Decode received H.264 and preview it in a native Direct3D window.

Hardware encode tuning, audio, and friend pairing are later milestones.

## Build

Requirements:

- Windows 10/11
- MSYS2/MinGW-w64 or Visual Studio with the Desktop development with C++ workload
- CMake 3.24+
- Windows SDK with C++/WinRT headers for the Windows Graphics Capture backend
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

The default capture backend is Windows Graphics Capture (`wgc`), which can provide real scRGB
frames on HDR desktops. The app asks Windows to hide WGC's yellow capture border by default; Windows
can still keep the border if borderless capture permission is denied or unavailable. Add
`--wgc-border` if you want to keep the system capture indicator visible. Use the older DXGI Desktop
Duplication path only as a fallback or comparison:

```powershell
.\build\debug\ScreenShare.exe --display 0 --capture-backend dxgi --width 1920 --height 1080 --fps 60 --seconds 15
```

Capture and encode a short full-resolution H.264 MP4:

```powershell
.\build\debug\ScreenShare.exe --display 0 --fps 60 --seconds 15 --record native.mp4
```

Capture and save the latest post-capture BGRA frame as a BMP diagnostic:

```powershell
.\build\debug\ScreenShare.exe --display 0 --width 1280 --height 720 --fps 60 --seconds 3 --dump-capture-bmp build\debug\capture-latest.bmp
```

Capture and encode a downscaled H.264 MP4:

```powershell
.\build\debug\ScreenShare.exe --display 0 --width 1920 --height 1080 --fps 60 --seconds 15 --record out.mp4 --bitrate-mbps 16
```

Capture and run the streamable H.264 packet encoder:

```powershell
.\build\debug\ScreenShare.exe --display 0 --width 1280 --height 720 --fps 60 --seconds 15 --stream-encode --bitrate-mbps 8
```

Opt into the asynchronous hardware H.264 encoder path after checking available encoders:

```powershell
.\build\debug\ScreenShare.exe --display 0 --width 1280 --height 720 --fps 60 --seconds 15 --stream-encode --stream-encoder hardware --bitrate-mbps 8
```

List Media Foundation H.264 encoders and probe whether they accept the app's NV12/H.264 stream
types for a chosen output shape:

```powershell
.\build\debug\ScreenShare.exe --list-h264-encoders --width 1920 --height 1080 --fps 60
```

Capture, stream-encode, and send H.264 packets over UDP:

```powershell
.\build\debug\ScreenShare.exe --display 0 --width 1280 --height 720 --fps 60 --seconds 15 --udp-send 127.0.0.1:5000 --bitrate-mbps 8
```

The sender automatically checks whether the captured display is running in Windows HDR mode. With
the default WGC backend, HDR desktops are captured as scRGB float frames and converted back to SDR
before encoding. If the preview or recording still looks too bright or too dim, tune the SDR white
point:

```powershell
.\build\debug\ScreenShare.exe --display 0 --width 1280 --height 720 --fps 60 --seconds 15 --udp-send 127.0.0.1:5000 --bitrate-mbps 8 --hdr-sdr-white-nits 203
```

Use a higher white-point value to darken true HDR/scRGB captures, or a lower value to brighten them.
`--no-hdr-to-sdr` keeps the raw conversion behavior for comparison. The older DXGI backend can still
receive HDR-active desktops as BGRA8 instead of true HDR; in that fallback case
`--hdr-sdr-exposure N` controls the conservative BGRA exposure compensation.

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

Listen, decode, and preview received frames in a native window until the window closes:

```powershell
.\build\debug\ScreenShare.exe --udp-recv 5000 --preview
```

Add `--seconds S` to stop preview automatically after a fixed duration.

For color debugging, combine `--dump-capture-bmp` on the sender with `--dump-decoded-bmp` on the
receiver. The sender dump is written after capture scaling and HDR/SDR conversion, before H.264; the
receiver dump is written after UDP reassembly, H.264 decode, and NV12-to-BGRA conversion.

Inspect a recording with FFmpeg:

```powershell
ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height,avg_frame_rate,duration -of default=noprint_wrappers=1 out.mp4
ffmpeg -y -ss 00:00:01 -i out.mp4 -frames:v 1 out-frame.png
ffprobe -v error -f h264 -show_entries stream=codec_name,width,height -of default=noprint_wrappers=1 build\debug\receiver.h264
```

The current executable prints capture stats, performs GPU scaling, can write an H.264 MP4 through
Media Foundation, and can produce streamable H.264 packets through the Microsoft H.264 encoder MFT.
Omit `--width` and `--height` to record or stream-encode at the selected display's native
resolution. Provide them only when you want to downscale; recording and stream-encoding require even
output dimensions because they feed NV12 into H.264. If `--bitrate-mbps` is omitted, the app
chooses a resolution-aware default; increase it manually if text or motion looks soft. The H.264
validation encoders request High Profile for better quality than the default baseline profile. The
MP4 path converts BGRA to BT.709 limited-range NV12 before H.264 encoding. The stream path asks the
capturer for GPU-generated BT.709 limited-range NV12 planes and falls back to the shared CPU
conversion only if that GPU payload is not present. Capture stats report the available NV12 path,
the stream input mode, average capture time, and average stream encode time. The encoder paths are
still validation paths, but `--stream-encoder hardware` can now feed the hardware MFT directly from
a GPU NV12 texture and skips capture readbacks when no recording or BMP dump is requested. Capture
stats report `stream_input=d3d11` when that direct path is active. When the source display is in Windows
HDR mode, the default Windows Graphics Capture backend requests scRGB float frames and the capture
shader converts them into SDR BGRA before CPU readback and encoding. If the older DXGI backend
provides an HDR-active desktop as BGRA8, the shader keeps the normal SDR color path but applies a
conservative exposure multiplier. SDR displays keep the normal capture path. The default SDR white
point is 203 nits and can be adjusted with `--hdr-sdr-white-nits N`; the DXGI HDR-active BGRA
fallback exposure is 0.88 and can be adjusted with `--hdr-sdr-exposure N`.

The default `--stream-encode` path still uses the stable software encoder MFT and feeds it through
system-memory NV12 samples. Use `--width`/`--height` for that validation path at high resolutions. Use
`--list-h264-encoders` to inspect available Media Foundation encoders; hardware MFTs are reported
with async/D3D11-manager support and whether they accept the app's current NV12 input and H.264
output media types. The stream path uses the stable software encoder MFT by default. Add
`--stream-encoder hardware` to try a hardware MFT through its asynchronous event model and direct
D3D11 NV12 texture input. The remaining hardware optimization is deeper pipelining so the sender
does less per-frame waiting on encoder events.

The `--udp-send` path fragments each encoded H.264 packet into MTU-friendly UDP datagrams with a
small header. The `--udp-recv` path binds a local UDP port, validates those datagrams, reassembles
complete encoded frames, and prints transport diagnostics. Add `--dump-h264 PATH` on the receiver
to write the reassembled H.264 elementary stream for FFmpeg inspection. It does not decode or
display video by default. Add `--decode-h264` to feed reassembled packets through the Microsoft
H.264 decoder MFT and print decoded frame diagnostics. Add `--dump-decoded-bmp PATH` to save the
latest decoded NV12 frame as a BMP snapshot. The raw `.h264` dump validates codec bytes and
dimensions, but it does not store transport timing. Add `--preview` on the receiver to open a
native Win32/Direct3D preview window; when `--seconds` is omitted the preview runs until the window
closes.

Windows display capture is event-driven: Windows returns a fresh frame when the desktop changes.
The stats therefore report both paced output frames and actual desktop update frames. A still
desktop can show near-zero `desktop_update_fps` while `output_fps` stays near the requested FPS.

## Architecture

Planned pipeline:

```text
Windows Graphics Capture
 -> DXGI Desktop Duplication fallback
 -> GPU scaling and stream NV12 conversion
 -> Media Foundation H.264 file encode for validation
 -> Microsoft H.264 MFT packet encode for transport validation
 -> H.264 hardware encoder capability probe
 -> optional asynchronous hardware H.264 stream encoder with direct D3D11 NV12 input
 -> UDP sender/receiver transport diagnostics
 -> Media Foundation H.264 decode validation
 -> native Direct3D receiver preview
 -> future deeper hardware encoder pipelining
 -> future GPU-side color conversion and renderer optimizations
```
An app to share your screen with others
