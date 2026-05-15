# Screen Share

Native C++ screen-sharing prototype for Windows.

The first milestone is a pure C++ capture foundation:

- Enumerate displays.
- Capture a selected monitor through Windows Graphics Capture, with DXGI Desktop Duplication fallback.
- Run at a requested target FPS.
- GPU-scale captured frames to the requested output resolution.
- Send and receive H.264 packets over UDP for local transport validation.
- Decode received H.264 and preview it in a native Direct3D window with GPU-side NV12 conversion.
- Capture system or microphone audio with WASAPI.
- Send Opus-compressed audio packets over UDP by default and optionally play them on the receiver with a small jitter buffer.

Internet/NAT traversal is a later milestone.

## Build

Requirements:

- Windows 10/11
- MSYS2/MinGW-w64 or Visual Studio with the Desktop development with C++ workload
- CMake 3.24+
- Windows SDK with C++/WinRT headers for the Windows Graphics Capture backend
- Opus development package for compressed audio (`mingw-w64-ucrt-x86_64-opus` on MSYS2 UCRT64)
- Optional: Qt 6 Widgets (`mingw-w64-ucrt-x86_64-qt6-base` on MSYS2 UCRT64) for the desktop control UI
- Optional: FFmpeg for inspecting generated MP4 files

```powershell
cmake --preset debug
cmake --build --preset debug
```

When building with MSYS2/MinGW, the default build statically links the GCC C++ and pthread runtime
libraries and copies the remaining runtime DLLs next to `ScreenShare.exe`: Opus, the Windows UCRT
redist DLLs, and `d3dcompiler_47.dll` when they are available from the Windows SDK. To run on another
computer, delete any old copied output folder first and then copy the whole fresh `build\debug` or
`build\release` output folder, not just `ScreenShare.exe`. If you previously copied
`libgcc_s_seh-1.dll`, `libstdc++-6.dll`, or `libwinpthread-1.dll` beside the exe, delete those old
copies after rebuilding because they are no longer needed by the default MinGW build. The build also
writes `ScreenShare-runtime-dependencies.txt` beside the exe with the local source path for each
staged runtime DLL.

The normal build also creates a portable zip automatically:

```powershell
cmake --build --preset release
```

The zip is written beside the build output, for example
`build\release\ScreenShare-release-windows-x64.zip`. It contains `ScreenShare.exe`, staged runtime
DLLs, the dependency manifest, README, and license. When Qt 6 Widgets is available at configure
time, the build also creates `ScreenShareUi.exe` and packages the Qt DLLs and plugin folders needed
to run the desktop app on another Windows computer.

This is attached to the default CMake `all` build, which is what the VS Code CMake Tools build
button normally runs. If you changed the selected CMake target to `ScreenShare`, switch it back to
`all` or select `package-portable` when you want to force a fresh zip.

To regenerate only the package after configuring the preset, build the package target:

```powershell
cmake --build --preset release --target package-portable
```

Set `SCREENSHARE_PACKAGE_PORTABLE_ON_BUILD=OFF` at configure time if you want normal builds to skip
zip creation.

## Run

Start the desktop control UI:

```powershell
.\build\release\ScreenShareUi.exe
```

The UI starts and stops the same `ScreenShare.exe` engine beside it. Use the Share tab on the
sending computer and the Watch tab on the receiving computer. Reports are enabled by default so a
test run can be sent as a zip without collecting separate log files. The UI opens in dark mode by
default, includes a theme toggle in the header, and can generate/copy an encrypted-session access
code.

Common live session:

```powershell
.\build\release\ScreenShare.exe --generate-access-code
.\build\release\ScreenShare.exe --watch 5000 --access-code CODE --log receiver.log
.\build\release\ScreenShare.exe --share 192.168.1.127:5000 --access-code CODE --log sender.log
```

`--watch PORT` expands to the normal receiver preview path: `--udp-recv PORT --preview
--audio-playback`, with default A/V sync enabled. `--share HOST:PORT` expands to the normal sender
path: `--udp-send HOST:PORT --audio-capture system --adapt-bitrate --adapt-resolution`, with a
live UDP queue cap so old queued video does not build into multi-second latency. It currently
defaults to the software H.264 encoder because the Windows/NVIDIA hardware MFT can build an input
queue and drop frames on some systems; pass `--stream-encoder hardware` when you want to test the
hardware path explicitly. The share preset runs until you stop it with Ctrl+C by default; add
`--seconds S` to choose a shorter test. Add the same `--session ID` on both sides when you want
sender and receiver logs/reports to be easy to match later. If omitted, each process generates its
own diagnostic session ID.
Use `--allow-plaintext` instead of `--access-code` only when you intentionally want an unencrypted
local UDP session.

