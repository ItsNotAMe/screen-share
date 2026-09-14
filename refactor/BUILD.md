# Native dependency and application build

The working branch is `refactor/backend-v2`. Gate A remains open: the new builds work, but normal application media is still routed through the existing backend.

## Current verified SDK and portable workflow

Python 3.11+ is required for SDK export and verification. Export happens separately from application configuration and uses no network. Run `build-webrtc.ps1 -SkipHooks` for each configuration first so metadata includes the selected MSVC toolset and Windows SDK.

```powershell
python scripts/webrtc-sdk.py export build/webrtc/checkout/src/out/screenshare-release build/webrtc-sdk-cache
python scripts/webrtc-sdk.py export build/webrtc/checkout/src/out/screenshare-debug build/webrtc-sdk-cache
python scripts/webrtc-sdk.py verify <sdk-directory>
python tests/WebRtcSdkTests.py
./scripts/run-webrtc-proof.ps1 -Configuration release -Hardware -AudioDevice -ArtifactDirectory <sdk-directory> -BuildDirectory build/sdk-proof-release
./scripts/run-webrtc-proof.ps1 -Application -Configuration release -ArtifactDirectory <sdk-directory> -BuildDirectory build/sdk-app-release -ViGEmSourceDirectory <existing-vigem-source> -Package
```

Paths may be absolute or relative to the repository. The existing local ViGEm source is `build/native-release/_deps/screenshare_vigemclient_source-src`; supplying it prevents a fresh application build from attempting a download. Qt, Vulkan headers, the exact Clang binary, MSVC and Windows SDK remain separately installed prerequisites; the SDK bundles WebRTC headers/libraries, not those toolchains.

SDK directories are named by a SHA-256 identity over metadata and a complete file-hash inventory. Export refuses to replace an existing identity and verifies it instead. Configuration verifies the inventory (including unexpected files), ABI, compiler hash, MSVC/SDK versions, GN arguments and build patch. These checks detect changed local artifacts; they are not a signature/authentication mechanism for untrusted downloads. `--resume <export-directory-name>` resumes an interrupted staging directory under the cache; files are rechecked against source before reuse. Staging is preserved after failures.

Verified exports each contain 40,835 files. Release identity: `1046901babf6f5dfb2c2284b929e0a189a8735c398cf307c7a5b8cda69f2f92a` (moved to `build/sdk-relocation/Release` for the relocation proof). Debug identity: `f17c9176316e5d40ac0accdbda1c18cf6a79e34f259827e93379672277888027` under `build/webrtc-sdk-cache`.

Current validation: 14/14 hardware/audio proof tests and 9/9 application tests per configuration. The relocated Release SDK also passes 14/14. The native portable zip is `build/sdk-app-release/ScreenShare-release-windows-x64.zip`. Extracted CLI `--help`, UI `--self-test` (offscreen) and `--gui-smoke-test` (Windows) pass with PATH reduced to Windows system directories and no QT_PLUGIN_PATH. Plugin logs confirm loading from the extracted package. Native packages now require WebRTC notices, stage the matching MSVC runtime, choose native Qt deployment tools/plugins, and reject unresolved dependencies.

This validates local portable staging/startup, not a release: installer/fresh-machine checks, remaining distribution obligations (including Qt), real device loss and latency evidence remain open. The older chronological build notes below describe the initial local-artifact phase; use this section for current commands/status.

## Prerequisites and pinned inputs

Use Windows x64, Visual Studio C++ Build Tools with its CMake tools component, Python and Git. The verified installation is VS Build Tools 2026 18.4.11620.152 / MSVC 14.50.35717. Windows SDK 10.0.28000.2526 was installed; its include/library directory is 10.0.28000.0. The prior 22621/26100 SDKs remain installed.

`webrtc-source.json` pins WebRTC, depot_tools, Chromium build/buildtools and Clang. WebRTC's pinned DEPS supplies the remaining dependencies. `native-dependencies.json` pins Qt 6.10.3 MSVC x64, the Qt download helper and portable Vulkan headers. SVG is included in Qt's base package; WebSockets is the extra module.

