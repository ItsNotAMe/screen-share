# Headless media checks

Continuous displayed-image age is now part of fresh packet-impairment reports.
The generated scene carries a frame ID and checksum through the real codec;
the consumer also measures the age of an image held during a freeze. Run the
closeout collapse checks with `--require-settling` to require freshness within
three seconds, including zero unreadable markers. See
[CONGESTION-WINDOW.md](CONGESTION-WINDOW.md) for the full Release/Debug commands,
matched failing old controls and measurement limits. All audio is discarded and
input goes to the recording sink. This does not measure physical display/input.
`python tests/RoomImpairmentTests.py` now covers 24 evidence checks.

Congestion diagnostics now split each synthetic response into observed input
application and return-image consumption, with exact total consistency checks.
They also record the affected link's modeled packet residence separately from
pump scheduling lateness. See [CONGESTION-STAGES.md](CONGESTION-STAGES.md) for
commands, rejected trials and interpretation limits. Fresh runner output requires
all stage records; the congestion load and existing recovery gates are unchanged.

The impairment runner now records five synthetic input/image responses in each
of baseline, impairment and recovery, with explicit release and revoke checks.
See [PLAYOUT-RECOVERY.md](PLAYOUT-RECOVERY.md) for the Gaming/Quality policy and
matched comparisons. It remains silent and does not inject physical input.
The four-viewer public-room regression switches Gaming→Quality→Gaming while
resizing, restarting and rejoining; restored sender settings alone are insufficient:
fresh 640-wide receiver observations and continued frame delivery are required.

Silent congestion diagnosis now supports bounded per-peer RTC logs and restricted
probe/ALR summaries. See [CONGESTION-RECOVERY.md](CONGESTION-RECOVERY.md) for the
one-command workflow, corrected transport budget and remaining latency boundary.

Live HTTPS/WSS lifecycle checks now pass on desktop and laptop with silent
four-viewer media, recovery and shutdown. See [LIVE-SERVICE.md](LIVE-SERVICE.md)
for the bounded one-command runner and its exact network/latency scope.
The same runner now supports CLI and coordinated host/viewer roles on two PCs.
Both LAN directions pass silent media/recovery/settings/input-marker checks;
see [TWO-MACHINE.md](TWO-MACHINE.md) for commands, evidence and latency limits.

Latest continuation: [FIELD-TESTING.md](FIELD-TESTING.md) records the final 12/12
Release/Debug headless checks, earlier 16/16 desktop run, blocked final desktop
attempt, FPS-resume fix, rejected pacing experiment and concise laptop handoff.
The optional field scene also has a silent `--self-test`; physical acceptance
remains separate from these automation results.

## Receiver buffering regression — 2026-09-18

`SwitchablePcmCapture` now emits exactly one PCM-or-silence block per output slot;
late input cannot overfeed audio and cause A/V synchronization to delay video.
The late-device cadence regression and the existing PCM/public-room suites pass
in both configurations. Lifecycle validation also rejects missing or over-100-ms
mean receiver-buffer reports on loopback; fifteen evidence tests pass. See
[RECEIVER-PACING.md](RECEIVER-PACING.md) for before/after artifacts, the final
180-second scenarios and the remaining two-hour/physical/network acceptance scope.

## Room ownership, memory and slow presentation — 2026-09-18

The full-room runner now observes 24 weak dependencies, live synthetic audio
endpoints and capture resources per cycle. Optional `--memory-accounting` performs
read-only post-stop heap/virtual-memory scans; `--idle-seconds` records cleanup
separately. `--slow-viewer` slows consumption through the actual UI/CLI one-frame
buffer and checks each viewer, frame replacement, queue bounds and recovery without
catch-up bursts. All audio is discarded; no physical input is injected.

Release/Debug 100-room accounting runs pass ownership and the unchanged handle
bound (+3/+2). Live heap allocations remain roughly 1.6/1.9 MB after warm-up;
much larger heap free-space totals and variable later decommit explain part of the
private-byte variation. They do not establish full memory acceptance. Both builds'
public input/media regression and fourteen evidence tests pass. See
[ROOM-STRESS.md](ROOM-STRESS.md) for commands, exact artifacts, failed trials and
interpretation. The isolated Release 180-second presentation run fails sustained
rate recovery; Debug passes the ratio check but also slows over time. The two-hour
soak and network/decoder impairment remain open. Investigate receiver buffering/
drops before treating short functional progress as gaming-latency evidence.

## Typed capture handles and idle cleanup — 2026-09-18

`scripts/trace-capture-handles.py` now captures typed outstanding handles and
correlates successful RPC port allocations in the Debug production-owner proof.
The actual run completed with five retained WGC/RPC ALPC ports identified. The
debugger is launched as a test child under a process-tree/log watchdog; it never
attaches to an existing application. This is diagnostic evidence, not acceptance.

The production-owner stress proof supports separate `--idle-seconds` observations
and an explicit test-only `--rpc-idle-cleanup` experiment. Neither changes the
immediate +8 handle check. Missing or invalid idle evidence fails independently.
The 500-cycle cleanup experiment fails at +254 handles, then releases 173 during
180 seconds idle. A reused capture thread still fails at +91 over 100 cycles.
The normal-policy 100-cycle run fails at +92 immediately, then falls from 398 to
289 handles over 360 seconds idle (warm-up median 304), without enabling the
cleanup API. The report retains both the failed immediate gate and later recovery.
See [CAPTURE-HANDLES.md](CAPTURE-HANDLES.md) for artifacts and interpretation.

Both configurations build and pass the three evidence CTests: 21 capture, six
trace and five room tests. Production policy is unchanged; full resource, soak,
impairment, physical/network and latency acceptance remain open.

## Full-room lifecycle and continuous media — 2026-09-18

`scripts/test-room-lifecycle.py` now runs complete room restarts in one native
process or a bounded continuous four-viewer run. Production quotas are unchanged;
each restart gets a new local Worker fixture. Synthetic audio is discarded and
input sinks are recording-only. See [ROOM-STRESS.md](ROOM-STRESS.md) for commands,
resource measurements, retained failed attempts and remaining acceptance.

Release/Debug **100-room** runs pass with +5 median handle growth each. Release
**180-second** and Debug **30-second** continuous runs pass. Both builds' original
room input/media regression passes after extracting the shared test fixture.
Five room-evidence and seventeen capture-evidence/watchdog tests pass.

The longer capture test completed **500 cycles but FAILED resource acceptance**:
median handles 300 → 526 (+226), peak 558. Memory was roughly bounded over that
run; neither its ownership nor the delayed handle growth is resolved. This is
not a two-hour soak or a production leak fix. Full memory/queue/impairment and
physical/network/latency gates remain open.

## Capture lifecycle/resource batch — 2026-09-18

`python scripts/test-capture-lifecycle.py build/sdk-proof-release build/webrtc/NEW`
now runs four silent generated-window modes with 100 cycles each: rapid close,
fresh owner thread, hardware/recovery and the production Windows capture owner.
It preserves the original +8 handle bound, records memory/GUI trends separately,
and rejects missing evidence, wrong binary hashes, timeout and excessive logging.

Final Release **400 cycles** and Debug **80 cycles** passed all four modes;
`build/webrtc/stress-group-final-{release,debug}/result.json` records each result.
A separate Debug production-owner **100 cycles** also passed, in
`stress-group-owner-final-debug/result.json`. Rebuilt focused CTest suites passed
**5/5 each**, including the **17-test** Python evidence/watchdog suite.

Production-owner coverage includes actual WGC/GPU frames, minimize/restore,
resize, recovery, alternating source-close/active-stop, owner-thread destruction,
retained-frame release and input geometry/property cleanup. It never injects
physical input or plays audio. Full room restarts and the two-hour four-viewer
soak remain separate. Private memory grew despite stable handles; see
[RESOURCE-STRESS.md](RESOURCE-STRESS.md) for exact measurements and next work.
This batch adds validation and diagnostics; it does not claim a new production
leak fix or waive the earlier +76/+10 failures.

## Mouse/keyboard integration — 2026-09-18

Final Release and Debug application builds passed. Both complete desktop-inclusive
matrices passed **15/15**, including all **11 headless** cases:

- `build/webrtc/desktop-input-final-desktop-release/result.json`
- `build/webrtc/desktop-input-final-desktop-debug/result.json`
- Build logs: `build/webrtc/desktop-input-final-build-{release,debug}.log`

Focused CTest checks passed **9/9 in each build**: native runtime linking, input
service, gamepad control, desktop input, controller protocol, A/V diagnostics,
report paths, video-frame input and UI self-test. CTest retains LastTest.log in
each build's Testing/Temporary directory.