LAN discovery can find a receiver without manually looking up its IP address. On the watching
computer, start Watch with LAN advertising:

```powershell
.\build\release\ScreenShare.exe --watch 5000 --lan-advertise --log receiver.log
```

On the sharing computer, discover nearby receivers:

```powershell
.\build\release\ScreenShare.exe --lan-discover
```

The output includes `share_target=HOST:PORT` plus a ready-to-run `ScreenShare --share ...` command.
The desktop UI exposes the same flow: Watch has a LAN discoverable checkbox, and Share has a
Find on LAN button that fills the address and port. Discovery uses UDP port 47995 by default; add
`--lan-discovery-port PORT` on both sides if you need a different port.
Discovery also reports whether the receiver expects encrypted traffic. Use the same access code on
both computers for encrypted sessions; there is no separate LAN invite code. If Watch is using an
access code, discovery prints `security=encrypted` and an access-code fingerprint, and the generated
Share command uses `--access-code CODE` as a placeholder. The raw access code is never broadcast.
The desktop UI compares the typed access code with the selected receiver fingerprint after Find on
LAN. If Watch is explicitly plaintext, discovery prints `security=plaintext` and the generated Share
command includes `--allow-plaintext`.

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

Capture and run the streamable H.264 packet encoder. The default stream encoder preference is
`auto`: the app tries the vetted hardware path first and falls back to software if hardware setup is
not available.

```powershell
.\build\debug\ScreenShare.exe --display 0 --width 1280 --height 720 --fps 60 --seconds 15 --stream-encode --bitrate-mbps 8
```

Force the asynchronous hardware H.264 encoder path after checking available encoders:

```powershell
.\build\debug\ScreenShare.exe --display 0 --width 1280 --height 720 --fps 60 --seconds 15 --stream-encode --stream-encoder hardware --bitrate-mbps 8
```

Force the software encoder path for comparison:

```powershell
.\build\debug\ScreenShare.exe --display 0 --width 1280 --height 720 --fps 60 --seconds 15 --stream-encode --stream-encoder software --bitrate-mbps 8
```

Stream encoding requests a periodic keyframe every 2 seconds by default so receivers can recover
after lost H.264 frames. Use `--keyframe-interval S` to tune that interval, or
`--keyframe-interval 0` to keep the encoder's default GOP behavior.

List Media Foundation H.264 encoders and probe whether they accept the app's NV12/H.264 stream
types for a chosen output shape:

```powershell
.\build\debug\ScreenShare.exe --list-h264-encoders --width 1920 --height 1080 --fps 60
```

Capture, stream-encode, and send H.264 packets over UDP:

```powershell
.\build\debug\ScreenShare.exe --display 0 --width 1280 --height 720 --fps 60 --seconds 15 --udp-send 127.0.0.1:5000 --bitrate-mbps 8
```

Add system audio to the same UDP sender:

```powershell
.\build\debug\ScreenShare.exe --display 0 --width 1280 --height 720 --fps 60 --seconds 15 --udp-send 127.0.0.1:5000 --bitrate-mbps 8 --audio-capture system
```

Raw audio is still available for transport diagnostics:

```powershell
.\build\debug\ScreenShare.exe --display 0 --width 1280 --height 720 --fps 60 --seconds 15 --udp-send 127.0.0.1:5000 --bitrate-mbps 8 --audio-capture system --audio-codec raw
```

UDP sending is paced by default using the selected stream bitrate, which spreads encoded frame
fragments over time instead of dumping each frame as one burst. Add `--no-udp-pacing` when you want
the older raw burst behavior for transport diagnostics.

Sender stats include `udp_queue_ms` so you can see how much future send time is currently waiting in
the paced UDP queue. The experimental `--udp-max-queue-ms MS` option can cap that queue by dropping
older queued media. Raw `--udp-send` keeps the default `0` queue cap disabled for diagnostics, while
the `--share` preset defaults to a live cap. Dropping queued H.264 inside a GOP can force receiver
recovery at the next keyframe, so prefer `--adapt-bitrate` and `--adapt-resolution` for normal live
runs.
The sender also gives the UDP pacer a small amount of headroom above the current encoder bitrate so
packet overhead and short H.264 bursts can drain instead of turning into permanent backlog.

