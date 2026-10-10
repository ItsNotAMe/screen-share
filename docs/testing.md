# Testing

Diagnostic coverage and report collection are described in [diagnostics](diagnostics.md).
`room-diagnostic-report` checks older decoder interface compatibility, bounded
history, default ICE configuration, cross-report correlation and redaction of
synthetic transport secrets. `room-v2-input-media` includes a real ICE restart
while other viewers continue streaming. These do not qualify Windows 7 or WAN NATs.

`update-session-deferral` exercises a verified local download while a room is
active, readiness after leaving, the new-room-before-click race, and closing
the update UI during a transfer. `room-v2-qt-ui` checks the shell's session
guard with the real room application lifecycle.

Build tests with the same native toolchain as the application; see [build](build.md).
Ordinary tests use generated media and injected input sinks. Keep physical
input, driver allocation and audible-output tests explicit.

Input service regressions cover accumulating requests, denial preserving existing
grants/held input, requests arriving during grant application and cancellation.
`desktop-input` uses generated HWNDs to check keyboard grants with mismatched or
missing capture-to-desktop bounds. Window mouse grants allow relative game input;
absolute events remain suppressed without exact bounds. Hidden
windows, removed identity markers and changed processes still revoke input.
`NativeCaptureTests --switch` includes a captured borderless window, minimized
keyboard grants and six source replacements; it requires an active desktop and
does not inject keys. These fixtures do not establish Silksong compatibility.
`video-frame-input` checks Qt's native E0-prefixed navigation/modifier scans through
both video event paths, mapping, protocol encoding/decoding and Windows input
payload construction. Dedicated arrows retain the extended flag; keypad scans
remain unprefixed. It does not inject physical input or qualify a specific game.
Game-input regressions cover relative wire payloads, accumulation instead of
replacement of motion samples, generation isolation, permission removal,
Escape/Tab routing and relative Windows payloads without absolute flags. The
recording desktop UI scenario forwards Escape press/release in fullscreen through
both video recipients, exits using Ctrl+Alt+Shift+F and releases control with
Ctrl+Alt+Shift+Q. These do not establish hardware mouse sampling or game delivery.
`VideoFrameInputTests --game-mouse` with `QT_QPA_PLATFORM=windows` also checks
native registration/confinement at the video centre (including resize), the consumed capture click, relative buttons and
wheel, a hidden cursor across grant updates, and unlocking on permission removal,
focus loss and stream clear. It creates a temporary window, confines the local
cursor briefly and records events without delivering input to a game.

For a two-PC game check, run the same updated build on host and viewer, create a
fresh room and share the game window. Grant Mouse and Keyboard while the host is
in ScreenShare: granting must preserve focus. The host then selects the game;
the viewer enables Game mouse and clicks the video. Check continuous camera
turns past screen edges, combined movement and camera input, buttons/wheel, Tab,
and Escape opening the game menu while viewer fullscreen remains active. Check
Ctrl+Alt+Shift+F for fullscreen and Ctrl+Alt+Shift+Q for release, then request/grant
again. Alt+Tab must release held input and unlock the viewer cursor; click the
video to recapture. Host focus loss pauses game input, and host revoke or
Ctrl+Alt+Shift+F12 must release held keys. Save reports on both PCs for any failure,
including the failed step and game display mode. This real connection check is
separate from recording-sink regressions.
Also leave and rejoin the same room several times, confirming the host stays
active and accepts a new viewer. If the room itself ends, restart sharing on the
host; an ended room cannot be repaired by rejoining from the viewer.
`CaptureSessionTest` in the WebRTC proof suite injects GPU frame deadline misses:
one miss must preserve capture and its generation; three consecutive misses use
bounded recovery, while cancellation and the lifetime retry budget still apply.
Native media reports distinguish `capture-gpu-frame-skipped` from a source error.
The native room UI scenarios check immediate icon requests, independent host menus,
distinct pending/granted states and preserving keyboard when requesting mouse.
Window keyboard regressions check direct grants and accepting requests.
The window keyboard UI fixture constructs the production runtime with window
capture selected at startup, adds keyboard to an existing mouse grant and records
delivery of a key. This covers runtime capability configuration as well as the UI.
Desktop input tests check background input suppression, releasing held keys during idle
health polling, retained ownership and resumption without replaying dropped input.
They also inspect the Windows key payload for arrows, right modifiers, keypad
keys and releases without calling SendInput. `capture-dispatcher-lifecycle`
checks replacement-source queue ownership and external queue borrowing.
The `desktop-input` UI scenario covers visible backend grant failures and checks
that host/viewer media failure sends Leave and promptly updates the room directory.

`room-v2-socket-reconnect` delays the server's socket-close notification and
forces two viewer reconnects plus a host reconnect using the real native room
runtime and local Worker. It checks replacement peer generations, fresh viewer
telemetry, restored control requests/grants/input delivery and continued media.
Viewer reconnects preserve the other viewers' peers. Capture, audio and input
are synthetic; the fault route exists only in the generated loopback fixture.

`room-peer-isolation` uses real native peers with synthetic video/audio. One
viewer receives media while a second completes SDP but has no usable ICE path.
It checks that the unconnected viewer never processes capture frames, the healthy
viewer continues receiving frames through the connection timeout/removal, and
both endpoints retain the timeout evidence after cleanup. It does not establish
real-WAN FPS or Windows 7 compatibility.