The runner adds `desktop-input`, `desktop-input-ui` and
`windows-desktop-input-ui`. Production encoder/decoder and encrypted channels
carry source-bound events into recording sinks. UI tests cover displayed mapping,
encoder padding, explicit capabilities/consent, focus release, source replacement,
fresh regrant and stale-generation rejection. CLI covers controller-to-mouse
handoff and test-owned native preview events. No physical keyboard/mouse/controller
injection or audible playback occurs. Windows cases use generated WGC windows
and real D3D presentation; offscreen Qt uses a recording presentation backend.

Initial failures are retained under `desktop-input-ui-first`,
`desktop-input-ui-trace`, `desktop-input-cli-first` and
`desktop-input-cli-diagnose`. Offscreen D3D could not present, requiring explicit
test presenter injection. Leading SEI before SPS/PPS hid receiver resize telemetry;
metadata now follows headers and precedes slices, with an ordering regression.
These runs are not counted as passes and production quotas/timeouts were not relaxed.

See [DESKTOP-INPUT.md](DESKTOP-INPUT.md). These results complete local input
integration, not physical confinement/driver behavior, impaired-network acceptance
or external input-to-photon latency. Stage 4 resource/stress work is next; all
physical, service-cost, NAT/TLS and default-cutover gates remain open.

## Controller integration — 2026-09-18

The runner now includes `gamepad-control` and a separate `controller-ui` service
fixture: nine headless cases or twelve with
`--desktop`. UI and CLI cases include explicit controller requests/grants/revoke
over real room channels with recording sinks and injected device readers. UI also
checks focus/panic/source transitions, unplug and failed grant. No physical input
or audible playback is used. See [CONTROLLERS.md](CONTROLLERS.md) for coverage and
the separate physical-driver/latency acceptance limits.

The initial combined UI run failed admission when another room scenario was added;
controller UI now has its own service instance, with unchanged production quotas.
Subsequent repeated regrant checks exposed a real repeated-revoke race: cleanup
could clear an unsent release and a request could precede the host acknowledgement.
The service now preserves pending release and blocks new requests until acknowledgement;
`RepeatedRevoke` tests this directly. Three independent controller UI repeats pass
under `build/webrtc/controller-revoke-repeat-{1,2,3}`. Earlier failed artifacts are
retained and are not counted as passing evidence.

Final Release and Debug desktop-inclusive matrices: **12/12 passed each**,
`build/webrtc/controller-revoke-release/result.json` and
`build/webrtc/controller-revoke-debug/result.json`. Release and Debug application
builds succeeded (`controller-revoke-build-{release,debug}.log`). The focused
runtime-link/input/controller/AV/UI smoke suite passed **6/6 in each configuration**.

## Input transport/safety and returning-image proof — 2026-09-18

The normal regression runner now includes `input-service` and `input-media`:
at that checkpoint, seven headless cases or nine with `--desktop`. Both cases use recording
sinks; no physical input is injected and synthetic audio remains discarded.

```powershell
python scripts/test-room-regression.py build/sdk-app-release build/webrtc/input-check
```

For only the actual four-viewer input/media scenario:

```powershell
node signaling-worker/tests/run-native-service.mjs build/sdk-app-release/RoomInputTests.exe build/webrtc/input-media-check media
```

`RoomInputTests` uses the production public RoomSession/NativeRoomRuntime and actual
WebRTC channels. A host-authorized key changes a synthetic image, which passes
through encoding, transport and decoding. It also verifies watchdog release while
both host and viewer runtime advancement pause, fresh explicit regrant and revoke.
Its `input_response_internal_ms` is one localhost sample, not an external latency
measurement, percentile or fulfillment of the gaming p95 target.

- Release/Debug complete application builds: `input-complete-app-{release,debug}.log`.
- Final seven-case matrices pass **7/7 in both builds**:
  `build/webrtc/input-verified-{release,debug}/result.json`. Each includes the
  actual input/media proof linked against the application libraries.
- Runtime/input/UI/diagnostics smoke: **5/5 each**, `input-smoke-{release,debug}.log`.
- Final closeout builds and focused input checks are recorded in
  `input-closeout-app-{release,debug}.log` and `input-closeout-tests-{release,debug}.log`.
  These include the test target's Qt deployment dependency and monotonic watchdog
  receive time across reordered state kinds.
- Portable tests cover explicit wire bytes/truncation/ranges, wrong channel,
  connection/permission/sequence validation, cross-kind reorder, 10,000 coalesced
  pointer submissions, full pad keepalive after a dropped state, 300 ms watchdog,
  delayed reliable input, queue overflow, stuck SCTP with an empty application
  queue, ownership/local-slot policy, backend failure, removal/rejoin and shutdown.
- Standalone four-viewer proof passes in both configurations at
  `input-public-final-{release,debug}/native-service-*/result.json`; observed internal
  response samples were 57/51 ms. Generated-window Windows-runtime binding also
  passes both at `input-windows-{release,debug}/native-service-*/result.json`, with
  keyboard grants rejected for a window share and mouse events sent only to the
  recording sink. Its response metric is null because no Windows input is injected.
- Earlier six-case matrices passed at `input-regression-{release,debug}/result.json`.
  The runner then gained the four-viewer scenario as a regular application target.
- The first standalone proof completed its assertions but failed result parsing
  because it printed two JSON objects. Evidence is preserved at
  `input-public-release`; the metric now lives in the single result object.

See [INPUT.md](INPUT.md) for integration contracts and the remaining Windows,
mapping and UI/CLI consent group. This is not Gate D or default-control enablement.

## Guarded normal-home/CLI adoption — 2026-09-18

- Release/Debug application builds pass (`adoption-verified-app-{release,debug}.log`).
- Both final desktop-inclusive matrices pass **7/7**:
  `build/webrtc/adoption-verified-{release,debug}/result.json`.
  Their UI metrics include `normal_home:true`; CLI metrics include
  `command_options:true`. Windows variants use generated WGC sources/native
  presentation; all audio remains synthetic/discarded and no physical input is sent.
- New UI coverage starts from the existing HomeWindow: repeated Create/Back
  navigation with one directory connection, no legacy HTTP client, plain-text
  pushed room names, password-protected Quick Join by room ID, decoded frames,
  returning home and closing during host activity.
- CLI coverage uses the new command parser for the actual host/viewer media
  scenario. Tests also cover persisted defaults/ephemeral overrides, manual/auto
  settings, malformed password files, role/option conflicts and origin validation.
- Actual executable startup rejection is recorded in
  `adoption-cli-launch-rejected.log`, `adoption-cli-legacy-rejected.log` and
  `adoption-ui-launch-rejected.log`. No service request is needed for these checks.
- Runtime/UI/diagnostics smoke checks pass **4/4 per build**:
  `adoption-smoke-{release,debug}.log`.
- Final include/indentation cleanup also builds in both configurations:
  `adoption-closeout-app-{release,debug}.log`; no media behavior changed after
  the final matrices.
- Intermediate Release headless and desktop matrices passed. The first Debug
  headless run failed the new test's first-row assumption during asynchronous
  removal of a previous room. The test now finds the exact room ID; both final
  full matrices pass without relaxed deadlines or media thresholds.
- The interactive desktop is available again. A new WGC pinned-display rebuild
  passes, but DXGI still returns unsupported (`adoption-capture-display.log`).
  This remains an open acceptance item, not a test pass for DXGI.

See [ADOPTION.md](ADOPTION.md). Guarded room workflows are implemented; legacy
input/gamepad/reporting parity, physical and external latency/resource/network
acceptance, and default cutover remain open. No deployment or update installation
was performed.

## Capture fallback/cursor/lifecycle — 2026-09-18

- Release/Debug application and focused capture builds pass:
  `build/webrtc/capture-app-{release,debug}.log` and
  `build/webrtc/capture-build-{release,debug}.log`.
- Both silent headless room matrices pass **5/5**:
  `build/webrtc/capture-headless-{release,debug}/result.json`.
- Final application rebuilds and offscreen runtime/UI/diagnostics smoke tests pass
  **4/4 in each build**: `capture-final-app-{release,debug}.log` and
  `capture-final-smoke-{release,debug}.log`. The final rebuild adds explicit standard
  includes and preview failure geometry; media behavior matches the matrix builds.
- Focused CTest checks pass **4/4 in both builds**: capture-session-headless,
  host-media-session-coordinator, capture-backend-cursor-policy and
  capture-recovery-policy. GPU cursor tests use WARP and synthetic pixels;
  no desktop or physical pointer is needed.
- `CaptureBackendTest.exe --live` and `CaptureRecoveryTest.exe --live` pass in
  both builds, using generated windows and no audio/input. The new lifecycle
  test exercises the production switchable-source wrapper and minimized startup.
- `CaptureBackendTest.exe --display` passes the Release WGC pinned-display
  rebuild. DXGI is explicitly reported unavailable (`DXGI_ERROR_UNSUPPORTED`),
  **not passed**, on the current desktop. No display pixels are read back or saved.
