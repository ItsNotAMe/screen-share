# Checkpoint A evidence

Status: in progress. Build/API inventory is not media integration proof.

## Hardware MF and owned GPU frames — 2026-09-14

The WebRTC encoder now uses an explicit MF hardware submission API: one submitted
frame and one replaceable pending frame, with nonblocking event polling on its
MTA worker. A 500 ms missing-output deadline triggers same-size software IDR
recovery and session/device quarantine. Generation cancellation interrupts the
wait without quarantining a healthy implementation. The legacy MF queue/drain
path is not used by WebRTC.

`D3dVideoFrameBuffer` retains its own NV12 texture/device owner. Concurrent CPU
fallback reads are serialized and cached; published textures are never reused.
Hardware receives same-device textures without readback. Synthetic capture now
runs on its own thread, respecting WebRTC's prohibition on arbitrary blocking
calls from the signaling thread. An initial hardware PeerConnection run exposed
that thread-affinity assertion; it was fixed before recording the passing runs.

Validation commands:

```powershell
./scripts/run-webrtc-proof.ps1 -Configuration debug -Hardware
./scripts/run-webrtc-proof.ps1 -Configuration release -Hardware
./scripts/run-webrtc-proof.ps1 -Application -Configuration debug
./scripts/run-webrtc-proof.ps1 -Application -Configuration release
```

- Hardware-enabled Debug/Release suites: **8/8 passed each**. Logs:
  `build/webrtc/hardware-adapter-debug.log`, `hardware-adapter-release.log`.
- Native Debug/Release application regression suites: **8/8 passed each**.
  Logs: `build/native-debug-hardware-adapter.log`,
  `build/native-release-hardware-adapter.log`.
- Hardware PeerConnections: 60 decoded 640×360 frames, 61 hardware encodes,
  zero sender GPU readbacks. Observed maximum local hardware-frame time was
  approximately 15 ms in these short runs; this excludes capture/display and is
  not an end-to-end latency measurement. Detailed output is in each proof build's
  `Testing/Temporary/LastTest.log`.
- Hardware lifecycle: three reset/release cycles, zero-rate/FPS-zero suspension,
  reduced-rate resume, forced keyframes/SPS level validation, and 100-input bursts
  replacing 99 pending frames without replay.
- Missing-output injection: real events are drained but output is withheld;
  after 500 ms the adapter emits software IDR recovery with the original RTP
  timestamp. Reset does not retry quarantined hardware.
- Cancellation during output wait returns within the test's 300 ms bound,
  produces no retired callback and does not quarantine the device.
- GPU ownership: a newer upload does not mutate a retained older frame;
  concurrent readbacks return the same cached I420 buffer; release of the last
  frame releases its otherwise-unreferenced device owner/thread.

Limits: the GPU producer uploads synthetic NV12, not live WGC/DXGI resources.
Live capture synchronization, GPU presentation, other devices/vendors, actual
1080p60 gaming throughput/latency, WASAPI/Opus transmission and soak tests remain.
The deadline handles asynchronous missing output; it cannot preempt an MF/D3D
driver call that never returns. Normal application routing is unchanged and
Gate A remains open. Raw WebRTC connection logging is disabled in the proof to
avoid saving ICE credentials/addresses; explicit test results remain available.

## Software MF codec integration — 2026-09-14

Added private factories under `src/media/webrtc`, with dedicated COM workers,
generation-scoped encoder jobs and a single replaceable pending raw frame. Real
PeerConnections now transmit synthetic H.264 through MF at both ends. The CPU
fallback is explicit; there is no hardware/GPU claim.

- Debug and Release proof suites: **4/4 passed** each, logs
  `build/webrtc/mf-adapter-debug.log` and `mf-adapter-release.log`.
- Local RTP/SRTP video test receives at least 60 correct 640×360 frames, plus all
  three data-channel messages; approximately 2.3 seconds total test duration.
- Encoder tests: zero bitrate (including zero FPS field), 400 kbps resume, forced
  keyframe, timestamp preservation, three reset/release cycles. A blocked callback
  makes the 100-input overload deterministic: only the newest pending frame is
  encoded; 99 replaced frames are reported dropped each cycle.