Add `--adapt-bitrate` to let receiver feedback apply conservative live bitrate changes to the
active stream encoder and UDP pacing queue. The sender reduces quickly on loss/recovery signals and
increases more slowly after repeated clean feedback, capped at the original target bitrate.
Use `--adapt-min-bitrate-mbps Mbps` to set the floor for reductions, and
`--adapt-reduce-cooldown S` to control how many seconds of feedback must pass between repeated
downward steps. With `--adapt-resolution`, the default floor is lower than the old target/4 rule so
the sender can reduce bitrate further before dropping to the next resolution tier.

Add `--adapt-resolution` to let the sender restart capture and stream encoding at a lower output
resolution after bitrate reaches its floor and receiver pressure continues. When feedback stabilizes,
resolution can step back up again after several stable feedback reports, which helps avoid bouncing
between tiers. Use `--adapt-resolution-min-scale N` to set the smallest allowed scale and
`--adapt-resolution-cooldown S` to space out resolution changes. Lower adaptive tiers are rounded to
H.264-friendly dimensions so receiver decoders do not add hidden padding.
Sustained sender-side queue pressure can also trigger a resolution step-down once bitrate has already
been reduced substantially, which avoids staying at native 1440p while the UDP queue is seconds
behind.
For example, 75% of a 2560x1440 capture becomes `1920x1088` because H.264 encoders prefer
16-pixel-aligned dimensions.

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

Add `--log PATH` to any command to save the console output while still showing it in the terminal.
For example, use this when sending receiver diagnostics:

```powershell
.\build\debug\ScreenShare.exe --udp-recv 5000 --preview --audio-playback --log receiver.log
```

Use `--save-report PATH` instead of `--log PATH` when you want a zipped report for one run:

```powershell
.\build\debug\ScreenShare.exe --watch 5000 --save-report receiver-report.zip
.\build\debug\ScreenShare.exe --share 192.168.1.127:5000 --session game-night --save-report sender-report.zip
```

Add `--access-code CODE` on both sides when you want an encrypted local session. The receiver
rejects senders that do not know the same code:

```powershell
.\build\debug\ScreenShare.exe --generate-access-code
.\build\debug\ScreenShare.exe --watch 5000 --access-code CODE
.\build\debug\ScreenShare.exe --share 192.168.1.127:5000 --access-code CODE
```

The access code derives the UDP encryption key locally, and saved reports redact the raw code.
Video/audio payloads and receiver feedback are encrypted with Windows CNG AES-GCM; packet metadata
such as sizes and timestamps remains visible for routing, pacing, and diagnostics. Use a reasonably
long/random code on untrusted LANs; short codes can still be guessed.

The report contains the command's console output, a `ScreenShare-report.txt` summary, and the
runtime dependency manifest when it is available beside the executable. It also records the local
session ID and fingerprint. Receiver feedback includes the receiver session fingerprint, so a
sender report can be matched to the receiver report that saw the same session. When the sender sees
receiver feedback, the report summary includes the latest compact receiver health snapshot too. You
can still use `--log PATH` when you want a plain text log file.

Stress the receiver with simulated jitter or datagram loss:

```powershell
.\build\debug\ScreenShare.exe --udp-recv 5000 --preview --simulate-jitter-ms 40
.\build\debug\ScreenShare.exe --udp-recv 5000 --seconds 15 --simulate-loss-percent 1
```

When `--decode-h264` or `--preview` is enabled, the receiver watches for gaps in the ordered H.264
packet stream. If loss leaves a missing frame behind, it skips damaged backlog at the next IDR
keyframe and resumes decode from that recovery point.

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

Preview window controls:

- F11 or Alt+Enter toggles fullscreen.
- Esc leaves fullscreen.
- F toggles between fit-to-window and 1:1 source-size scaling.
- 1 switches to 1:1 scaling and resizes the window toward the current source resolution.
- M toggles receiver audio mute when audio playback is enabled.
- + and - adjust receiver playback volume in 5% steps.

Listen, preview video, and play received audio:

```powershell
.\build\debug\ScreenShare.exe --udp-recv 5000 --preview --audio-playback
```