- The initial seven-case Release run passed five cases, then native CLI preview
  could not present. Debug reproduced it. A read-only Windows query confirmed
  test desktop `Default` versus input desktop `Screen-saver`, explaining this
  observed occlusion. This does not prove the cause of all historical reports.
- `--desktop` now checks that the input desktop matches the test desktop before
  running. An unavailable/locked/screensaver desktop produces `passed:false`,
  `blocked:true`, a reason and exit code 2, with no simulated success. The runner
  never dismisses the saver, unlocks Windows or sends physical input. Its
  `capture-desktop-preflight/result.json` records the tested blocked condition.

Rerun the full desktop matrices after an interactive desktop is available.
Headless checks remain usable meanwhile. See [CAPTURE-RECOVERY.md](CAPTURE-RECOVERY.md)
for source, cursor, HDR, recovery contracts and the remaining physical gates.

## GPU receive/recovery — 2026-09-17

- Release/Debug application and focused proof builds pass:
  `build/webrtc/video-final-{app,proof}-{release,debug}-build.log`.
- Both full desktop matrices passed **7/7**:
  `build/webrtc/video-verified-{release,debug}/result.json`.
  The Windows UI/CLI scenarios require native decoded frames, hardware decoder
  telemetry and live resize while retaining existing room/audio/settings checks.
- `MfDecoderAdapterTest.exe --gpu` passes in both configurations:
  `build/webrtc/video-final-decoder-{release,debug}.log`. Each checks 44 CPU frames,
  44 startup-fallback frames and 44 GPU frames across four configure/release
  cycles; 1080p bottom-row luma/chroma, retained surfaces after release, RTP/NTP,
  zero implicit readbacks, bounded texture retention, device retirement, fixed-size
  fallback, backoff/keyframes and exhausted malformed-input recovery are covered.
- Both `StreamSettingsTest.exe --gpu` runs pass:
  `build/webrtc/video-final-settings-{release,debug}.log`, including hardware
  decoder allowlisting and existing source-scaling/fallback/settings checks.
- Four simultaneous Windows hardware-decoding viewers passed in **18.631 s**
  (Release) and **17.543 s** (Debug):
  `build/webrtc/video-four-viewer-gpu-release/native-service-b8fadbbb-9de4-4d42-bc3c-8601f508006a`
  and `build/webrtc/video-four-viewer-gpu-debug/native-service-6d6676b4-1a9b-4145-af21-dfa5088c417d`.
  Software four-viewer integration passed in **18.008 s** at
  `build/webrtc/video-four-viewer-software-release/native-service-1ff0db19-8e49-4753-9916-63450b06e3ca`.
- Final review added whole-draw context protection for the shared MF/presentation
  device. Both application builds passed again (`video-context-app-*-build.log`),
  followed by **4/4** targeted Windows UI/CLI scenarios in Release/Debug:
  `build/webrtc/video-context-{release,debug}-{RoomCliWindowsTests,RoomUiWindowsTests}/`.
  The four GPU-viewer proofs above do not attach a renderer; the targeted frontend
  runs exercise the final shared-context change.
- The final decoder test additionally verifies quarantine survives Configure and
  replacement decoder instances from the same factory. Release/Debug runs pass;
  build logs are `video-quarantine-proof-*-build.log`. A new runtime/factory is
  required to retry hardware after quarantine.
  Both final application rebuilds also pass (`video-quarantine-app-*-build.log`).
  Final native-runtime-link, A/V diagnostics, report-path and UI-self-test smoke
  checks pass **4/4 in each build** (`video-final-smoke-{release,debug}.log`).

Intermediate `video-receive-release` failed an old CPU-only test assertion. The
stricter native-frame assertion then exposed delayed pre-resize output being
validated against new dimensions (`video-cli-size-debug`). Dimension limits now
travel with timestamp associations; corrected Windows scenarios and both full
matrices pass. No timeout or success threshold was relaxed. Temporary decoder
logging used for diagnosis was removed.

The desktop tests use generated windows and discarded synthetic audio. UI/CLI
pixel assertions deliberately request readback; the separate Qt native renderer
test requires zero readbacks across GPU/CPU switching and injected recovery.
Physical devices, driver hangs/removal, historical occlusion, strict zero-copy,
network impairment and measured gaming latency remain unverified. See GPU-RECEIVE.md.

## Audio processing/downmix — 2026-09-17

- Release and Debug desktop-inclusive matrices passed **7/7 each**:
  `build/webrtc/audio-completion-{release,debug}/result.json`.
  Actual UI/CLI sessions select microphone speech processing, publish its status,
  switch back to bypassed system/process audio and preserve video. UI additionally
  loses/retries the microphone endpoint while video progresses.
- Both focused PCM runs pass:
  `build/webrtc/audio-completion-pcm-{release,debug}.log`. Coverage includes real
  APM/DC filtering, mono/stereo identity, 5.1/7.1 per-channel gains, full-scale
  headroom, malformed formats, processor-factory failure rollback, mic worker
  restart, all non-mic bypass modes and the existing silent recovery/lifecycle suite.
- Four-viewer scenarios pass in **18.589 s / 18.549 s**:
  `build/webrtc/audio-completion-four-viewer-release/native-service-fdf0ca4e-84c8-46fd-8edc-6cbb0c3f0c3c`
  and `build/webrtc/audio-completion-four-viewer-debug/native-service-26392211-833d-49d6-8b74-f11e735e5d73`.
- Final application/proof builds:
  `build/webrtc/audio-completion-final-{app,proof}-{release,debug}-build.log`.
  A final defensive extended-format size guard was followed by both PCM suites
  and **4/4** smoke checks per configuration (native runtime link, A/V diagnostics,
  report path and UI self-test): `audio-completion-smoke-{release,debug}.log`.
  The initial Debug configure lacked the pinned developer environment; rebuilding
  with VsDevCmd satisfied the unchanged SDK verification.

No physical microphone/speaker or mouse/keyboard was used. Actual Windows capture
channel-format negotiation, microphone listening quality, physical unplug/replug,
driver hangs and external latency remain unverified. See AUDIO-PROCESSING.md.

## Diagnostics and settings integration — 2026-09-17

Final artifacts for this batch:

- Application and proof Release/Debug builds:
  `build/webrtc/diagnostics-{app,proof}-{release,debug}-build.log`.
- Native protocol/link smoke: `NativeRoomRuntimeTests.exe` passed in both builds,
  including V2 framing, canonical absence, exact three-second renderer/report
  expiry, malformed fields, no-renderer replacement, replay and generation checks.
- `StreamSettingsTest.exe` passed in both builds:
  `build/webrtc/diagnostics-settings-{release,debug}.log`. It injects sender
  rejection/topology errors, checks two-viewer isolation, retries and preserved
  manual settings, and validates codec/encode/retransmit/decoder/buffering stats.
- Final desktop-inclusive matrix results:
  `build/webrtc/diagnostics-verified-{release,debug}/result.json`.
  **7/7 passed in each configuration.**
  UI/CLI tests cover actual encrypted renderer reports, no-renderer null values,
  shared pipeline/recovery diagnostics, preset preservation and partial UI state.
- Four-viewer Release/Debug scenarios passed in **18.000 s / 18.490 s**:
  `build/webrtc/diagnostics-four-viewer-release/native-service-38fe2fc3-b953-4a8c-af65-0c5dee01cd96`
  and `build/webrtc/diagnostics-four-viewer-debug/native-service-10741da8-b1d1-44ec-902b-dbf90439631a`.
  Fresh decoder/codec/handoff/recovery reports, telemetry expiry with continuing
  media, resumption, restart, rejoin and failure isolation remain covered.

The intermediate `diagnostics-final-release` matrix failed the new partial-UI
fixture assertion because the preceding settings operation had not fully settled.
The test now waits for matching requested/applied/source-observed revisions and no
pending update before injecting its partial snapshot. No assertion was weakened or
timeout increased. The earlier `diagnostics-release` matrix passed all seven cases.

All audio remains synthetic/discarded and no physical input is injected. These
checks do not establish physical display, image/input latency, hardware decode,
driver-loss acceptance, real TLS/NAT or performance improvement. See DIAGNOSTICS.md.

## Native target visibility and Windows fixture corrections

Production presentation now tests the root window's minimized state before GPU
work, plus child visibility and nonzero client area. The Windows recovery scenario
checks child-surface minimize/hide/restore with no extra device recovery, in
addition to its original injected device-loss budget checks.

The startup timeout reproduced with `qtVisible=1`, `winVisible=0`, 974 enqueued
frames, zero presented frames and zero graphics errors:
`build/webrtc/presentation-target-windows/native-service-10ce4020-b184-45c6-afa0-36644f92be8b`.
The fixture now explicitly shows its test-owned window without activation after
Qt's first show. This addresses the launcher's hidden STARTUPINFO overriding the
first native show ([Windows contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-showwindow)).
The adapter now forwards native outcomes, and startup failure diagnostics retain
Qt/native visibility, drops, errors and HRESULT.

