# Testing

Build tests with the same native toolchain as the application; see [build](build.md).
Ordinary tests use generated media and injected input sinks. Keep physical
input, driver allocation and audible-output tests explicit.

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

The audio test checks ordered PCM blocks delivered in 50ms bursts, with no gaps
after startup. The video/input test forwards six complete key presses across two
busy GPU presents, so renderer frame drops cannot silently discard key events.

```powershell
cmake --build build/release --target NativePcmAudioTests VideoFrameInputTests NativeCaptureTests
ctest --test-dir build/release -R '^(native-pcm-audio|video-frame-input)$' --output-on-failure
```

On an interactive Windows desktop, `build/release/NativeCaptureTests.exe --cadence`
checks 30/60 FPS on its own stationary test window, including minimize/restore;
it saves no pixels. `build/release/NativePcmAudioTests.exe --wasapi` checks silent
native playback and process-only capture. Neither proves audible signal fidelity
or end-to-end behavior on a second computer.

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