Run from the repository root:

```powershell
./scripts/sync-webrtc.ps1
./scripts/install-native-deps.ps1
./scripts/build-webrtc.ps1 -Configuration Debug
./scripts/build-webrtc.ps1 -Configuration Release -SkipHooks
./scripts/run-webrtc-proof.ps1 -Configuration debug
./scripts/run-webrtc-proof.ps1 -Configuration release
./scripts/run-webrtc-proof.ps1 -Configuration debug -Application
./scripts/run-webrtc-proof.ps1 -Configuration release -Application
```

Only use `-SkipHooks` after a successful hook run for this checkout. SDK installation is separate; `build-webrtc.ps1` reports the missing SDK before attempting compilation. Download/build tools live under ignored `build/`; application configuration does not fetch WebRTC.

The runner imports the Visual Studio developer environment for its own process and restores it afterward. It explicitly uses Visual Studio's native CMake and DEPS-pinned Windows Ninja. Do not substitute MSYS Ninja/CMake: Ninja invokes a POSIX shell, and that CMake distribution lacks the native RC dependency wrapper required here.

## Outputs and offline use

- WebRTC: `build/webrtc/checkout/src/out/screenshare-{debug,release}/obj/webrtc.lib`.
- Artifact metadata: `screenshare-artifact.json` beside each output's `args.gn`.
- Probes: `build/webrtc-proof/{debug,release}/WebRTCProof.exe` and `MfCodecProbe.exe`.
- Application: `build/native-{debug,release}/ScreenShare.exe`, `ScreenShareUi.exe`, `ScreenShareUpdater.exe` and staged runtime files.

The root `native-debug` and `native-release` presets are available; original default presets remain for baseline/packaging comparison until native delivery is validated. The native presets currently disable automatic portable packaging and LTO. Do not describe the native portable/installer release as validated.

`cmake/WebRTC.cmake` imports `ScreenShare::WebRTC` using `SCREENSHARE_WEBRTC_ARTIFACT_DIR`. A local artifact works offline while its matching source checkout and generated headers remain available. This is currently a local build artifact, not a relocatable SDK. Configuration checks the source revision, architecture, configuration, CRT, standard library, compiler checksum, library checksum, GN argument checksum and build-patch checksum. A Release artifact was explicitly tested and rejected by a Debug consumer.

Immutable cache packaging keyed by the complete dependency/compiler/SDK identity, and dependency-notice bundling, remain TODO. Do not mark those complete based on the metadata file alone.

## ABI choices and source patch

Both configurations use x64 clang-cl, C++20, MSVC STL, dynamic CRT (`/MDd` Debug, `/MD` Release), exceptions and RTTI. Debug iterator checking is enabled to match ordinary MSVC Qt builds.

The selected Chromium build defaults to a static CRT and disables STL exceptions. `webrtc-build.patch` changes exactly those defaults so WebRTC can coexist with Qt. The patch is checked before applying and its checksum is recorded in each artifact. It does not change congestion control or media algorithms.

The actual GN arguments are generated by `build-webrtc.ps1`, validated by `gn gen --fail-on-unused-args` and stored with the build. Standalone MSVC STL is supported by this pinned source through `use_custom_libcxx=false`; do not infer support for future WebRTC revisions.

The application temporarily uses Opus from the complete verified WebRTC archive. Chromium's vendored Opus checkout omits makefile inputs required by its standalone CMake build. The native application must not link MinGW's Opus binary. The v2 media implementation will feed PCM through the ADM rather than the legacy Opus adapter.

Qt's Vulkan header lookup must use `build/Vulkan-Headers/include`. Letting it find `C:/msys64/ucrt64/include` brings MinGW CRT headers into the MSVC compilation. No Vulkan renderer is being introduced.

## What the probes establish