Both UI and CLI had a separate fixture assertion expecting a taller WGC window to
fill a 16:9 canvas. Windows cases now require positive, centered, even pillarboxes;
synthetic 16:9 cases still require a full image. No media or presentation assertion
was removed, no timeout increased and no audio/input device was used.

Release/Debug builds and both five-case silent matrices passed under
`build/webrtc/presentation-target-{release,debug}/result.json`. The first desktop
matrix stopped at the incorrect CLI image assertion; UI was run independently.
Final Release Windows UI passed in **14.804 s** and Windows CLI in **8.520 s**:
`build/webrtc/presentation-target-final-windows-ui/native-service-f562686e-a2bd-47df-b3fc-fe176fccd8e4`
and `build/webrtc/presentation-target-final-windows-cli/native-service-75cca481-76a7-4539-8e50-2b99bf428125`.
These passes include the shared shell's Windows room scenarios. They do not prove
the cause of historical visible-but-occluded results, physical driver acceptance,
hardware decoding/zero-copy presentation or external image/input latency.

The subsequent Debug desktop run exposed a real teardown assertion, not a fixture
failure: `Thread::BlockingCallImpl` rejected the signaling executor's invoke from
`D3dVideoDevice::~D3dVideoDevice`. The local debugger stack is retained at
`build/webrtc/presentation-debug-stack-final/native-service-4e3056c7-2244-456b-9498-c2cde4cd6f3e/native.log`.
The temporary debugger launcher was removed after diagnosis. Cleanup now joins the
owner before releasing scaler COM references; invoke permissions remain unchanged.
`StreamSettingsTest --gpu` now releases the last retained scaled texture from a
thread with all invokes disabled and checks device lifetime expiry. It passes in
Release and Debug (`build/webrtc/desktop-lifecycle-gpu-{release,debug}.log`).
Final Release and Debug desktop-inclusive matrices: **7/7 each**, including Windows
CLI/UI shutdown, `build/webrtc/desktop-lifecycle-{release,debug}/result.json`. Final build evidence:
`build/webrtc/desktop-lifecycle-{app,proof}-{release,debug}-build.log`.

## Shared application shell

The final silent Release/Debug matrices passed **5/5 each**:
`build/webrtc/room-shell-final-{release,debug}/result.json`.
Build logs: `build/webrtc/room-shell-final-{release,debug}-build.log`.
The browser and source/audio scenarios now run their actual pages inside the
production RoomApplication/AppShell. They assert one top-level window, bounded
page count after failed/repeated joins, unchanged hidden-directory subscription
counts, return navigation, retained profile settings, screen-awake release and
asynchronous repeated close during streaming and immediate admission cancellation.
The Release legacy UI `--self-test` passed after shell extraction.

The optional generated-window Windows UI run failed before these scenarios, at
NativePresentationRecovery's initial wait for three presented frames (line 79),
in 15.510 s. Evidence:
`build/webrtc/room-shell-windows/native-service-3ac5a76d-1111-41d0-9007-37859d3f4741`.
That test emits no renderer snapshot at this wait, so its precise cause is unknown;
do not label this a confirmed occlusion failure or claim desktop shell acceptance.
Existing desktop presentation gates remain open. All audio remained synthetic
and silent; no physical keyboard/mouse input was sent.

## Audio failure and recovery

`build/webrtc/audio-recovery-{release,debug}/result.json` passed **5/5 each**.
Actual UI tests start with failed capture, retry from widgets, then lose/retry
playback while video continues. CLI tests expose failed output and recover through
a timed command. `PcmAudioDeviceTest` passed in both configurations; logs are
`build/webrtc/audio-recovery-pcm-{release,debug}.log` and cover owner-thread release,
no background retries, failed/successful same-device recovery and shutdown.

The final synthetic four-viewer proof passed in **18.248 s**, including host audio
loss with video continuity for all viewers and isolated output loss/retry for one:
`build/webrtc/audio-recovery-four-viewer-final/native-service-71238484-3be2-473e-9e0f-6ddc19ba4c37`.
The same recovery scenario with generated-window Windows capture passed in
**18.635 s**:
`build/webrtc/audio-recovery-windows-room/native-service-abcacbc7-2359-44c3-9888-7a0764fc89b3`.
These tests open no physical audio endpoint and use no physical input. See
[AUDIO-RECOVERY.md](AUDIO-RECOVERY.md) for the contract and untested driver behavior.

## GPU scaling integration

Final Release/Debug evidence is in
`build/webrtc/gpu-scaling-bounded-{release,debug}/result.json` (**5/5 each**), with
GPU pixel/encoder logs `gpu-scaling-bounded-{pixels,mf}-{release,debug}.log` in the
same `build/webrtc/` root. The generated-WGC four-viewer scenario passed with desktop
access under `build/webrtc/gpu-scaling-bounded-windows-room/`. No physical audio or
input was used. [GPU-SCALING.md](GPU-SCALING.md) describes submission bounds,
failure tests and the difference between source GPU scaling and hardware encoding.

## No shared audio coverage

The default room matrix now starts the CLI host with production `source: none`,
checks silent decoded audio alongside delivered video, then resumes capture through
a timed change. The actual UI scenario switches to None, verifies disabled device
controls and continued video, rejects a failed resume, then resumes successfully.
`PcmAudioDeviceTest` (without `--wasapi`) verifies bypassed factories, capture-owner
destruction, three off/on cycles, silence after restart, cancellation and pacing
without catch-up bursts. It also exercises the Windows None endpoint selector
without opening a physical audio endpoint. `PublicRoomSessionProof` checks real
Opus silence and resumption across four viewers with unchanged room/settings state.

Evidence: `build/webrtc/no-shared-audio-release/result.json` and
`build/webrtc/no-shared-audio-debug/result.json` passed **5/5** each; PCM lifecycle
checks passed in both configurations. Four-viewer Release evidence is under
`build/webrtc/no-shared-audio-four-viewer/`. These are correctness checks; their
drain deadlines are not latency measurements. Desktop/GPU acceptance was not rerun.

## Production room regression — one command

After building the application test targets and installing signaling-worker dev
dependencies, run (Python 3.11+ and Node.js required):

```powershell
python scripts/test-room-regression.py build/sdk-app-release build/webrtc/room-regression-new
```

This runs the actual presentation worker, native room/service integration, shared
CLI media controller, actual offscreen Qt widgets and delayed-mutation-ack recovery.
Every native room case uses the local production Worker through the existing
loopback-only fixture. Audio is synthetic and silent. No mouse/keyboard operation
is needed; default mode does not open desktop capture/presentation windows.

Use `--repeat 3` for complete repeated rounds. Add `--desktop` to include both
generated-window WGC/GPU UI and CLI scenarios; this requires a Windows desktop/GPU
session but remains silent and uses no physical input. Build directories select
Release/Debug; the runner never rebuilds or changes application defaults. It does
not include physical audio tests and has no audible-test switch.

Each case has a 90-second outer watchdog in addition to the fixture's 60-second
native watchdog. On Windows, a stdin-gated bootstrap joins a kill-on-close Job
before it can launch Node/native/Worker children. Closing the job cleans up the
whole tree on success, failure, timeout or cancellation. If job assignment fails,
no test child launches. Retained runner logs are capped at 1 MiB; runaway output
fails the case. The first failure stops further cases/rounds. Existing evidence
directories are rejected rather than overwritten.

`result.json` includes pass/fail, per-case timing/logs, executable/runner/fixture
hashes, nested Worker evidence and scenario metrics. Failed fixture evidence is
linked too. Logs, native executable hashes and Worker-bundle hashes stay in the
case directory. Repeated rounds restart scenarios; they are not a continuous
two-hour soak. Native hosts/viewers still share a process within each scenario.
No gaming input, network impairment, remote TLS/NAT, physical driver loss or
external latency acceptance is claimed.

Runner failure-path verification:

```powershell
python scripts/test_room_regression_tests.py
```

This checks exit propagation, log flooding, timeout and successful-exit descendant
cleanup, silent/default scenario selection and fail-fast evidence preservation.

## Component and scenario coverage

The room UI scenario now tests local profile migration and persistence: stable
randomized Guest names, all stream preset/resolution/FPS/bitrate combinations,
strict serializer/parser parity with CLI configuration, independent corruption
fallback, rejected invalid values and unwritable-file rollback. Actual browser and
session widgets save drafts without applying them, then create/rejoin a fresh room
and verify stream/playback defaults reach the runtime. Profiles live in temporary
directories; tests verify only nickname and versioned stream/playback keys exist.
No credentials, device IDs, capture handles or process IDs are persisted.