For same-computer loopback tests, keep receiving and timing audio but render it muted so the receiver
does not feed back into system-audio capture:

```powershell
.\build\debug\ScreenShare.exe --watch 5000 --audio-playback-muted
```

This automatically enables A/V sync correction. Disable it only for diagnostics:

```powershell
.\build\debug\ScreenShare.exe --udp-recv 5000 --preview --audio-playback --no-av-sync
```

Add `--seconds S` to stop preview automatically after a fixed duration. The preview playout buffer
starts with 150 ms of latency for video-only preview, or 100 ms when A/V sync is enabled, and drops
frames that are more than 500 ms late by default. Use `--preview-latency-ms MS` to trade latency for
jitter tolerance, and `--preview-max-late-ms MS` to control how long late frames can stay eligible for
presentation before they are dropped. When A/V sync is enabled, the effective audio jitter target is
kept at least as large as the preview latency so the audio and video playout clocks stay connected.

Capture system audio and send it to the receiver:

```powershell
.\build\debug\ScreenShare.exe --audio-capture system --audio-send 127.0.0.1:5000 --seconds 15
```

Opus is the default audio codec for `--audio-send` and combined `--udp-send --audio-capture`.
Use `--audio-codec raw` when you want uncompressed WASAPI packets for diagnostics.

Play received audio on the receiver:

```powershell
.\build\debug\ScreenShare.exe --udp-recv 5000 --audio-playback --seconds 15
```

Audio playback is opt-in. It uses the default Windows render endpoint in shared mode and starts
after buffering 120 ms of audio by default. Use `--audio-playback-latency-ms MS` to trade latency
for jitter tolerance. Use `--audio-playback-muted` to consume and time audio packets without audible
output, which is useful when testing sender and receiver on the same computer. Use
`--audio-playback-volume PERCENT` to set the initial receiver playback volume from 0 to 200 percent.
When the preview window is open, M toggles mute and + or - adjusts volume in 5% steps. The receiver
still prints audio transport diagnostics when playback is not enabled.

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
a GPU NV12 texture and skips capture readbacks when no recording or BMP dump is requested. The
hardware stream encoder uses Media Foundation's asynchronous event model with a bounded input queue
instead of waiting for every frame. Capture stats report `stream_input=d3d11`, `stream_queue`, and
`stream_dropped` when that direct path is active. When the source display is in Windows HDR mode, the
default Windows Graphics Capture backend requests scRGB float frames and the capture shader converts
them into SDR BGRA before CPU readback and encoding. WGC uses its first available frame so a still
desktop can start streaming immediately. If the older DXGI backend provides an HDR-active desktop as
BGRA8, the shader keeps the normal SDR color path but applies a conservative exposure multiplier. SDR
displays keep the normal capture path. The default SDR white point is 203 nits and can be adjusted
with `--hdr-sdr-white-nits N`; the DXGI HDR-active BGRA fallback exposure is 0.88 and can be adjusted
with `--hdr-sdr-exposure N`.

The default `--stream-encode` path uses `--stream-encoder auto`. Auto mode tries the vetted
asynchronous hardware MFT path with direct D3D11 NV12 texture input, then falls back to the stable
software encoder MFT if hardware setup fails. If fallback requires CPU-visible NV12 and the capture
path was optimized for hardware input, the sender restarts capture once with software-compatible
readback enabled. Add `--stream-encoder hardware` to require hardware and fail loudly if it is not
available, or `--stream-encoder software` for comparison/debugging. Use `--list-h264-encoders` to
inspect available Media Foundation encoders; hardware MFTs are reported with async/D3D11-manager
support and whether they accept the app's current NV12 input and H.264 output media types. The
hardware path also asks supported encoder MFTs for low-latency mode and no B-frames, while keeping a
bounded queue so live capture can keep pacing even when the encoder has startup latency. Stream
encoding also requests closed periodic GOPs with one IDR per GOP; the default interval is 2 seconds
and can be adjusted with `--keyframe-interval S`.