`WebRTCProof` creates two local full PeerConnections, transmits synthetic H.264 through the MF software encoder/decoder factories, and requires at least 60 received video frames with correct dimensions/pixels. It also transfers a message over each of three DTLS data channels. The control channel is reliable/ordered; input-state and telemetry are unordered with zero retransmissions. Opus audio is transmitted through the PCM ADM and the receive endpoint must observe at least 30 audible blocks alongside video. This diagnostic uses SDP-bundled local candidates and no server. Production signaling still needs targeted trickle ICE, generations and bounded queues.

`MfEncoderAdapterTest` tests zero-rate/resume, keyframes, timestamp preservation, deterministic 100-input burst coalescing and three reset/release cycles. `MfDecoderAdapterTest` tests four cycles including 1920×1080 cropping, RTP wrap, NTP metadata and callback-thread ownership. Both are included in the default seven-test Debug/Release proof suite. Implementation contracts and limitations are in `src/media/webrtc/README.md`.

The upstream `webrtc_lib_link_test` intentionally attempts creation with a null observer and prints `creation=failed`; its exit success only establishes symbol/link coverage. Our independent probe requires successful connections and received messages.

`MfCodecProbe` feeds synthetic 640×360 NV12 frames to the existing encoder/decoder, requests a keyframe, updates bitrate and repeats initialization/teardown. The software mode is in CTest. Run `MfCodecProbe.exe --hardware` separately for hardware/D3D11 input. It retains device/texture references and validates decoded visible dimensions. This tests existing Windows primitives, not their WebRTC adapters, zero-rate suspension, asynchronous submitted-frame age or GPU presentation.

Use `./scripts/run-webrtc-proof.ps1 -Configuration debug -Hardware` (or `release`) to enable the actual-GPU tests. This builds/runs ten tests, including `WebRTCProof --hardware`, `MfEncoderAdapterTest --hardware` and `MfHardwareAdapterTest`. Without `-Hardware`, seven tests run and the GPU-dependent tests remain unrun. The hardware proof requires a D3D11/MF-capable GPU and explicitly fails if it silently falls back.

Hardware tests verify GPU input with zero sender readbacks, rate/zero-rate/keyframe behavior, 100-input burst coalescing, a 500 ms missing-output failure followed by software IDR recovery, session quarantine, cancellation, retained textures and concurrent cached readback. The GPU source is synthetic NV12 upload; live capture and GPU presentation are covered by the separate -LiveCapture tests below.

The probes do not establish real-game latency, multi-viewer isolation, NAT connectivity, full lifecycle safety, preemption of a hung driver call or performance on another machine.

## Audio device proof

Use `./scripts/run-webrtc-proof.ps1 -Configuration debug -Hardware -AudioDevice`
(and `release`) for all twelve tests. `-AudioDevice` plays a brief quiet tone
through the default output and captures only the test process. It adds physical
WASAPI mono/stereo/mute/lifecycle and invalid-device checks, plus process-loopback
capture through Opus and a receive-side PCM sink alongside H.264. Received audio
is measured without replay to prevent feedback. The ordinary suite uses synthetic
audio and needs no audio hardware. Other capture modes, microphone processing,
full-session device recovery and actual A/V latency remain separate work.

## Live capture proof

Add `-LiveCapture` to include `wgc-owned-hardware-capture` and `webrtc-live-wgc-h264`
(fifteen tests with all device options). Each creates a generated window and captures only that
window. WGC requires interactive desktop and capture-service access; restricted
execution can fail with "The specified service does not exist as an installed
service". After building, run `build/webrtc-proof/{debug,release}/LiveCaptureTest.exe --repeat`
from a normal desktop terminal. CTest imposes a 45-second timeout. This proves
owned GPU capture into the hardware encoder adapter, fixed-size resize, window
closure and retained pixels across three sessions in one process.
`WebRTCProof.exe --live-capture` proves WGC through local hardware H.264
PeerConnections alongside Opus/data channels. Neither establishes external
source-process-exit safety, device recovery or gaming latency.