UI/CLI presentation now shares `backend/render/FramePresentationBackend` and
`FramePresentationSession`. The existing `video-frame-input` worker scenarios
exercise that shared policy. The Windows CLI scenario additionally covers real
GPU recreation after injected present/resize loss, bounded recovery, terminal title
persistence, explicit clear/reuse, fit/1:1 and fullscreen restore, minimize/drop,
audio-control callbacks, malformed/legacy frames and independent window closure.
These controls use direct messages only to generated test HWNDs, never SendInput
or physical keyboard/mouse events. Audio remains synthetic and silent.

The UI copy-link test waits for its button to become enabled, since backend Active
status/room ID can precede the Qt snapshot that enables the control.

`video-frame-input` now also tests the actual asynchronous presentation worker
with an injected backend: three recoveries despite intervening good frames,
250 ms drop-only backoff, fourth-failure exhaustion, nonrecoverable errors, explicit
clear, owner-thread destruction and shutdown during backoff. A stalled renderer
receives 1,000 retained frames; only the newest pending buffer survives and the
999 replaced owners are released. It uses Qt offscreen and no physical input/audio.
`RoomUiWindowsTests` additionally injects loss after real GPU presents, checks
three resource recreations, terminal exhaustion and clear/reuse. These remain
injected failures, not physical driver-removal acceptance.

Presentation checks validate immutable NV12 ownership through the shared UI/CLI
handoff: pointer identity on packed buffers, no `ToI420` call, padded-plane packing,
I420 fallback, rotation rejection, 100-frame overwrite ownership and late-callback
rejection after stop. Real media scenarios assert decoded frames use the retained
path with zero conversions/repacks. Windows UI/CLI variants additionally verify
successful presents and actual DXGI maximum frame latency of one. No physical
input or audible output is used. CPU decode/upload still exist; these checks do
not prove hardware decoding, GPU zero-copy or external image/input latency.

Viewer playback coverage uses only synthetic output endpoints. `pcm-adm-lifecycle`
checks exact gain, mute without device recreation, replacement, startup/first-write
rollback, Busy, queued and in-write cancellation, diagnostics, restart persistence
and owner-thread destruction. Actual-widget tests mute/unmute, change output and
volume, reject a missing output and verify continued frames and unchanged room
membership. The CLI test applies timed mute/replacement settings and checks their
results; strict parsing rejects host changes, bad volume and unordered times.
Physical output switching/unplug/driver-hang acceptance remains separate.

Live audio coverage is silent: `pcm-adm-lifecycle` exercises endpoint startup
failure, a real five-second missing-PCM timeout with old audio continuing, Busy,
successful changes, recording restart, cancellation during read and activation,
bounded backlog, and owner-thread destruction. The public four-viewer scenario
switches to silent PCM and back, checking decoded Opus on every viewer while frames
continue. Actual Qt widgets and timed CLI configuration use the same public API;
UI checks include a failed device selection and forbidden viewer operation.
Windows variants use generated WGC windows with synthetic capture/playout audio.
Do not enable physical playback to run these tests. Physical device loss/recovery
and external end-to-end latency remain separate acceptance gates.

Live source coverage uses actual widgets and scripted CLI changes. The UI scenario
switches to a larger source, checks decoded dimensions with unchanged room/member/
stream revisions, rejects viewer requests and confirms a failed replacement leaves
frames/audio flowing. The Windows variant switches between two generated windows.
Capture-owner tests cover failure, a real five-second missing-frame timeout, Busy,
stop cancellation and destruction on the owner thread. Decoder tests cover bounded
keyframe growth and oversized declarations; all routine audio remains synthetic.

Shared upload coverage uses the public four-viewer room proof for allocation,
leave/rejoin redistribution and positive per-peer transport-rate samples. Actual
UI tests pause video with a tiny allowance, verify decoded frames stop while silent
synthetic audio continues, then restore video and remove the cap. CLI tests check
configuration and reported applied allocation/rates. Pure settings tests cover
individual caps, 1–63 viewer arithmetic and counter reset/replacement behavior.

`room-v2-qt-ui` exercises actual Qt host/viewer widgets offscreen against the local
Worker, including coalesced edits, invalid settings, frame delivery and responsive
asynchronous stop/close. It uses silent synthetic audio and programmatic widget
actions, without physical input. [ROOM-UI.md](ROOM-UI.md) includes the explicit WGC
and real-renderer variant; offscreen rendering does not substitute for that proof.
The same scenario now covers the v2 browser: pushed room creation/count/removal,
password rejection/recovery, playback after join, nickname normalization and
nickname-only persistence, hidden subscriptions and rapid hide/show replacement.
Live nickname/policy changes are also checked during media delivery: stale revision
conflicts, draft reload, pushed directory changes and unauthorized/invalid edits.
A signaling barrier deterministically checks mutation queue bounds and pending
result resolution during shutdown. `room-v2-mutation-ack-recovery` delays replies
past the real deadline, checking uncertainty, no automatic retry, late-response
isolation and continued frames. Browser tests exercise copy/paste room links,
credential/URL rejection and admission through the configured service. Clipboard
checks run only in offscreen Qt, never against the real Windows clipboard.
See [CHECKPOINT-B.md](CHECKPOINT-B.md).

The application suite includes `room-v2-cli-entry` (actual executable dispatch,
HTTPS enforcement and secret-free errors) and `room-v2-cli-media` (shared CLI
controller, local Worker, silent synthetic media, live changes, cancellation,
admission failure and bounded preview conversion). Use
`ctest --test-dir build/sdk-app-release -R "^room-v2-cli-" --output-on-failure`
after building. [ROOM-CLI.md](ROOM-CLI.md) documents reproducible finite runs and
the explicit generated-window Windows capture/preview variant.
`room-v2-cli-deployment` also stages the CLI alone in a fresh directory, verifies
its Qt networking/TLS dependencies and executes entry validation there.

Routine runs are silent: omit `-AudioDevice`. The runner explicitly resets the
cached device-test option OFF, while retaining synthetic PCM/Opus checks that
never play to speakers. Audible WASAPI tests require both `-AudioDevice` and
`-AllowAudibleTests`; only use them when the user requests audible testing.
Older commands below containing `-AudioDevice` describe historical device runs.

The public session proof also updates all four live viewers to fixed 320x180,
20 FPS and a manual 1 Mbps limit, verifies applied/source-observed revisions and
decoded reduced frames, restarts a peer, rejoins with the updated preferences,
then restores 640x360/30 FPS/Auto bitrate. It rejects invalid preferences, viewer
settings commands and commands after Stop. This checks correctness, not remote
latency or achieved bitrate/FPS. Source-observed is not a remote presentation ack.

The public-session scenario now uses NativeRoomRuntime itself; only source/audio
dependencies and evidence collection are diagnostic. It includes an authenticated
restart request, fresh rejoin and a single-viewer delivery failure while healthy
viewers continue. Capture startup failure must publish a Media error and drain.

For the Windows binding, build WindowsRoomSessionProof and run:

```powershell
node signaling-worker/tests/run-native-service.mjs build/sdk-proof-release/WindowsRoomSessionProof.exe build/webrtc/windows-runtime-check windows-media
```

This creates a test-owned animated window and captures it through WGC. Audio is
synthetic; no mouse/keyboard input occurs. It requires a Windows graphical session.
In the Codex sandbox WGC reports a missing capture service; the same test passes
outside that sandbox. With `-LiveCapture`, CTest also registers
`windows-room-session-media`. The regular CPU-only headless path remains available.
The Windows test permits codec fallback and does not assert hardware-only encoding.

`public-room-session-media` exercises the public v2 RoomSession API against an
isolated real Worker. It creates a host and four independent viewers, verifies
decoded H.264 and audible Opus separately for each viewer, leaves cleanly, cancels
admission, coalesces stop and holds a media-drain barrier to prove resources remain
alive while stopping. It also checks production rejects plaintext loopback and
unlisted room admission works. No Qt/event/SDP pump is exposed to the caller.
Build PublicRoomSessionProof and run the CTest entry; the usual native-service
harness records executable/Worker hashes and enforces a 60-second watchdog.

`room-media-session` tests role-based membership, stale socket generations,
peer-failure isolation without retry storms, asynchronous retirement/rejoin gates,
cancelled pending peers, startup errors and event-count/byte-pressure termination.
The authenticated four-viewer scenario now reports `shared_room_session: true`:
its snapshots and SDP/ICE use the same session routing as the backend target.
The coordinator advances it automatically; the diagnostic only observes progress.

`media-engine-lifecycle` exercises ten native engine lifetimes, wrong-thread
rejection, dependency validation, preserved ICE policy, partial-track rollback
and reliable control/unreliable transient-channel policy. It also verifies native
peer channel rejection, repeated close and rejection of late ICE revival. The authenticated
four-viewer scenario also uses this same backend factory/thread implementation.
Run it with CTest alongside `room-backed-four-peer-media`; neither requires
physical input. These are correctness checks, not latency or resource acceptance.