- Decoder tests: four cycles at 640×360/1920×1080, 44 delivered of 48 submitted
  High-profile frames. One delayed frame per cycle is discarded on Release, not
  delivered to a later session. RTP wrap, NTP and callback thread are checked.
- Native Debug application rebuild and regression tests: **8/8 passed**,
  `build/native-debug-mf-adapter.log`.
- Native Release application rebuild and regression tests: **8/8 passed**,
  `build/native-release-mf-adapter.log`.

MF initially enumerates a default output type larger than small input; applying
the negotiated dimensions to that initial enumeration caused a test failure.
The final implementation bounds coded allocation globally and checks actual
visible output against negotiated dimensions. The decoder's delay also required
the test to associate callbacks with submitted timestamps rather than assume
every Decode call returns its own frame immediately.

The software adapter currently supports up to 1080p60, CPU conversion and a
post-call 500 ms failure check. It cannot interrupt a hung transform. Hardware
event/submission ownership, watchdog/fallback/quarantine, GPU lifetime and WASAPI
ADM/Opus transmission remain required before Gate A. Normal app routing remains
legacy. See `src/media/webrtc/README.md` for exact ownership contracts.

## Current results — native build and primitive probes

Work is on `refactor/backend-v2`. At the user's request, unnecessary uncommitted UDP adaptation/recovery edits were archived to `build/pre-v2-legacy-edits` (patch plus six source/test files), then removed. Approved planning and baseline tooling were retained.

See [BUILD.md](BUILD.md) for pinned versions, ABI decisions, exact commands and limitations.

| Validation | Result | Evidence |
|---|---|---|
| Restored baseline CLI build and CTest | Passed, 8/8 tests | Existing debug build after removing the two legacy policy tests |
| Debug WebRTC archive | Built and linked | `build/webrtc/debug-build.log` and output `screenshare-artifact.json` |
| Release WebRTC archive | Built and linked | `build/webrtc/release-build.log` and output `screenshare-artifact.json` |
| Debug direct data channels + MF software probe | Passed, 2/2 tests | `build/webrtc/proof-debug-codecs.log` |
| Release direct data channels + MF software probe | Passed, 2/2 tests | `build/webrtc/proof-release.log` |
| MF hardware, retained D3D11 input | Passed two 120-frame cycles; 120 decoded and two keyframes each; no pending input queue | `build/webrtc/mf-hardware.log` |
| Full native Debug application | Built CLI/UI/updater/controller client; 8/8 tests | `build/native-debug-build.log` |
| Full native Release application | Built CLI/UI/updater/controller client; 8/8 tests | `build/native-release-build.log` |
| Release UI `--self-test` | Exit 0, offscreen | `build/native-ui-self-test.txt` / `build/native-ui-self-test-error.txt` |
| Release artifact with Debug consumer | Rejected for configuration mismatch as intended | `build/webrtc/artifact-rejection.log` |

Hardware primitive probe maximum encode-call durations were 1.26 ms and 0.30 ms in the two short 640×360 cycles. These are not frame-age, network or gaming latency measurements.

Failures investigated and resolved: depot_tools needed its Windows bootstrap before disabling self-update; SDK 28000 was missing; MSYS Ninja/CMake were unsuitable for native compiler/resource commands; the data-channel probe lacked Winsock initialization; Chromium's Opus copy lacks standalone CMake inputs; Qt's automatic Vulkan lookup included MinGW CRT headers; the updater lacked `<cwctype>`; MF's encoder CLSID requires `wmcodecdspuuid`; the offscreen Qt test needed its plugin path. None was recorded as a passed media gate.

## Restored-backend loopback measurement

`build/baseline/legacy-loopback-20260914` contains invocation arguments, process exit/CPU data and sender/receiver logs. Source was display 0, 2560×1440 HDR converted to 1920×1080 SDR, target 60 FPS / 12 Mbps, software encoding with bitrate/resolution adaptation. Sender ran 20 seconds, receiver 25 seconds, local UDP decode only. No audio or presentation. Dependency hooks were running concurrently.