The `--udp-send` path fragments each encoded H.264 packet into MTU-friendly UDP datagrams with a
small header. A background sender thread paces queued datagrams against the encoder bitrate by
default, while the capture and encoder loop continues running at the requested FPS. Sender stats
report `udp_queued`, `udp_pending`, `udp_peak_pending`, `udp_queue_ms`, `udp_peak_queue_ms`, and
`udp_dropped_frames` for that pacing queue. The sender also listens on the same UDP socket for
receiver feedback packets and reports
`udp_feedback_health`, `udp_feedback_completed_frames`, `udp_feedback_resyncs`, and
`udp_feedback_skipped_packets` when a receiver is present. Sender stats also include adaptive
bitrate advice through `bitrate_advice_mbps`, `bitrate_advice_action`, and
`bitrate_advice_reason`. By default this remains diagnostics-only; with `--adapt-bitrate`, reduce
and increase advice are applied to the live encoder through Media Foundation's bitrate control and
to the UDP pacing queue. Increases only happen after repeated clean receiver feedback and are capped
at the original target bitrate. Preview-only drops and local UDP queue spikes use short hysteresis
before they can reduce bitrate or resolution, so resize, restart, and playout timing hiccups do not
immediately bounce the stream downward. `--adapt-min-bitrate-mbps Mbps` sets the adaptive bitrate floor, while
`--adapt-reduce-cooldown S` prevents repeated reductions from every feedback report during the same
loss/recovery episode. Add `--adapt-resolution` to make resolution a second adaptation lever: the
sender steps down to lower output-resolution tiers when bitrate is already at its floor and pressure
persists for several reports, then steps back up after sustained stable feedback. Resolution changes
restart capture and the stream encoder while keeping UDP frame IDs and H.264 timestamps moving
forward. Stats report `stream_bitrate_mbps`,
`resolution_scale`, `resolution_adaptation`, `resolution_adaptations`,
`resolution_stable_feedback`, `resolution_stable_required`, `resolution_reduce_pressure`,
`resolution_reduce_required`, `bitrate_advice_min_mbps`,
`bitrate_advice_cooldown`, `bitrate_advice_suppressed`, `bitrate_adaptation`,
`bitrate_adaptations`, and `bitrate_adaptation_failures` so you can see whether the active encoder
accepted the update and whether cooldown is suppressing extra reductions. The
`--udp-recv` path binds a local UDP port, validates media datagrams, reassembles complete encoded
frames, sends compact feedback back to the sender's source address, and prints transport
diagnostics. Receiver-side simulation flags can
delay incoming datagrams with `--simulate-jitter-ms MS` or drop a percentage with
`--simulate-loss-percent P`.
Simulation stats report `simulated_dropped`, `simulated_delayed`, `simulated_delay_pending`,
`feedback_sent`, and `feedback_errors`.
Jitter simulation is useful with `--preview` for testing the playout buffer. Loss simulation is
useful with `--decode-h264` or `--preview` for testing keyframe recovery; receiver stats report
`h264_decode_resyncs`, `h264_decode_restarts`, and `h264_decode_skipped_packets`. Add
`--dump-h264 PATH` on the receiver to write the reassembled H.264 elementary stream for FFmpeg
inspection. It does not decode or display video by default. Add `--decode-h264` to feed reassembled
packets through the Microsoft H.264 decoder MFT and print decoded frame diagnostics. Add
`--dump-decoded-bmp PATH` to save the latest decoded NV12 frame as a BMP snapshot through the CPU
diagnostic converter. The raw `.h264` dump validates codec bytes and dimensions, but it does not
store transport timing. Add `--preview` on the receiver to open a native Win32/Direct3D preview
window; preview uploads decoded NV12 luma and chroma planes to GPU textures and converts to SDR
Rec.709 in the pixel shader. The preview title includes the current scale mode, and the window can
toggle fullscreen with F11 or Alt+Enter. Decoded preview frames pass through a small
timestamp-ordered playout buffer before presentation, so network and decoder bursts are smoothed
before they hit the window.
If the decoded stream changes resolution, the preview drops queued frames from the old size and
restarts its playout clock so adaptive-resolution tier switches do not carry stale timing into the
new size. Receiver stats report `preview_queue`, `preview_playout_resets`, `preview_late_drops`, and
`preview_overflow_drops` for that playout stage, along with `preview_latency_ms` and
`preview_max_late_ms` for the active playout settings. Receiver stats also include
`receiver_health=waiting|ok|loss|recovering|buffering|preview-drop`; when `--preview` is active,
the preview window title shows the same compact health summary plus decoded resolution, playout
latency, queue depths, reset count, and presented-frame count. The health label reflects the latest
reporting interval, while the numeric drop/resync counters remain cumulative for diagnostics and
sender-side adaptation deltas. While no UDP media is arriving, the receiver logs a single
`waiting_for_stream` line instead of repeating full zero-value stats every second; detailed stats
resume as soon as new packets arrive.
When `--seconds` is omitted the preview runs until the window closes.