Build the proof targets using [BUILD.md](BUILD.md), then run from the repository
root. No mouse, keyboard, window, audio device or GPU is required for these two
commands; the Windows software Media Foundation codec must be available.

Quick smoke (includes a real ICE restart; allow up to the per-process watchdog):

```powershell
python scripts/test-headless-media.py build/sdk-proof-release build/webrtc/headless-smoke
```

Longer regression (100 capture restarts, 100 coordinator restarts, 20 one-viewer runs and three four-viewer runs):

```powershell
python scripts/test-headless-media.py build/sdk-proof-release build/webrtc/headless-regression --regression
```

Use a fresh output directory each time. Both commands return nonzero on failure.
To include native room/directory WebSocket checks, also build application tests:

```powershell
python scripts/test-headless-media.py build/sdk-proof-release build/webrtc/media-and-room-smoke --room-build-directory build/sdk-app-release
```

The optional room checks include native HTTP admission plus a real socket server and a separate networking thread,
including the actual 30-second heartbeat and 10-second missing-pong timeout. It
has a 75-second watchdog. No deployed service or credentials are needed.

Each child has a 45-second watchdog, or 60 seconds for a four-viewer scenario;
a timed-out child is killed and reaped. These
executables do not spawn descendants. Results include executable SHA-256, exit
code, elapsed time, timeout status, logs and capture timing percentiles in
`result.json`. The runner stops on the first failure and preserves its evidence.
WebRTC transport logging remains disabled to avoid recording signaling secrets.

Current coverage:

- Optional `RoomSocketTests`: authorization headers, snapshot readiness, pushed
  state/signals, room/self/role checks, one-shot resync, heartbeat/reconnect,
  directory lifecycle and saturated writes. This tests native transport, not
  server authorization, media connection-ID routing or Cloudflare cost.
- Optional `RoomAdmissionTests`: create/join bodies, typed rejection, identity/token
  validation, redirect/cookie suppression, bounded responses, cancellation and
  the real ten-second timeout. Socket tests also verify admission role binding.

- `HostPeerOwnerTest` exercises automatic deadlines/restart, failed-completion
  isolation, capture detachment, healthy media progress, stale requests and 25
  owner restarts with weak timers still queued. It never pumps WebRTC messages
  or explicitly ticks the registry. Four real peers now negotiate concurrently
  through that scheduled owner; restart and rejoin advance ready SDP futures
  without blocking peer callbacks. Signaling delivery is still in-process.

- The shared `SignalingExecutor` owns the WebRTC event loop for all media
  scenarios. Its headless test covers FIFO execution, native event callbacks,
  task failure, 64-command pressure, cancellation, closure destruction on the
  owner thread and repeated/self-requested shutdown. `--negotiation-only` also
  completes actual offer/answer operations using external future waits, without
  manually pumping messages. Other media diagnostics retain nested waits on
  the owned thread; those waits are not the application integration pattern.

- `PeerNegotiation` is built as the same library in the application and proof.
  Real offer/answer/restart paths now use its asynchronous operations. A dedicated
  `WebRTCProof --negotiation-only` scenario cancels 25 pending offers, destroys
  their owners, pumps late callbacks, verifies a fresh negotiation succeeds,
  and checks busy/stale requests, malformed/oversized SDP and native failures.
  It runs once in smoke/regression and also before the four-peer scenarios.

- A capture-bound peer registry automatically removes subscriptions on terminal
  failure and explicit removal. Cleanup futures are polled without waiting on
  signaling; queue-pressure/cancellation errors retry on later ticks. A blocked
  delivery/filled-command-queue test checks pending status, retry, healthy frame
  progress and exactly-once close. The four-peer scenario injects a terminal
  peer failure and verifies capture detachment before rejoin. This is a lifecycle
  fault injection, not an actual network failure.

- Capture subscription commands carry both host-session and viewer-connection
  generations. The coordinator test rejects retired attachment and cleanup
  commands after viewer replacement; the real four-peer rejoin scenario sends
  stale capture removal and verifies that the replacement keeps decoding.

- `HostPeerRegistry` owns peers behind `IMediaPeer`, dispatches lifecycle restart
  and close actions, retains failure snapshots and rejects retired-generation
  requests. The real four-peer scenario uses this owner for restart, removal,
  replacement and stop. A deterministic test covers dispatch failure isolation,
  timed-out close, stale/reused generations and exactly-once close/destruction.
  The proof adapter queues restart work for its local SDP driver; authenticated
  room delivery and the application event loop remain integration work.

- `PeerConnectionLifecycle` enforces the initial 20-second connection deadline,
  stale connection-event rejection and a rolling three-restarts-per-minute
  budget. Deterministic tests cover deadlines, duplicate disconnects, transient
  recovery, backoff, budget expiry and close without sleeping. Actual peer ICE
  state callbacks feed the policy; the four-peer scenario requests a real
  host-offered ICE restart with fresh credentials, preserves viewer settings
  and checks continued media on that viewer and healthy peers. JSON includes
  restart negotiation and media-check elapsed time; these are local scenario
  timings, not display latency or network-outage recovery acceptance.

- ICE candidates travel separately from SDP through the reusable bounded
  `IceCandidateHandoff`. Each direction waits for successful local and remote
  description application, rejects stale generations and closes its callback
  on teardown. SDP is asserted to contain no candidates. The dedicated test
  covers ordering, stale/closed delivery, overflow, invalid fields and delivery
  failure; real single/four-peer media tests exercise the handoff. This remains
  in-process signaling, without STUN/NAT or room-service transport coverage.

- Production `HostMediaSession` serializes capture/membership commands, assigns
  operation/session IDs and joins workers on stop. Its headless test covers
  100 restarts, stale commands, isolated callback failure, startup failure and
  cancellation despite a full command queue. The four-peer proof now uses this
  coordinator for capture and subscriber lifetime. Peer creation/signaling and
  the application facade remain outside it. Smoke runs ten child processes;
  regression runs 31. See [CHECKPOINT-B.md](CHECKPOINT-B.md).
- [COMPARISON.md](COMPARISON.md) defines the matched before/after scorecard.
  These checks establish regression coverage, not superior end-to-end latency.

- Production `CaptureSession` owns source construction, acquisition, recovery,
  callback delivery and source destruction on one worker. Both synthetic video
  and WGC adapters use this lifecycle.
- Headless owner scenarios exercise 100 fresh session IDs, initially disabled
  delivery, slow consumers, three device rebuilds, terminal exhaustion, source
  closure, startup timeout, callback failure and cancellation during backoff.
- Production `CaptureDistributor` gives each viewer its own delivery worker and
  one replaceable pending frame in addition to any frame already being consumed.
  Headless scenarios run four consumers with slow and failing consumers, remove
  a slow viewer during capture, and verify stale session/device/sequence rejection
  plus replacement-subscription isolation. Its JSON includes delivered/replaced
  counts and maximum capture-to-handoff age.
- The software media proof uses the same capture session with paced owned CPU
  pixels, actual H.264/Opus PeerConnections, offscreen decoding and three DTLS
  data channels. The longer command repeats complete media-process teardown.
  Both synthetic and WGC media proofs now deliver through the distributor.
- The four-viewer scenario creates four real host-to-viewer PeerConnection
  pairs with independent source wrappers, video senders, H.264 and three data
  channels per connection. It slows the fourth source handoff, checks continued
  healthy decoding, applies a 200 kbps WebRTC sender limit only to that viewer,
  resumes its delivery and replaces its complete connection while the others
  continue. Results include per-viewer decoded counts, pending replacements and
  successful rejoin. Opus is shared and checked at the aggregate playout sink.
- The production `CaptureVideoSource` wrapper preserves owned input dimensions
  and acquisition timestamps across handoff/conversion, and declares screen
  content with denoising disabled. The scenario includes a small-frame and
  delayed-timestamp check of that bridge.
- `StreamSettingsTest` checks the production Auto/Manual mapping, conservative
  initial-rate calculation, invalid/replayed revisions, fixed letterboxing and
  independent WebRTC pixel/FPS requests. It verifies downward adaptation and
  recovery without source upscaling. The four-peer scenario now applies its
  200 kbps setting through `ViewerStreamSettings`, checks no minimum bitrate was
  introduced, and waits for the source to observe that settings revision.

Capture-to-callback percentiles use one local monotonic clock and include pixel
generation. They do not measure encode, network, decode, display or gaming input
latency. Source frame pacing skips missed opportunities rather than producing a
catch-up backlog. Normal stop preserves frames already owned by consumers;
device loss invalidates resources from the failed device generation.

The WGC checks remain separate and require an interactive Windows desktop/GPU:
`CaptureRecoveryTest --live` and `WebRTCProof --live-capture`. They create their
own windows and need no manual input. Run through a process watchdog, as a
native driver/RPC hang cannot be preempted by the capture session stop token.