Both exited 0. All 1,200 submitted frames were decoded; no incomplete drops, decoder resyncs or skipped packets. Sampled sender queue was zero. Encoder timing was approximately 4 ms per frame; final mean decoder timing was 3.328 ms. Process CPU totals were 12.422 seconds sender / 8.938 seconds receiver. The desktop was mostly unchanged, so this is not a gaming workload or upload-quality benchmark. The receiver's approximately 47.8 FPS whole-run average includes its extra five idle seconds.

Still missing: measured GPU utilization, presented-frame age, A/V behavior, external end-to-end latency, a viewer machine and representative moving/game content.

## Starting baseline — 2026-09-14

- Commit: `e8c82ae47d64aa19a9cc0983adbc55af307b59fb`.
- Preserved pre-existing CMake/runtime changes and untracked AdaptiveStreamPolicy/StreamRecoveryPolicy modules and tests. They extract adaptation helpers and accelerate decode-gap recovery; they are not v2 changes.
- `cmake --build --preset debug --target ScreenShare AdaptiveStreamPolicyTests StreamRecoveryPolicyTests -j 4`: passed.
- `ctest --test-dir build/debug --output-on-failure`: 10/10 passed. This uses the existing debug build; it does not establish a clean new-toolchain build.
- Added `scripts/collect-backend-baseline.ps1`: records source commit, changed-file hashes, executable hash, selected hardware inventory, scenario descriptions and optional CTest output. Missing inventory is explicitly reported. Existing evidence directories cannot be overwritten.
- Local evidence: `build/baseline/20260914-192244-855/manifest.json` and `ctest.txt`. Earlier sandbox-only inventory explicitly records access-denied errors; the subsequent elevated read succeeded.
- Host: Windows 11 Pro build 26200, Ryzen 7 9800X3D, 16 logical CPUs, approximately 61.6 GiB RAM. RTX 5070 Ti driver 32.0.16.1062; Radeon integrated graphics driver 32.0.21045.5002; a virtual display adapter is also installed. Actual capture/encode adapter remains to be measured.
- Visual Studio Build Tools 2026 18.4.11620.152 is installed. VS 2022 installation is incomplete. PATH currently selects MSYS2 tools; compatible SDK/compiler/Qt availability still needs validation.
- No streaming benchmark, viewer machine, network conditions, end-to-end latency, GPU utilization or audio measurement recorded yet.

## Source/API audit

`webrtc-source.json` records the exact official source selected via `git ls-remote`. It is not yet a complete artifact/toolchain lock.

Downloaded exact-revision `DEPS`, `webrtc.gni`, encoder/decoder/frame-buffer and audio-device headers to `build/webrtc-api-audit/` for inspection.

- `api/video_codecs/video_encoder.h`: InitEncode, RegisterEncodeCompleteCallback, Release, Encode and SetRates are present; encoder information includes native-handle and hardware-acceleration capabilities. Implement against these signatures rather than older examples.
- `api/video/video_frame_buffer.h`: kNative, ToI420 and GetMappedFrameBuffer are present. GPU ownership and synchronization remain adapter responsibilities; these APIs alone do not establish zero-copy delivery.
- `webrtc.gni`: rtc_build_examples, rtc_include_tests, rtc_enable_sctp and rtc_use_h264 exist. Their dependencies and final effective GN values still require validation; do not treat this list as approved build arguments.
- DEPS identifies a Windows Clang artifact `clang-llvmorg-24-init-7747-g62397f8b-27.tar.xz`. Installed clang-cl is not automatically interchangeable with this pinned compiler.

## Resume

Next: implement device recovery and forced source-handle reuse stress coverage; source lifecycle and GPU presentation evidence is recorded below. The reproduced WGC shutdown and restart regressions now pass; live capture through PeerConnections is proven below. Finish broader audio mode/recovery validation, artifact cache/notices/packaging and protocol fixtures. Only then evaluate Gate A for wider media routing migration. Preserve all unchecked tasks in TODO.md.

To collect another baseline without overwriting evidence:

```powershell
./scripts/collect-backend-baseline.ps1 -RunTests -Scenario inventory-only
```

For an actual stream benchmark, supply SourceDescription, StreamSettings and NetworkConditions and retain correlated sender/viewer reports. The collector does not launch capture or claim to measure latency.

## WASAPI / PCM ADM / Opus evidence — 2026-09-14

- Added portable 48 kHz / 10 ms PCM endpoint contracts, Windows endpoints and a
  WebRTC ADM. Capture and render own their COM resources on separate workers.
  Capture handoff caps at 30 ms; mute/volume, explicit device failures, bounded
  event waits, stop/join and callback detachment are implemented.
- Full Debug/Release proof commands with `-Hardware -AudioDevice`: 11/11 each.
  Logs: `build/webrtc/audio-full-debug.log`, `audio-full-release.log`.
  Includes H.264/Opus/three DTLS channels, real process-loopback through Opus,
  physical render/capture lifecycle, mono/stereo, capture mute, three teardown
  cycles, no callbacks after stop, and invalid explicit-device failures.
- Native application Debug/Release regression suites: 8/8 each. Logs:
  `build/native-debug-audio-adapter.log`, `build/native-release-audio-adapter.log`.
- Physical default output: 1122 frames capacity at 48 kHz (23.375 ms), actual
  shared engine period 10000 us. A 10 ms PCM block is not a 10 ms device buffer
  or end-to-end latency guarantee. Capture delay is explicitly estimated.
- Fixed pinned AudioTransport sample-count semantics: the playout request uses
  per-channel frames; the returned count includes all interleaved samples.
  Initial combined synthetic proof failed before this correction. The initial
  physical-to-Opus tone was below the receive detector threshold; increasing
  the generated test signal passed with the same detector and media code.
- Physical-to-Opus proof captures only its own generated source; received audio
  is measured without replay to prevent feedback. Physical playout is validated
  separately. No microphone or unrelated process audio was captured.
- Still pending: live capture/GPU presentation, microphone processing, consistent
  multichannel policy, other audio modes/devices, device recovery, silence/video
  independence scenarios, actual A/V timing, delivery/cache/notices and remaining
  Gate A evidence. The normal application still uses the legacy backend.

## Owned live capture — 2026-09-14

- Added opt-in immutable NV12 snapshots to DesktopCapturer. One reusable GPU
  conversion target feeds unique snapshots; a GPU event query establishes
  completion before publication. Polling has a 50 ms failure bound, but cannot
  interrupt a hung D3D/Windows API call. The encoder receives GPU pixels without
  CPU readback. D3dVideoDevice retains the selected capture device and rejects
  borrowed/incomplete/wrong-device frames at its capture-import boundary.
- Capture protects complete shader-state sequences on the shared immediate
  context. Resize closes/discards the old WGC frame before pool recreation;
  output dimensions stay fixed. Frames, frame pool and session close explicitly.
- Added LiveCaptureTest and runner -LiveCapture option. It creates only its own
  window, submits 80 WGC frames through the hardware WebRTC encoder adapter,
  resizes/closes the window and compares retained pixels after later writes and
  stop. Explicit validation readbacks are separated from encoder readbacks.
- Debug/Release ordinary hardware/audio proof suites remain 11/11 each:
  build/webrtc/live-adapter-debug-final.log and live-adapter-release.log.
  Native app regressions remain 8/8 each: build/native-{debug,release}-live-capture.log.
- Actual WGC requires capture-service access outside the restricted process.
  Initial restricted run failed CreateForWindow with service-unavailable; this
  was not counted as success. Normal-desktop Debug/Release individual runs passed
  with 77 hardware output frames and zero encoder readbacks. Evidence:
  build/webrtc/live-capture-{debug,release}-device.log and corresponding errors logs.
- Repetition exposed an UNRESOLVED intermittent shutdown hang. Debug cycles 1–3
  and Release cycle 1 passed; Release cycle 2 exceeded a 45-second watchdog.
  Logs: build/webrtc/live-capture-*-cycle-*.log. An earlier hung Debug stack is in
  build/webrtc/live-capture-debug-stacks.log: DesktopCapturer::Stop -> WGC session
  destruction/Close -> synchronous Windows RPC. Explicit Close improved some runs
  but did not establish a fix. A diagnostic variant keeping the source event
  loop alive also timed out (live-window-debug-cycle-1); it was reverted.