Audio support is currently split between standalone WASAPI capture/send diagnostics and receiver
playback, with combined audio+video UDP streaming available through `--udp-send --audio-capture`.
Use `--list-audio-devices` to list active output and microphone endpoints. Use
`--audio-capture system --seconds 5` to capture the default output device through WASAPI loopback, or
`--audio-capture microphone --seconds 5` to capture the default microphone endpoint. Add
`--audio-device-id ID` with an id from the device list to select a specific endpoint. The diagnostic
prints the mix format, buffer size, packet/frame/byte counts, silence/discontinuity counters, and
peak/RMS levels. Add `--audio-send HOST:PORT` to transmit WASAPI packets over UDP with
application-level fragmentation. Audio is encoded as 48 kHz stereo Opus at 128 kbps by default;
add `--audio-codec raw` for uncompressed transport diagnostics. When `--audio-capture` is combined with the normal video
`--udp-send` path, the sender captures audio on a background thread and sends it through the same UDP
socket as video. Sender stats report `audio_capture_*`, `audio_udp_*`, `audio_capture_qpc`,
`audio_codec`, and the combined UDP pacing bitrate.
The receiver's `--udp-recv PORT` mode accepts both H.264 media packets and raw or Opus audio packets; for
audio it reports completed packet/frame/byte counts, pending or dropped incomplete packets,
discontinuity/timestamp counters, the latest audio format, and `audio_codec`. Add `--audio-playback`
to render received audio through the default Windows output endpoint with a packet-id-ordered jitter buffer.
Receiver stats report `audio_playback`, queued playback packets and milliseconds, rendered
packet/frame counts, playback drops/skips, latency-trim drops, render backpressure, and the latest
audio QPC timestamp. If the render endpoint falls behind, the receiver trims old queued audio rather
than letting live playback drift seconds behind the preview.
When both video and audio are present, receiver stats also report `av_sync`, `av_audio_ahead_ms`,
`av_audio_elapsed_ms`, and `av_video_elapsed_ms`. The preview title includes a compact `av +/-Nms`
field that estimates live playout skew after preview latency, queued audio, and WASAPI render padding.
The console fields remain useful diagnostics for relative drift between the received video timestamp
timeline and the WASAPI audio QPC timeline.
With `--preview --audio-playback`, the receiver waits until both audio and video sender-clock anchors
are known, then aligns the live start point by trimming leading audio packets or preview frames from
the stream that began earlier. If automatic A/V sync sees video but no audio packets arrive, it
continues video-only instead of letting the preview queue stall; the receiver reports
`av_sync_correction=video_only_no_audio` in that case. Audio rendering waits until the preview playout
clock has started, so decoder startup latency cannot let audio begin before video. During playback,
the receiver schedules audio packets against the same sender-clock preview playout timeline, so audio
that belongs to a future video timestamp waits before entering the WASAPI render buffer. Add
`--av-sync` explicitly to require full audio+video sync startup for diagnostic comparisons, or
`--no-av-sync` to compare raw receiver timing. Receiver stats report `av_sync_correction`,
`av_sync_start_qpc`, `av_sync_playback_start_qpc`, `av_sync_video_start_drops`, `av_sync_audio_start_drops`,
`av_playout_audio_ahead_ms`, `audio_playback_sync_waits`, `av_sync_preview_bias_ms`, and
`av_sync_audio_bias_ms`.

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
 -> default auto stream encoder with periodic keyframes, queued direct D3D11 hardware input, and software fallback
 -> UDP sender/receiver transport diagnostics with receiver feedback and opt-in live bitrate adaptation
 -> Media Foundation H.264 decode validation with keyframe-aware recovery
 -> paced native Direct3D receiver preview with GPU NV12 conversion
 -> standalone WASAPI audio capture diagnostics
 -> raw WASAPI audio UDP transport diagnostics
 -> default Opus audio compression with raw-audio diagnostics
 -> opt-in receiver WASAPI audio playback with a jitter buffer
 -> combined audio+video UDP streaming with A/V diagnostics
 -> receiver A/V sync drift diagnostics
 -> default receiver A/V sync correction for preview+audio playback
 -> future session setup and renderer optimizations
```
An app to share your screen with others