## Remaining integration

This is the first shared production component, not the complete session runner.
The normal UI/CLI still uses legacy media pending Gate A. Local proof peers share
one process. Separate host/viewer processes, production session-facade routing,
complete settings behavior, adaptive multi-viewer isolation, scripted authorized gaming input,
network impairment and resource-growth reports remain to be implemented.
Synthetic success does not satisfy WGC, Internet or external latency acceptance.
The four-viewer delay acts at the source handoff, not the decoder or network.
The sender-limit check verifies parameter independence and continued fixed-size
decoding; it does not prove congestion recovery, actual wire-rate limits or
Auto/Manual product semantics. Four software encoders require sufficient CPU.

The settings core now implements the source/RTP mapping, but complete product
semantics still require the coordinator/UI and impairment acceptance. Fixed
resolution keeps its canvas; manual FPS targets its rate while encoded-frame
drops remain allowed; manual bitrate is an upper operating limit subject to
WebRTC congestion control. Auto source requests use WebRTC's `VideoAdapter`.
No measured-bandwidth controller was added. Accepted sender settings queue the
source revision; `observedRevision` reports when capture delivery sees it, not
when a remote display has rendered it.

GPU resizing now keeps NV12 planes on the GPU, with per-device submission bounds,
owned output and quarantined CPU fallback. Matching dimensions preserve the native
frame. `StreamSettingsTest --gpu` verifies pixels, viewer isolation and fallback;
it requires D3D11 but no preview window. The same test without `--gpu` includes a
deterministic stalled-completion submission-bound check. See GPU-SCALING.md for
hardware-encode and generated-WGC room tests. End-to-end timing remains acceptance
work; CPU/synthetic settings checks need no GPU.

Coordinator contract: assign a fresh nonzero session ID, serialize owner calls,
keep frame callbacks short, reject obsolete IDs downstream, and join before
releasing callback state. Callbacks may request cancellation but must not join
or destroy their own session. Hold `WindowsMediaRuntime` across joined Windows
capture sessions, after UI STA initialization. The capture factory must create
its source on the owner thread; it must not return an object constructed on the
UI thread. There is no unbounded frame handoff queue in this component.

Stop the capture owner before stopping its distributor. Removing a subscriber
joins its delivery worker before returning. Serialized membership calls may
reuse a viewer ID only after removal completes; no old callback then survives
into the replacement. A device generation change replaces pending frames but
does not preempt an already-running callback: downstream consumers must check
the sample generation and honor retired GPU resources. Slow callbacks cannot
block other delivery workers, but removal/shutdown must wait for an in-flight
callback, so native-call watchdogs remain necessary. The current admission
ceiling is 63 subscriptions, matching the planned service abuse ceiling.
# Local v2 service runtime tests

## Compiled native client against workerd

After installing the signaling-worker npm dependencies and building application
tests (UI optional), run from the repository root:

```powershell
node signaling-worker/tests/run-native-service.mjs build/sdk-app-release/RoomServiceTests.exe build/webrtc/native-worker-evidence
```

CMake also registers `room-v2-native-worker` when Node and Miniflare are available;
it prints an explicit status message if prerequisites are missing. The executable
is built independently of that registration. This test launches an isolated local
workerd/SQLite service and exercises the production Qt admission/socket clients.
It covers directory push/resync, create/join, passwords/capacity, revision-bound
edits/conflicts, signaling, host reconnect, visibility, kick and closure. It does
not instantiate a media PeerConnection; SDP/ICE payloads are synthetic.

The harness binds only numeric loopback and uses the clients' explicit plaintext
diagnostic opt-in. A test-only entry supplies the HTTPS URL and trusted edge IP
normally provided by Cloudflare; production HTTPS checks are unchanged. Remote TLS
and actual edge deployment are not covered. The native process has a 60-second
watchdog; sockets/runtime are disposed after it exits. Each run creates a unique
artifact directory with executable/Worker-bundle hashes, timing, exit status,
limitations, result JSON and a bounded native log. No physical input is required.

Snapshot events now include their accepted revision alongside the copied payload,
so a consumer can issue expectedRevision edits without reading live network state.

Background-delivery coverage now deliberately holds a directory request while
admission, attachment, resync, edits and signaling execute. It tests newer updates
and host closure racing with the old acknowledgement, plus the actual five-second
fetch abort and eventual retry. The 1.5-second control-operation watchdog is a
regression bound, not a gaming-latency measurement. Expiry injection and observations
respect the production state serializer and asynchronous cleanup semantics.

Directory coverage now includes real pushed snapshots/deltas, publication failures
before and after commit, closure retry, version fences, lease expiry/renewal,
provisional capacity and host reconnecting, no per-room listing calls, resync floods,
and a 500-room maximum-name snapshot. `tests/directory.test.mjs` injects faults and
deadlines only in its in-memory test entry. See CHECKPOINT-C.md for remaining
real-time scheduling, hibernation, queue-pressure and native integration limits.

From `signaling-worker`, run `npm run typecheck` then `npm test`. The suite bundles
the production v2 Worker into an isolated Miniflare/workerd instance with SQLite
Durable Objects and real HTTP/WebSocket traffic. It needs no account, deployment,
physical input or desktop capture. The runtime test has a 60-second watchdog and
disposes sockets/runtime on completion. It tests concurrent admission, credentials,
socket replacement/reconnect, automatic pong and global capacity. Expiry is injected
through a test-only entry point and exercises the production alarm handler; actual
hibernation/alarm timing and native-client/media integration remain separate work.
# Authenticated four-viewer media (2026-09-16)

RoomSessionCoordinator now owns automatic room/media event dispatch. The scenario
wait loop only observes state; it does not drain room events or forward SDP/ICE.
A five-second observation pause during ICE restart verifies independent progress,
reported as `autonomous_dispatch`. This is not a five-second latency target.
`room-session-coordinator` tests bounds, late send failures and cancellation.
Pending-signaling metrics now come from the shared coordinator. Commands below
are unchanged; expect roughly 35 seconds for the real-room scenario.

Managed room peers now exercise HostPeerOwner's scheduled restart budget and
asynchronous capture retirement. The initial offer waits for capture attachment;
capture startup and final stop are awaited outside signaling. The
`scheduled-peer-owner` test holds a delivery callback open during BeginStop and
checks that signaling remains responsive until shutdown completes.

RoomNetwork now owns the Qt networking loop; the scenario never calls
QCoreApplication::processEvents. RoomPeerRoster drives peer creation/removal from
authenticated host snapshots, including socket disconnect/reconnect, kick/rejoin
and room-close cleanup. `room-network-ownership` separately tests bounded queues,
overflow, coalesced stops, cancellation and stale roster isolation headlessly.
The outer diagnostic still waits on command futures; it is not the normal UI/CLI
session coordinator. Commands for running the scenario remain unchanged.

The backend now schedules negotiation itself; the diagnostic does not poll native
negotiation futures. Additional coverage cancels before the first scheduled tick
and lets an unanswered peer hit its automatic 20-second deadline while healthy
media continues. Expect roughly 30 seconds for this scenario. Source relocation
to backend/ and frontend/ does not change the command below.

After building the hardware/audio-enabled WebRTC proof preset (requires the pinned
Qt 6.10.3 Core, Network and WebSockets installation), run from the repository root:

```powershell
node signaling-worker/tests/run-native-service.mjs build/sdk-proof-release/RoomMediaProof.exe build/webrtc/room-media-evidence media
```

This one command starts isolated workerd, creates/joins using native admission and
room sockets, negotiates real four-viewer H.264/Opus through authenticated signaling,
checks slow-viewer isolation and twelve encrypted data channels, restarts ICE,
kicks/rejoins a viewer, closes the room and cleans up under a watchdog. No physical
mouse/keyboard input is used. CTest includes it as `room-backed-four-peer-media`.
Each evidence directory includes JSON verdict/metrics, executable and Worker hashes,
and a native log. Signaling queue peaks and sent candidate counts are reported.

Capture/audio are synthetic; audible PCM is mixed receiver evidence. This test uses
shared production negotiation/room components with diagnostic orchestration, not
the unfinished normal UI/CLI facade. It does not establish desktop/GPU capture,
remote TLS/NAT, gaming input/image latency, leak bounds or service-cost acceptance.
## Sender diagnostics evidence — 2026-09-17

Release and Debug application builds succeeded. The final silent production
matrix passed 5/5 in each configuration, including actual widget pause/resume,
selected-peer preservation and CLI freshness reporting:
`build/webrtc/diagnostics-final-regression-{release,debug}/result.json`.
`build/sdk-proof-release/StreamSettingsTest.exe` passed counter reset, replacement,
zero-rate, exact three-second expiry and independent-generation checks.