- The live test remains optional and may fail/time out. Do not report 12/12,
  complete capture shutdown, or Gate A success. Preserve this regression test.
  New stage diagnostics distinguish source close, capture stop and retained-pixel
  validation. The application still uses the legacy backend.
- Remaining: resolve WGC close lifecycle, source-process exit/minimize/handle
  reuse and device loss, live PeerConnection routing, GPU presentation, capture
  timestamps and actual performance. No cross-machine/gaming-latency claim.

## WGC lifecycle corrections and live PeerConnections — 2026-09-14

This supersedes the unresolved status in the previous capture entry, retaining
those failed runs as history. Gate A still remains open.

- Shutdown now returns up to two queued WGC frames, flushes the GPU context,
  closes the session and then closes the frame pool. Six generated-window runs
  per configuration passed after this change, including source-thread exit,
  retained pixel comparison and zero encoder readbacks. Logs:
  build/webrtc/wgc-stop-{debug,release}-{1..6}.log and corresponding errors logs.
- Three cycles in the same process then exposed a second failure: stale cached
  GraphicsCaptureItem activation factory after RoUninitialize unloaded the DLL.
  Debugger evidence: build/webrtc/wgc-repeat-stack.log. Clear the C++/WinRT
  factory cache before balancing capture's RoInitialize. The SDK's 128-bit
  interlocked cache operation requires /clang:-mcx16 on this translation unit;
  otherwise linking fails with __atomic_compare_exchange_16. Both application
  and proof CMake builds now supply it for clang-cl.
- Final LiveCaptureTest --repeat passes three full cycles in each configuration;
  outputs contain 76–78 hardware frames per cycle. Logs:
  build/webrtc/wgc-repeat-{debug,release}.log and corresponding errors logs.
- Added a capture-worker source and shared generated-window helper in the proof.
  WebRTCProof --live-capture sends actual WGC GPU frames through hardware H.264,
  local RTP/SRTP and MF decoding, alongside synthetic Opus and three data channels.
  Debug/Release receive at least 60 video frames with zero sender readbacks.
  Logs: build/webrtc/live-peer-{debug,release}-device.log and errors logs.
- Ordinary hardware/audio suites: 11/11 each, live-peer-{debug,release}.log.
  Both optional live tests were run separately with capture-service access.
  Native application regression suites: 8/8 each,
  build/native-{debug,release}-wgc-stop.log. No normal app routing switch.
- Limits: generated window at 640x360, one local viewer; no measured gaming
  latency, GPU presentation, external source-process exit/device recovery or
  100-cycle/long-duration soak. Capture timestamps currently use delivery time
  in the proof. Hung external driver/OS calls still cannot be preempted.

## Receive NV12 ownership and GPU presentation — 2026-09-14

- Decoder output is now move-owned NV12. Its I420 fallback is converted once
  under a mutex on demand. Normal GPU presentation no longer converts NV12 to
  I420 and back; CPU decoding and GPU texture upload remain explicit.
- LatestVideoFrameSink retains one pending frame, replaces older output and
  rejects callbacks after Stop. A 500-frame deterministic burst returns only
  frame 499. Concurrent fallback conversion shares one validated I420 buffer.
- Nv12VideoPresenter performs D3D operations on the target window thread. Its
  existing shader/fit path gets direct packed NV12, counts conversions/repacks,
  opts into nonblocking Present and sets/verifies a one-frame DXGI device queue.
  Busy/occluded submissions are dropped; software-device fallback is reported.
- Debug/Release hardware/audio suites: 12/12 each, logs
  build/webrtc/presentation-{debug,release}.log. Native app regression suites:
  8/8 each, build/native-{debug,release}-presentation.log.