`room-peer-departure` exercises the host's real networking thread and signaling
coordinator after a viewer becomes reconnecting or leaves. Queued sends to that
viewer are discarded while another viewer's signaling continues without a room
transport error. The Worker `viewer-departure.test.mjs` covers disconnect, Leave,
kick, late ICE from retired connections, and joining afterward. Run it through
`npm test` in `signaling-worker`; these are local fixtures, not a deployed-service
or WAN qualification.

```powershell
cmake --build build/release --target RoomUiTests RoomUiWindowsTests
ctest --test-dir build/release --output-on-failure
```

CTest runs the registered suite; list it with `ctest --test-dir build/release -N`.
No fixed historical test count represents current coverage. Node.js/local Worker
dependencies are required for room integration tests. UI tests launch their own
isolated local service through `signaling-worker/tests/run-native-service.mjs`.

Focused examples from the repository root:

```powershell
ctest --test-dir build/release -R '^room-v2-qt-ui$' --output-on-failure
node signaling-worker/tests/run-native-service.mjs build/release/RoomUiTests.exe build/ui-input-evidence media desktop-input
node signaling-worker/tests/run-native-service.mjs build/release/RoomUiTests.exe build/ui-controller-evidence media controllers
node signaling-worker/tests/run-native-service.mjs build/release/RoomUiTests.exe build/ui-resolution-evidence media native-resolution
node signaling-worker/tests/run-native-service.mjs build/release/RoomUiWindowsTests.exe build/ui-native-evidence windows-media
```

The Windows suite needs an interactive desktop and validates capture plus native
D3D presentation. These input scenarios use recording sinks, not real OS input.
Optional `SCREENSHARE_UI_PREVIEWS` saves UI screenshots; use the offscreen suite
for deterministic popup positions. Windows may clamp popup positions to a screen
edge. Qt widget grabs do not include the native D3D video plane: use actual frame
presentation counters and native rendering checks, not a black grab, as evidence.

Audio cadence and retained-frame resize regressions run without physical input:

The PCM audio test checks ordered blocks delivered in 50ms bursts, with no gaps
after startup, and unity-level stereo content in 5.1/7.1 layouts with overload
protection. `SharedAudioLevelTests` sends synthetic audio through the shipping
MediaEngine and encrypted Opus transport at 100% viewer volume. It checks stereo
SDP in both directions, mono codec compatibility, received RMS levels and channel
separation for equal, left-only, right-only and opposite-phase signals. It opens
no physical audio devices. The video/input test forwards six complete key presses
across two busy GPU presents, so renderer frame drops cannot silently discard key events.

`ApplicationAudioTargetTests` checks rendering-helper selection, multiple audio
sessions, unrelated apps, stale/recycled parent PIDs, helper restarts during a PCM
read, playback starting after sharing, and silence after the window owner exits.
Its default run opens no audio devices. On an interactive Windows desktop, run
`build/release/ApplicationAudioTargetTests.exe --app PID` while that app plays
audio to check the actual discovery and shared-source PCM capture path. It prints
the selected process and levels, discards samples, and saves or replays no audio.

```powershell
cmake --build build/release --target NativePcmAudioTests SharedAudioLevelTests VideoFrameInputTests NativeCaptureTests
ctest --test-dir build/release -R '^(native-pcm-audio|shared-audio-level|video-frame-input)$' --output-on-failure
```

On an interactive Windows desktop, `build/release/NativeCaptureTests.exe --cadence`
checks 30/60 FPS on its own stationary test window, including minimize/restore;
it saves no pixels. `build/release/NativePcmAudioTests.exe --wasapi` checks silent
native playback and process-only capture. Neither proves audible signal fidelity
or end-to-end behavior on a second computer.
`build/release/NativeCaptureTests.exe --switch` checks six generated-window source
replacements, continued frames/input geometry, and keyboard grants while minimized
past the normal target freshness timeout. It does not inject keyboard input.

`WindowChromeTests` checks native caption-button hit testing in normal, maximized,
and restored windows. Build that target, then run it on a Windows desktop with
`QT_SCALE_FACTOR` set to `1`, `1.5`, and `2` in separate processes. These overrides
apply only to the test process; they do not change Windows display settings.

The wider runners remain in `scripts/test-headless-media.py`,
`scripts/test-room-regression.py`, `scripts/test-room-hardware-load.py` and
`scripts/test-room-impairment.py`; use `--help` for current fixture/options schemas.
Use the matching evidence validators in `tests` when changing those tools.

`run-webrtc-proof.ps1 -Hardware` opts into GPU tests; `-LiveCapture` opts into
capture fixtures. Its `-AudioDevice` mode requires `-AllowAudibleTests`: never
assume a device test is silent merely because it is unattended.

Physical-reader tests and native virtual-driver allocation are separate opt-ins.
Do not rerun native allocation until the known leftover virtual-device state is
resolved; see [known limitations](known-limitations.md). Test success with recording
sinks does not establish physical controller, HDR, NAT or external A/V latency.

Historical investigation reports are available in Git at commit `376d59a`.
They are not current acceptance checklists. Deferred work remains deferred unless
a reported regression or new request makes it relevant.

## Offline setup regression

`python tests/NativeDependencySetupTests.py` checks bootstrap orchestration with
small mocked installers: no WebRTC downloads, C++ builds, devices or network are
used. It covers cache reuse, separate configurations, interruption recovery,
custom paths, missing/corrupt inputs and CMake's pre-compiler setup. Set
`CMAKE_COMMAND` to native Visual Studio CMake if the PATH points at MSYS.
These checks do not establish that a real dependency download/build succeeded.