The optional desktop matrix did **not** pass: Windows CLI `PreviewLifecycle`
timed out at its three-second presentation wait (RoomCliTests.cpp line 77), before
diagnostics integration. It repeated in the isolated desktop check. Evidence:
`build/webrtc/diagnostics-regression-20260917/result.json` and
`build/webrtc/diagnostics-desktop-check/`. The cause is not established; desktop
GPU acceptance remains unresolved. No timeout was relaxed or success fabricated.
All tests used silent synthetic audio and no physical input.

## Local presentation outcomes — 2026-09-17

The isolated Windows CLI recheck passed before renderer behavior changed:
`build/webrtc/presentation-diagnose/`. This does not identify the cause of the
earlier failure. Timeout failures now include caller stage, frame/error/recovery
counts, last graphics error, drop reason and visibility/minimized state.

Headless worker tests exercise busy/occluded/minimized/unavailable/unknown outcomes,
ensure these consume no recovery budget, retain error codes through backoff and
terminal state, and clear the last error on explicit restart. Windows CLI tests
verify real minimize/resume and injected native HRESULTs. Room UI tests verify the
actual viewer diagnostics label updates. No waits or acceptance bounds were relaxed.

The Debug seven-case desktop-inclusive matrix passed in 61.452 seconds:
`build/webrtc/presentation-outcomes-debug/result.json`. A preceding Release
seven-case matrix also passed (`presentation-outcomes-release/result.json`).
The final Release binaries, including periodic CLI reporting and the viewer-label
assertion, passed 7/7: `build/webrtc/presentation-outcomes-final-release/result.json`.

## Receiver decoder telemetry — 2026-09-17

Final Release and Debug application builds passed. Both silent five-case matrices
passed, including actual host UI receiver dimensions, null/stale serialization,
CLI reports after resolution changes and the existing mutation/recovery tests:
`build/webrtc/receiver-checked-{release,debug}/result.json`.

NativeRoomRuntimeTests passed in both configurations with bounded wire validation,
all truncations, canonical unknown/zero FPS, replay, generation, flood and exact
three-second expiry checks. Release StreamSettingsTest passed collector extraction,
missing-data and mailbox-isolation checks. The final real four-viewer proof passed
in 12.881 seconds (`build/webrtc/receiver-checked-four-viewer/`), including stale
reports while video keeps decoding, report recovery, ICE restart and rejoin.

The optional desktop matrix failed before media admission in the CLI preview:
`receiver-telemetry-release/` recorded visible=0, outcome=occluded, 193 occluded
drops and zero errors/recoveries. Preview creation now explicitly ensures visibility
despite the fixture's hidden launch state; the visibility assertion passes.
`receiver-final-release/` still recorded visible=1, outcome=occluded, 194 drops and
zero errors. Thus this correction does not establish a fix for desktop occlusion;
that GPU acceptance result remains open. No timeout or success criteria were relaxed.
Tests remained silent and used no physical keyboard/mouse input.

## Sender/network details — 2026-09-17

Release and Debug application builds pass with the extracted PeerDiagnosticsWidget.
Release's five-case matrix passed at `build/webrtc/network-details-release/result.json`.
Debug's final five-case matrix passed at `build/webrtc/network-details-debug/result.json`,
including the live payload/RTT assertion.
The final Release CLI scenario additionally requires a real nonzero payload rate
and numeric selected-path RTT (`build/webrtc/network-details-live-cli/`).

The Release collector proof passed with constructed WebRTC reports for selected
versus unused ICE pairs, RTP rate units, actual zero, counter/stream replacement,
linked RTCP loss/jitter, stale expiry, missing fields and nonfinite/out-of-range
values. UI scenarios exercise the actual nonmodal popup, peer-pinned updates,
clearing after departure, duplicate-name IDs and the above-four capacity warning.

These tests are silent and do not send physical input. No desktop GPU rerun or
external latency/performance acceptance is claimed; the previously documented
occlusion result remains open.
All runs used silent synthetic audio and test-owned windows/messages. Physical
driver failures, end-to-end latency and the earlier intermittent timeout cause
remain open; local renderer counters do not close those gates.
## Stage 2–4 unattended closeout additions — 2026-09-18

### Paired legacy/v2 comparison

Build the `BackendComparison` Release target and run `scripts/compare-backends.py`;
the exact command and measurement boundaries are in COMPARISON.md. It captures
only an owned generated window, injects no input, and emits no audio. It needs
an active Windows capture desktop but no mouse/keyboard interaction. The runner
uses fresh processes, bounded deadlines, preserved failed logs, alternating
backend order and strict workload/measurement validation. The default is 24
short runs across three scenes and one/four viewers. `backend-comparison-evidence`
tests reject mismatched settings, incomplete samples and unsupported conclusions.
The fixture must use paced silent playout (`DiscardPcmPlayout`); a no-op write
spins the ADM and invalidates CPU/video timing. Reports without that explicit mode
are rejected. Native `retained-only` runs isolate pixel-consumer overhead but
cannot pass the image-latency/quality validator. Private high-resolution media
waits are tested with Windows timer throttling explicitly enabled in the isolated
test process; no machine-wide timer or power setting is changed.

Do not call its scene-to-CPU-consumer age physical display latency. CPU readback,
codec defaults, configured ceilings versus measured bitrate and source update
rates are explicit comparison boundaries, not hidden normalization assumptions.

For encoder/fairness controls, use `scripts/compare-codec-controls.py` with the
same executable/origin/output arguments. It runs software and hardware variants
for both backends, with one/four viewers in forward/reverse order. Legacy uses
its existing low-latency mode. Both processes honor requested timer precision;
the production legacy path also shares the precise waits and single-submission
hardware deadline. Source/binary/report hashes and source snapshots distinguish
the pre-fix and improved-legacy baselines. See HARDWARE-ENCODING.md.

For capture/decoder attribution, `scripts/compare-pipeline-stages.py` uses the
same executable/origin/output arguments. Its twelve short cases isolate capture
configuration, fixed-rate scheduling, prerender smoothing and codec stages;
`--case codecs` selects four repeated post-fix traces. Capture-only and traced
runs cannot pass ordinary image-comparison validation. Sender/receiver RTP
epochs are not assumed identical. See [CAPTURE-LATENCY.md](CAPTURE-LATENCY.md)
for the capture decision and the shared complete-picture decoder fix.
`MfDecoderAdapterTest` now requires immediate output for every complete frame,
including across CPU/GPU resize and reset; successful drain alone cannot pass.

Software-throughput diagnostics add bounded `encoderControlTrace` rate/FPS and
keyframe records to `--case codecs`. Run `MfCodecProbe --rates` for the direct
1080p60 software codec's 12/6/3/12 Mbps assignments; an optional second argument
writes only its generated H264 scene. Its bitrate is measured against the codec
timeline, not network delivery, and successful completion does not certify a
hard bitrate ceiling. See [SOFTWARE-THROUGHPUT.md](SOFTWARE-THROUGHPUT.md).
The encoder lifecycle test parses the actual software PPS to require CABAC,
rejects transform restarts for small FPS jitter, and requires a keyframe on a
real 60-to-30 change. GPU fallback/lifetime tests verify twelve readbacks reuse
one staging allocation, retained images stay unchanged, and resizing replaces
the scratch texture instead of accumulating a cache. All remain silent.

Run WGC/live-admission comparisons with access to the active Windows capture
desktop and authorized test service. A restricted process can build and pass
component tests while failing WGC activation or room admission. Preserve those
failed attempts, resolve the execution environment, and start a fresh output
directory; never interpret readiness/admission failure as a latency measurement.

Release/Debug `stage23-final-*/result.json` pass **12/12 each**, including input
pressure/timing and redacted report export. See SESSION-REPORTS.md.
`network-timestamps-{release,debug}/result.json` pass **6/6 each** with encrypted
packet impairment and separate receiver processes. See NETWORK-IMPAIRMENT.md for
commands, preserved failures and the unresolved latency boundary. SERVICE-COST.md
records real ten-room heartbeat/listing invariants and the eight-hour operation model.
Earlier runs were blocked while the interactive desktop was `Screen-saver`.
Do not unlock it or synthesize user input to run generated-desktop tests; wait
for an available capture desktop. Physical acceptance stays open.

Final batch details are in UNATTENDED-RESULTS.md. Both final `closeout-final-*`
headless matrices pass 12/12; final rebuilt `lifecycle-latest-*` runs pass 60 seconds
plus ten-second idle in both builds. The 7200-second software run's original data
passes reevaluation after fixing an ideal sample-count assumption; raw and derived
reports are preserved with hashes. That batch's network results were Release 5/6
and Debug 6/6, including a retained Release bandwidth-recovery failure. Later
transport/pacing fixes and fresh packet results are recorded in
CONGESTION-RECOVERY.md and COMPARISON.md; neither erases the original failure.