- WebRTCProof --live-capture passes in both configurations with a visible GPU
  receiver window, resize, neutral-chroma validation and center/letterbox sampling.
  Final runs: 55/56 GPU submissions, 5/4 replaced pending frames, zero extra
  presentation conversions/repacks, zero sender readbacks, 60/61 decoded frames.
  Logs: build/webrtc/presentation-{debug,release}-device.log and errors logs.
- The initial visual probe read stale WM_PAINT pixels from the window GDI DC.
  Flip-model output must be sampled from the composed desktop at that window's
  coordinates. Observed desktop RGB was tinted; tests now verify neutral chroma
  before composition and relative brightness/dark bars afterward. Display settings
  were not modified. This is a content/placement test, not calibrated color QA.
- The generated receiver window must remain unobscured during this optional test.
  Successful Present is an accepted submission, not measured physical scanout.
  No capture-to-display latency, input latency, GPU decoding, external-source
  process exit, device-loss recovery, long soak or complete Gate A claim.
- Normal app routing remains legacy. The next work is source/device recovery,
  remaining capture lifecycle scenarios and artifact/delivery/protocol evidence.

## Source closure, minimization and process exit — 2026-09-14

- Owned WGC capture now observes the original item's Closed event using a shared
  atomic flag. Its revoker is detached before session teardown; callbacks never
  hold a raw capture-object pointer. Window process/thread identity is checked,
  and Closed remains latched until explicit Start, even if another window appears.
- Minimized sources return queued frames without publishing them and wait up to
  10 ms when a timeout was requested, avoiding a hot polling loop. Restore resumes
  capture. Closed/minimized state is checked after GPU snapshot completion too.
  Capture-owner state is exposed; remote source-status UI is not integrated yet.
- Device removal at capture polling/completion boundaries raises a distinct
  CaptureDeviceLostError retaining its HRESULT. No automatic restart, GPU reset,
  injected hardware removal or complete device-recovery claim is made.
- LiveCaptureTest --repeat passes three cycles per configuration with minimize,
  restore, fixed-size resize, permanent closure, replacement-window rejection
  and retained pixels. CaptureProcessExitTest passes three child-process exits
  per configuration. It captures only a generated source in its own suspended-
  startup, kill-on-close job; children are cleaned up if the parent proof exits.
- Both configurations also pass live PeerConnection/GPU presentation regression.
  Device logs: build/webrtc/source-{LiveCaptureTest,CaptureProcessExitTest,WebRTCProof}-{debug,release}.log
  and corresponding errors logs. Capture-service access required normal desktop
  execution outside the restricted process.
- Ordinary hardware/audio suites: 12/12 each, source-lifecycle-debug-final.log
  and source-lifecycle-release.log under build/webrtc. Native application suites:
  8/8 each, build/native-{debug,release}-source-lifecycle.log.
- Still pending: forced HWND reuse stress, actual device removal/recovery,
  remote status integration, long lifecycle/latency checks and remaining Gate A
  build-delivery/protocol evidence. Normal application routing remains legacy.

## Receiver presentation recovery — 2026-09-14

Completed after foundation commit `4ca8318`. The v2 presenter catches typed DXGI device-removed/reset/hung/internal-driver errors, releases its GPU resources on the window thread, drops the failed frame, and rebuilds on a fresh frame after a 250 ms backoff. Three rebuilds are allowed per presenter lifetime; the fourth failure is terminal. Successful frames do not replenish the budget. Other errors propagate. Low-latency queue settings survive recreation. No frame retry queue is added. Capture/encoder recovery and actual driver removal remain open.

`PresentationRecoveryTest` validates backoff, terminal state and error isolation deterministically. Its `--gpu` mode injects a device-loss HRESULT at the render boundary, performs real resource release/recreation on its generated window and requires acceptance of a fresh frame. This is not an actual driver-removal test. Initial restricted desktop runs could recreate resources but could not get an accepted Present; both Debug/Release desktop-session runs passed. The desktop-dependent variant is registered only with `-LiveCapture`.