The capture translation unit uses clang-cl `/clang:-mcx16` for the Windows SDK's
128-bit interlocked factory-cache clearing operation. Without it, the pinned
compiler emits an unresolved `__atomic_compare_exchange_16` helper. The change
does not alter the WebRTC artifact ABI or require rebuilding that dependency.

The live PeerConnection test now includes a receiver GPU window with a verified
one-frame DXGI queue limit, nonblocking presentation and resize. Keep the generated
receiver window unobscured: it checks neutral decoded chroma and samples its own
center/letterbox pixels from the composed desktop. Desktop color transforms may
change RGB. Presentation still uploads CPU-decoded NV12; no GPU-decode or zero-copy
claim. `presentation-latest-frame` is a device-independent queue/ownership test.

`-LiveCapture` also enables `wgc-source-process-exit`. Run
`CaptureProcessExitTest.exe` to create, capture and terminate a generated child
source three times. Children belong to a kill-on-close job so a proof timeout
cannot leave source processes running. `LiveCaptureTest --repeat` now also covers
minimize/restore and permanent closure after a replacement window is created.
Actual forced HWND reuse, device removal and automatic recovery remain separate.

Receiver presentation recovery proof: run `build/webrtc-proof/<configuration>/PresentationRecoveryTest.exe --gpu` in a desktop session. This creates only a diagnostic window and injects a failure at the rendering boundary; it does not reset the desktop GPU. The default proof suite now has 13 tests with `-Hardware -AudioDevice`; `-LiveCapture` also registers this desktop-dependent recovery test.

Automatic capture recovery: `CaptureRecoveryTest --live` exercises three replacement generations and terminal exhaustion on a generated window. `CaptureRecoveryTest` alone tests retry/generation policy. Hardware/audio suites now contain 14 tests per configuration. The live variant is included by `-LiveCapture` and requires a desktop session.
# Capture lifecycle stress diagnostics

Run from an interactive Windows desktop after building the native proof:

```powershell
python scripts/stress-live-capture.py build/sdk-proof-release/LiveCaptureTest.exe build/webrtc/capture-stress-new --cycles 100 --timeout 1200
python scripts/stress-live-capture.py build/sdk-proof-debug/LiveCaptureTest.exe build/webrtc/capture-only-new --cycles 100 --capture-only --timeout 300
python scripts/stress-live-capture.py build/sdk-proof-debug/LiveCaptureTest.exe build/webrtc/capture-close-regression-new --cycles 1 --capture-only --close-source-first --timeout 15
python tests/LiveCaptureStressTests.py
```

Each output directory must be new. The runner records separate stdout/stderr,
binary SHA-256, numeric/hex exit code, deadline outcome and every completed-cycle
resource sample in `result.json`. It kills only its own proof child on timeout.
Interrupted or malformed final samples are preserved as failed evidence.
The generated source windows are the only capture targets.

The full mode exercises owned WGC capture, minimize/restore, resize, hardware
encoding, device reconstruction, software fallback and permanent source closure.
Capture-only stops while the source is still open and omits encoder/device-recovery work for isolating resource growth;
it is not a replacement for the full proof. Both print markers after resource
destruction and private-memory/working-set/handle/GDI/USER counts per cycle.
These samples include OS/driver caches; successful cycles alone do not prove
absence of leaks or completion of the production session lifecycle gate.
The `--close-source-first` variant retains a known rapid-shutdown hang and is
expected to fail by watchdog on the current reference machine; do not hide it in
the passing capture-only result or run it without a timeout.

The Windows capture adapter now pins the system `GraphicsCapture.dll` once per
process to mitigate the reproduced callback into unloaded module code during
teardown. Session/pool/device destruction and balanced COM initialization remain
in place. The module pin intentionally remains until process exit. See the latest
`CHECKPOINT-A.md` evidence before removing this workaround; timing sleeps are not
a substitute for module lifetime.