Validation: 13/13 hardware/audio proof tests in each configuration (`build/webrtc/presentation-recovery-{debug,release}.log`); 8/8 native application tests each (`build/native-{debug,release}-presentation-recovery.log`); separate desktop GPU proofs (`build/webrtc/recovery-desktop-{debug,release}.log`). No physical gaming latency claim. Normal application routing remains legacy. Gate A remains open.

## Capture reconstruction and encoder retirement — 2026-09-14

`DesktopCapturer::RebuildWindowDevice()` rebuilds an owned WGC window source on the capture owner thread while retaining the original GraphicsCaptureItem and Closed subscription. It checks source process/thread identity and latched closure before/after reconstruction; it never creates an item from a replacement HWND or falls back to a display. Failed reconstruction stops capture. Explicit Start is required afterwards. Device-only cleanup is separated from COM teardown so the item remains valid during recreation. Display reconstruction is not implemented.

`MfHardwareSession::RetireDevice()` permanently quarantines the original device. DeviceRemovedReason is also checked before hardware use and during output polling. All encoders sharing that session discard native frames from the retired device without CPU readback, preserve keyframe intent, and accept a replacement device's frames using software. Ordinary transform stalls still use the existing same-frame fallback when the device remains healthy. The capture owner must retire the session before rebuilding/publishing replacement-device frames.

Validation: Debug/Release hardware/audio suites 13/13 (`build/webrtc/device-rebuild-{debug,release}.log`), native application suites 8/8 (`build/native-{debug,release}-device-rebuild.log`). Hardware tests cover two viewers, retired input drops, fresh-device timestamps/IDRs and shared cached readback. LiveCaptureTest --repeat passed three same-process rebuild/closure cycles per configuration in the desktop session (`build/webrtc/rebuild-live-{debug,release}.log`, matching `-errors.log`). Older stdout labels call all outputs hardware; assertions separately verify hardware before retirement and software after it; the label is corrected in source.

Limits: these tests explicitly retire a healthy device/reconstruct resources; they do not induce actual driver removal. Automatic session-level recovery, bounded retries, repeated device generations, full device-error classification, forced HWND reuse and latency measurements remain open. The proof's LiveCaptureSource does not yet invoke this recovery automatically. Gate A remains open; normal product routing is unchanged.

## Automatic proof capture recovery — 2026-09-14

The live proof now catches typed capture-device loss, retires the current D3dVideoDevice, waits a cancellable 250 ms backoff, rebuilds the original WGC item and publishes through a new device owner. CaptureRecovery allows three rebuilds per source lifetime; a fourth loss retires the last owner and fails. Reconstruction errors are terminal. Each successful rebuild advances the diagnostic generation; first-frame startup/recovery has a five-second deadline. Recovery is in the proof source, not the normal application backend.

Retirement now belongs to every D3dVideoDevice, including replacement generations. Cached ToI420 results are suppressed after retirement. Encoders reject retired native input before conversion and check again before delivery; a readback/loss race drops the frame without permanently failing the software encoder. Tests cover two encoders moving across three device owners with fresh IDRs. This is not yet a production callback-generation barrier: already executing callbacks are not preempted.

Validation: Debug/Release hardware/audio suites 14/14 (`build/webrtc/automatic-recovery-{debug,release}.log`). Desktop CaptureRecoveryTest --live passes in both: three automatic rebuilds, fourth-loss failure, cached-frame rejection and Stop during backoff. Live WebRTCProof --live-capture passes in both (60 decoded H.264 frames, Opus/data channels, zero sender readbacks). Logs: `build/webrtc/automatic-{CaptureRecoveryTest,WebRTCProof}-{debug,release}.log` and corresponding error logs. Normal application sources were not changed in this milestone; application suites were last validated at the prior commit.

Release LiveCaptureTest --repeat had one unsuccessful run after reaching second-cycle shutdown, with no exception text captured. Debug repeated cycles passed. A standalone Release rerun (`automatic-LiveCaptureTest-release-retry.log`) passed all three cycles, and a cdb run (`automatic-live-release-debugger.log`) also passed all three without an access violation. The initial failure is retained as unresolved evidence, not explained away by passing reruns. Investigate before claiming clean repeat lifecycle/Gate A.

Actual driver removal, broad HRESULT classification, production session recovery/callback retirement barriers, forced HWND reuse, packaging and latency evidence remain open. Source recovery supports owned WGC windows only.

## SDK delivery and extended lifecycle validation — 2026-09-14

Completed a broader build-delivery milestone after `329c077`. `webrtc-sdk.py` exports content-addressed schema-2 SDKs with library, complete header/generated-header trees, Clang builtins and license notices. Each file is hashed; the inventory and ABI/build metadata form the identity. CMake checks file integrity, unexpected files, compiler hash, MSVC toolset, Windows SDK, configuration, runtime, GN arguments and build patch. Existing local schema-1 outputs remain supported. Export preserves interrupted staging and verifies existing destinations instead of overwriting them. Python integrity tests cover valid artifacts, changed/extra headers, wrong identities and outside paths (5/5).

The pinned upstream notice generator was missing FFmpeg/OpenH264 mappings when H.264/proprietary codecs are enabled. A wrapper adds paths declared in their README.chromium files; unknown dependencies still fail. Compiler-rt's notice is also bundled. Native portable packages carry WebRTC notices and installed Qt SBOMs. Remaining distribution obligations/notices and installer/fresh-machine validation are still open.

Debug and Release SDKs each contain 40,835 inventoried files. IDs and commands are in BUILD.md. Fresh SDK proof builds pass 14/14 each (`sdk-proof-{debug,release}.log`). Release SDK was physically moved from its cache directory to `build/sdk-relocation/Release`; reconfiguration/rebuild again passed 14/14 (`sdk-relocated-release.log`). No original WebRTC source include directory is used by the imported target; pinned compiler and system/Qt prerequisites remain external.

Fresh application builds exposed real delivery gaps: missing vcruntime140_1.dll from Qt deployment; a MinGW-only plugin directory; and UI self-test checking icons before constructing QApplication. The build now stages MSVC redistributables explicitly, selects native Qt tools, resolves both native/MinGW plugin layouts, keeps Debug and Release plugin variants separate, and registers UI self-test in CTest. Failed self-tests print diagnostic groups. Application suites pass 9/9 each (`sdk-app-{debug,release}-final.log`). Native packaging fails for missing WebRTC notices or unresolved runtime libraries and validates the staging path before recursive cleanup.

Release zip: `build/sdk-app-release/ScreenShare-release-windows-x64.zip`. Extracted to `build/sdk-portable-smoke-final`, it passes CLI --help, UI --self-test with offscreen platform, and --gui-smoke-test with Windows platform. PATH contained only Windows system directories; QT_PLUGIN_PATH was empty. `portable-plugin-paths.log` confirms package-local qwindows/style/SVG plugin loading. No new UI screen or product media routing was introduced. An initial fresh application configure attempted the existing ViGEm download and failed under restricted networking; the runner now accepts the existing local ViGEm source explicitly for offline builds.

Lifecycle investigation: LiveCaptureTest now accepts --cycles 1..100 and prints cycle-start/destruction markers. Twenty Release cycles passed under cdb (`capture-stress-release-debugger.log`), then twenty without debugger timing (`capture-stress-release.log` / `capture-stress-release-errors.log`), all reaching destruction. The earlier unexplained failure was not reproduced and is NOT marked fixed. Actual driver removal, complete callback/resource-leak barriers, forced HWND reuse and Gate A remain open.

Reference machine collected with no identifying names/addresses: Windows 11 Pro 10.0.26200; Ryzen 7 9800X3D (8 cores/16 logical processors); 66,157,719,552 bytes RAM. Reported adapters: NVIDIA RTX 5070 Ti driver 32.0.16.1062, AMD Radeon Graphics 32.0.21045.5002, Virtual Desktop Monitor 13.50.53.699. Enumeration does not prove the adapter selected in every probe. Both peers run on this machine against generated 640x360 grayscale content, nominal 30/60 FPS depending on test, 1 Mbps lifecycle tests, stereo 48 kHz Opus and local direct ICE. Full inventory: `build/baseline/native-media-reference/manifest.json`. This is not an external LAN/game latency baseline.
