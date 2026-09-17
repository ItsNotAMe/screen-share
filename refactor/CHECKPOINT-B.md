# Checkpoint B evidence

## Shared application shell — 2026-09-17

Both existing opt-in UI entry points now use RoomApplication and the normal
AppShell. Browser/session navigation retains one window; returning destroys the
old page and resumes the pushed directory. Title-bar/Alt+F4/programmatic close
drains media and directory asynchronously, including admission cancellation and
repeated close requests. Screen-awake state is released on return/exit, and shells
without a revoke handler no longer reserve the global panic shortcut.

Release/Debug builds and final silent regression matrices passed **5/5 each**:
`build/webrtc/room-shell-final-{release,debug}/result.json`. Actual shell tests cover
failed join/return, repeated sessions, saved defaults, source/audio recovery,
directory connection counts, page removal, live shutdown and immediate admission
cancellation. The Release `ScreenShareUi --self-test` also passed.

The optional Windows UI run failed at NativePresentationRecovery's first wait for
three presented frames, before any shell scenario ran (15.510 s):
`build/webrtc/room-shell-windows/native-service-3ac5a76d-1111-41d0-9007-37859d3f4741`.
It does not establish desktop shell/presentation acceptance; no assertion was
removed or timeout extended. Ordinary home/create/join/control and CLI action
adoption, Stage 2 completion and default cutover remain open.

## Audio endpoint failure isolation and recovery — 2026-09-17

Reported host capture/viewer output startup and live I/O failures now preserve the
room and video. Failed endpoints are released on their owner; paced silence/discard
keeps workers available for explicit same-device retry. Public health, separate UI
error feedback/retry actions, and CLI health/scripted recovery use the same controls.
There is no automatic retry or physical-device substitution. See AUDIO-RECOVERY.md.

Release/Debug application builds and PCM recovery tests passed; silent matrices
passed **5/5 each** (`build/webrtc/audio-recovery-{release,debug}/result.json`).
PCM tests cover startup/live loss, endpoint release/thread affinity, failed retries,
same-device recovery, retained volume and stop. Actual UI/CLI tests validate health
and action/status behavior while frames continue. Final synthetic four-viewer proof
passed in **18.248 s** under `build/webrtc/audio-recovery-four-viewer-final/`, with
all-viewer host-capture failure and one-viewer playback failure/recovery.
The generated-window Windows capture version also passed in **18.635 s** under
`build/webrtc/audio-recovery-windows-room/`, using silent synthetic audio.

Physical audio unplug/driver hangs, microphone processing and multichannel handling
remain open. STAGE-2-REMAINING.md now groups the remaining Stage 2 delivery work and
separates Stage 3/4/5 dependencies; this feature does not close the stage.

## Bounded per-viewer GPU scaling — 2026-09-17

The native source now scales/letterboxes NV12 on the capture device and preserves
owned GPU textures into MF encoding. The GPU command backlog is bounded to four
submissions; pressure drops frames without readback. Unsupported operations
quarantine scaling on that device and retain the CPU fallback. Source snapshots,
UI details and CLI JSON expose the scaling path and active image rectangle.
Dropped frames no longer acknowledge settings whose dimensions were never emitted.
Implementation details and remaining acceptance: [GPU-SCALING.md](GPU-SCALING.md).

Release and Debug application builds, `StreamSettingsTest --gpu` and
`MfHardwareAdapterTest` passed. Final logs:
`build/webrtc/gpu-scaling-bounded-{pixels,mf}-{release,debug}.log` and
`build/webrtc/gpu-scaling-bounded-{app,proof}-{release,debug}-build.log`.
The pixel/ownership test covers concurrent independent viewers, zero scaling
readbacks before explicit pixel validation, color/orientation/letterboxing, Auto
adaptation, controlled unsupported-resource fallback, retirement, and a portable
stalled-completion admission-bound scenario.

The final generated-window Windows four-viewer proof passed in **14.772 s**:
`build/webrtc/gpu-scaling-bounded-windows-room/native-service-f50a88e4-75cd-4914-8367-54b553d00780`.
All four peers reported GPU scaling and zero scaling-readback fallbacks after the
live dimension change while real decoded media continued. The initial sandboxed
capture run failed during capture startup; the desktop-access run passed. This
does not resolve the separately recorded preview swap-chain occlusion failure.

The final silent UI/CLI/service regression matrix passed **5/5 Release and 5/5
Debug**, including live source-path/rectangle diagnostics and the dropped-revision
fix: `build/webrtc/gpu-scaling-bounded-{release,debug}/result.json`.
Tests used no physical audio or input. These results establish the scaling path
and its bounds on the tested GPU, not a measured gaming-latency improvement.

## Device-free shared audio selection — 2026-09-17

Host None selection is integrated across the native runtime, Windows adapter,
opt-in browser/session UI and initial/timed CLI JSON. AudioSwitchControl resolves
None to a portable paced silent endpoint before any device factory is called.
Successful handover releases the old endpoint on its worker; failed resumption
keeps silence. Recording restart preserves None. The retained track avoids media
renegotiation and retains the existing conservative upload audio reserve.

Release/Debug application builds and targeted PCM/public-room proof builds passed.
Both default silent room matrices passed **5/5**:
`build/webrtc/no-shared-audio-{release,debug}/result.json`.
`PcmAudioDeviceTest` passed without physical WASAPI playback in both configurations.
The four-viewer Release proof passed in **13.974 s**, artifact:
`build/webrtc/no-shared-audio-four-viewer/native-service-2d15438b-c864-4ea9-a759-1de4a6df7bfe`.
Coverage includes initial silent video, UI live disable/rollback/resume, owner-thread
device release, no factory fallback, restart persistence and bounded silent pacing.
Physical driver hangs/unplug, microphone processing, latency and previously recorded
DXGI occlusion remain open. No physical audio or mouse/keyboard input was used.

## Validated local profile defaults and browser/session integration — 2026-09-17

RoomProfile now persists versioned, allowlisted stream settings and playback
volume/mute alongside the canonical nickname. New/invalid nickname profiles receive
a stable saved Guest-xxxxxxxx name generated from the system RNG; valid existing
names are preserved. Stream serialization and parsing use the same validation as
CLI JSON, covering all presets/modes/dimensions/limits and aggregate allowance.
Corrupt/unknown/oversized groups fall back independently. Invalid saves retain old
values; failed QSettings writes restore the previous in-memory key.

Browser-created sessions load validated defaults before runtime construction and
offer explicit save-for-new-session controls. Saving a draft does not apply media
changes or advance the stream revision. New room/viewer sessions consume the saved
stream settings and playback volume/mute. Standalone JSON UI/CLI entry points stay
explicit and do not load/write local defaults. No credentials, room/service identity,
device IDs, process IDs or capture handles are persisted. Shared Apply/Save reads
the same widget state, with independent local-save feedback.

Validation, temporary profile files and silent synthetic audio:
- Full application suites: **19/19 Release (90.57s), 19/19 Debug (90.62s)**.
- Complete production regression including generated-window WGC/GPU scenarios:
  **7/7 cases**, `build/webrtc/profile-regression-20260917/result.json`.
- Profile checks cover all 24 preset/resolution/FPS-mode/bitrate-mode combinations,
  serialization parity with CLI, absent optional limits, invalid-save preservation,
  oversized/malformed/type-invalid/unknown-field fallback, independent group
  retention, randomized nickname stability and real unwritable-path rollback.
- Actual browser/session widgets save defaults without changing active settings,
  reject an odd-width draft, close/recreate the room and viewer, and verify selected
  stream width and playback volume/mute in both controls and runtime status.
- Saved-key assertions exclude room/password/source/device data; session nickname
  edits remain distinct from saved profile identity.

Profile persistence is implemented. This does not close default-shell adoption,
gaming input, physical-device/resource or external latency acceptance.

## Reconciled implementation checks and production regression matrix — 2026-09-17

Audited DETAIL-CHECKS.md against the implemented public facade/runtime, opt-in
frontends, settings and the existing checkpoint evidence. Updated its stale saved
date/current-next-action and reconciled older entries that still claimed facade,
peer/signaling composition, playback, stream modes, aggregate allocation, profile/
links or presentation were unimplemented. Split mixed implementation/acceptance
requirements instead of marking broader hardware/network/resource gates complete.
Default-shell cutover, gaming input, GPU zero-copy/hardware decode, physical device
loss, +76/+10 capture-handle regressions, service-cost and external latency stay open.

Added scripts/test-room-regression.py to run production app scenarios as one silent
batch: real room-service integration, shared CLI media, actual offscreen UI,
delayed-mutation-ack recovery and the actual presentation worker. Optional --desktop
adds generated WGC/GPU CLI/UI checks; --repeat repeats complete rounds. Each case
uses the existing native-to-Worker fixture and retains its metrics/hashed evidence.
The matrix does not rebuild, change app defaults or invoke physical audio tests.

The runner uses a stdin-gated bootstrap assigned to a Windows kill-on-close Job
before launching children. Success/failure/timeout/cancellation closes the entire
tree, including orphan descendants. Assignment failure launches no test children.
There is a 90-second outer watchdog, 1 MiB retained-log bound, fail-fast behavior,
non-overwriting evidence directories and JSON results even after orchestration
failure. Fixture failures remain linked to their native evidence. Executable,
fixture, runner and Worker-bundle hashes make artifacts identifiable.

Validation:
- Six runner tests passed: exit propagation, runaway output, descendant cleanup
  on timeout and success, silent matrix selection and fail-fast JSON/log preservation.
- Two complete Release rounds with --desktop: **14/14 cases, 118.741s**.
  `build/webrtc/room-regression-release-20260917/result.json`
- One fully headless Debug round: **5/5 cases, 36.458s**.
  `build/webrtc/room-regression-debug-20260917/result.json`
- Final runner revision, headless Release: **5/5 cases, 35.606s**.
  `build/webrtc/room-regression-final-20260917/result.json`

All audio remained synthetic/silent; desktop controls targeted only generated
test HWNDs. Repeated independent scenarios are not a continuous soak, and native
hosts/viewers still share a process within each scenario. Scripted authorized gaming
input, network impairment, remote TLS/NAT and external latency remain untested here.
Usage and failure-path test commands are in HEADLESS-TESTING.md.

## Shared UI/CLI presentation backend and complete preview lifecycle — 2026-09-17

Moved FramePresentationBackend and the common recovery policy into backend/render.
FramePresentationSession now owns factory/error handling, recovery, terminal state
and diagnostics for both the Qt worker and Win32 CLI preview. The renderer borrows
NV12 pixels synchronously while callers retain ownership. No additional queue,
thread or pixel copy is introduced. UI still owns its latest-only worker queue.

Removed the CLI's duplicate D3D device/shader/texture/swap-chain/viewport pipeline
(ReceiverPreviewWindow.cpp loses roughly 500 net lines). The preview retains its
window controls and legacy/retained frame overloads, delegates GPU work to the same
native backend as the UI, validates shapes before sizing, and skips minimized
frames. A separate redraw operation preserves scaling/resize/clear behavior without
counting old-frame redraws as fresh frames.

The CLI now recovers from present/resize device errors with the same lifetime
three-rebuild/250 ms policy. Exhaustion stops renderer calls, preserves audio/room
membership, exposes a persistent terminal title and emits bounded JSON status on
error transitions. Final telemetry includes errors/recoveries/terminal. Repeated
unchanged titles do not issue Win32 updates. Callback exceptions are contained;
native resources are released before HWND destruction. Closing a preview uses its
local close flag rather than posting WM_QUIT into another window's event loop.

Validation (synthetic silent audio; direct messages only to test-owned HWNDs):
- Full application suites: **19/19 Release (89.63s), Debug (90.65s)**.
- Final focused CLI/worker checks pass in both builds. One final Release UI run
  exposed an existing readiness race: room ID/backend Active could precede the Qt
  snapshot enabling Copy room link. The test now waits for button enablement.
  Three subsequent UI runs each passed: Release **26.72s**, Debug **28.27s** total.
- Final Windows CLI: **8.430s Release, 8.367s Debug**. Tests actual GPU recreation
  after injected present and resize faults, backoff/no renderer calls, fourth-failure
  exhaustion, terminal-title persistence, clear/reuse, fit/1:1/fullscreen restoration,
  minimize/restore, mute/volume callbacks, malformed/legacy frame entry points and
  two-window close isolation. The fullscreen check excludes WS_VISIBLE, which
  ShowWindow/SetWindowPlacement manage separately from the restored frame style.
- Final Windows UI: **13.018s Release, 13.512s Debug**, including real resource
  recreation, budget exhaustion/clear and measured DXGI maximum frame latency 1.
- Standalone PresentationRecoveryTest rebuilt at its new include path and passed.

Artifacts:
- `build/webrtc/shared-presentation-tests-{release,debug}.log`
- `build/webrtc/shared-presentation-final-tests-{release,debug}.log`
- `build/webrtc/shared-presentation-ui-repeat-{release,debug}.log`
- `build/webrtc/shared-presentation-cli-release-final/native-service-7ae6eb5f-41ec-4b5f-8c94-7d4c24e0ea80`
- `build/webrtc/shared-presentation-cli-debug-final/native-service-55725007-b849-44a7-a9f9-f0bd6f94295f`
- `build/webrtc/shared-presentation-ui-release-final/native-service-702d3c5b-b0c3-479a-90f9-81c27910a685`
- `build/webrtc/shared-presentation-ui-debug-final/native-service-0b9a86e8-fc75-47b2-8a92-0c720388f677`

This closes duplicate-renderer cleanup and CLI preview recovery integration.
Physical driver loss/hangs, GPU zero-copy/hardware decode, gaming input, default
cutover and external performance/resource acceptance remain open. Grouped pending
work in agents/todo.md now replaces stale instructions to rebuild completed facades.

## Integrated UI presentation recovery — 2026-09-16

The actual VideoFrameWidget worker now uses the existing PresentationRecovery
policy around attach/resize/present rather than resetting and retrying every
exception forever. Three recoverable DXGI failures permit rebuilds with a 250 ms
drop-only backoff; a fourth or nonrecoverable failure becomes terminal. Successful
frames do not replenish the budget. Explicit session clear resets the budget and
resources. The v2 window displays a leave/rejoin instruction while audio/controls
remain available. No network requests, new media thread or frame queue were added.

FramePresentationBackend is constructed/disposed on the worker and is injectable
for desktop-free tests. The native backend preserves low-latency configuration,
resize/scaling redraws and real presentation behavior. Window-handle validation
belongs to the native backend so Qt offscreen handles do not bypass worker tests.

Validation, silent synthetic audio and no physical input:
- Full application suites: **19/19 Release (89.48s), Debug (90.35s)**.
- The actual worker test covers success between failures, backoff with no renderer
  calls, fourth-failure exhaustion, nonrecoverable failure, explicit clear/reuse,
  owner-thread disposal and shutdown during backoff.
- A stalled renderer receives 1,000 frames: exactly one pending frame remains,
  999 overwritten buffers are released, and the final retained buffer drains.
- Windows UI Release **12.588s**, Debug **13.833s**: typed failures injected after
  actual GPU work trigger real teardown/recreation three times, terminal exhaustion,
  clear/reuse and measured DXGI maximum-frame-latency 1.

Artifacts:
- `build/webrtc/recovery-tests-release.log`, `recovery-tests-debug.log`
- `build/webrtc/recovery-windows-release/native-service-b71d059d-cc63-451e-a631-59181fd20b7e`
- `build/webrtc/recovery-windows-debug/native-service-5618167f-54c5-450c-b6c9-65d0f7fb1ffc`

These are injected failures with actual resource recreation, not physical driver
removal or recovery from a driver call that never returns. Capture/encoder recovery,
CLI preview recovery, physical-device acceptance and external latency remain open.
The default application path and cutover gates are unchanged.

## Retained NV12 handoff and low-latency presentation — 2026-09-16

UI and CLI now retain immutable packed decoder NV12 through a shared portable frame
instead of converting NV12 to I420/back and copying into another UI vector. Legacy
vector producers remain compatible through the same frame type. Padded NV12 takes
one explicit plane pack, other formats use an explicit conversion fallback, and
shape/rotation/timestamp bounds are validated. A latest-only sink releases pending
ownership on stop and rejects late callbacks. Qt preserves final frame counters;
the shipped CLI emits a final `presentation` record.

V2 renderers now request and measure DXGI maximum frame latency 1 and present
without waiting. Busy/occluded frames are dropped, never queued for retry; successful
presentation counts no longer include dropped frames. The widget exposes error and
queue-limit diagnostics. No extra media thread or queue was introduced.

Validation (synthetic silent audio, no physical input):
- Application suites: **19/19 Release (85.83s), 19/19 Debug (87.84s)**.
- Ownership checks in CLI integration prove packed-buffer pointer identity after
  original references are released, fail on any unexpected NV12 `ToI420` call,
  verify padded-plane pixel values and I420 fallback, reject rotation, retire
  100 overwritten buffers correctly and reject callbacks after Stop.
- Real H.264 media in UI/CLI asserts directly retained decoded buffers with empty
  copy vectors and zero conversion/repack counts.
- Windows UI: Release **11.598s**, Debug **11.910s**, actual successful presents
  and measured DXGI queue limit 1. Artifacts:
  `build/webrtc/presentation-windows-ui/native-service-8770749f-605a-4b90-b809-224810f6f41e`
  and `build/webrtc/presentation-windows-ui-debug/native-service-f8250a4c-5f50-4901-b1ff-8ff91992c41b`.
- Windows CLI: **6.730s**, artifact
  `build/webrtc/presentation-windows-cli/native-service-e766dc92-daea-4158-be5c-c457a5cbcd98`.
- Logs: `build/webrtc/presentation-*.log`.

This proves removal of redundant CPU handoff operations and bounded local
presentation configuration. The MF decoder remains software/CPU NV12 and textures
still require upload. Hardware decode/GPU zero-copy, remote latency, driver-loss
acceptance and matched whole-program efficiency remain open; milestone 2 is not
complete and default AppShell remains legacy.

## Viewer playback device, volume and mute — 2026-09-16

Viewer-local playback now uses one bounded public `UpdatePlayback` operation across
native Windows runtime, actual widgets and timed CLI configuration. Output device,
0–100% volume and mute apply independently of host capture and other viewers.
Volume/mute reuse the existing device. Device replacement commits only after its
first successful write; startup/first-write failure retains the previous healthy
output and settings. An independent revision reports application. Stop cancels
pending public completion; successful settings survive ADM playout restart.

The existing playout worker owns all endpoint construction/write/destruction.
There is no new media queue, thread or clock, and no service request, renegotiation
or room recreation. Actual buffer/engine-period diagnostics refresh after a switch.
Device initialization may temporarily pause local sound; Windows driver activation
cannot be forcibly preempted. This is not physical unplug/driver-hang acceptance.

Validation (all output synthetic and silent):
- Application suites: **19/19 Release (86.90s), 19/19 Debug (87.97s)**.
- Native PCM lifecycle plus existing four-viewer regression: **2/2 Release
  (16.98s), 2/2 Debug (16.86s)**. Playback-specific checks cover exact gain, mute
  without endpoint recreation, startup/first-write rollback, Busy, cancellation
  during write, queued cancellation, restart persistence and owner destruction.
- Actual-widget tests cover viewer mute with continuing video, unmute, output and
  volume changes, missing output, unchanged membership, host rejection and public
  queue/stop ordering. CLI tests exercise timed mute/replacement, acknowledged
  results, and malformed/unauthorized configuration rejection.
- Windows UI: Release **12.792s**, Debug **13.007s**. Artifacts:
  `build/webrtc/playback-windows-ui/native-service-d2997cc0-5a9c-443c-a26f-2b598bd240f8`
  and `build/webrtc/playback-windows-ui-debug/native-service-af72e3be-4fe3-451a-8c5d-92704d0b55e7`.
- Windows CLI: **6.890s**, artifact
  `build/webrtc/playback-windows-cli/native-service-02ecc0ff-9568-43a1-b615-28b01e6e7187`.
- Logs: `build/webrtc/playback-*.log`. Windows scenarios use generated WGC windows;
  they neither capture/play physical audio nor inject keyboard/mouse input.

Milestone 2 remains open for default-shell adoption, input consent/control and
zero-copy presentation. Physical device recovery, resource and external image/input/
audio latency gates remain open; default AppShell is still legacy.

## Live shared-audio selection — 2026-09-16

The public RoomSession, native Windows runtime, opt-in host widgets and timed CLI
configuration now share bounded audio-source handover. System output, microphone
and process output can change without room/peer recreation. First replacement PCM
commits an independent revision; startup failure or a five-second missing-PCM
deadline preserves the old healthy source. Stop cancels pending commands and joins
capture owners. Success persists across ADM recording incarnations. Recording must
already be active; no service requests or automatic retries are added.

The wrapper owns one current and at most one candidate producer, each retaining a
single 10ms block. WASAPI retains at most 20ms of packet samples, preserving the
30ms application capture handoff bound. Source cadence drives reads; missing input
becomes silence and stale handoff blocks are discarded. Process activation's wait
now accepts cancellation, and its callback owns the completion event rather than
retaining a borrowed handle after timeout. Driver calls themselves are not preemptible.

Validation (all audio synthetic, no physical input):
- Full application suites: **19/19 Release (87.73s), 19/19 Debug (86.83s)**.
- Final widget/CLI assertions: **2/2 Release (15.53s), 2/2 Debug (16.10s)**.
- Native four-viewer/PCM lifecycle: **2/2 Release (17.07s), 2/2 Debug (17.12s)**.
  Tests exercise decoded silence/resume on all four viewers with continuing video,
  Busy, real five-second timeout, failed startup, bounded backlog, cancellation
  during read/activation, recording restart and endpoint owner-thread destruction.
- Final Windows UI: Release **10.891s**, Debug **11.728s**. Artifacts:
  `build/webrtc/audio-switch-windows-ui-final/native-service-64f9c8af-5f52-4070-82db-cfbc12b86036`
  and `build/webrtc/audio-switch-windows-ui-debug/native-service-ae4db56c-bec1-49fb-9dbf-aca85d9daa8b`.
- Windows CLI: **6.708s**, artifact
  `build/webrtc/audio-switch-windows-cli/native-service-60d4b9d2-ca09-40e4-b388-464ccdeef2aa`.
- Windows four-viewer source switch: **12.592s**, artifact
  `build/webrtc/audio-switch-windows-four-viewers/native-service-1b19b9a7-c873-472d-a825-363724940121`.
- Build/test logs: `build/webrtc/audio-switch-*`.

An initial four-viewer assertion assumed decoded audio would drain within 800ms;
it failed while video continued, and diagnostics showed silence arriving later.
The corrected correctness test observes 30 consecutive silent playout blocks within
a three-second deadline before testing continued silence and explicit resume.
This is not a gaming latency gate or evidence of sub-80ms playback. Physical WASAPI
switch/unplug/recovery, live playback-device selection, external latency/resource
acceptance and default-shell adoption remain open. Milestone 2 is not closed.

## Live video-source switching and decoder resize recovery — 2026-09-16

Host display/window switching now uses one bounded public RoomSession operation,
the existing capture worker and an independent selection revision. The worker polls
the current source while starting a candidate; first valid frame commits it, while
failure/closure/five-second first-frame timeout discards the candidate. Stop cancels
pending public completion and drains native ownership. UI refresh/select/share and
up to 64 ordered CLI/config captureChanges use the same operation. The room, peer
identities, room revision and stream-settings revision stay intact. Audio is unchanged.

The larger-source UI scenario initially failed: capture committed, but decoded
frames stopped. The pinned WebRTC receiver initializes its decoder limit from the
first frame without necessarily reconfiguring for growth. The MF adapter now
accepts bounded keyframe-declared sizes, still checking actual output against that
declaration and the global 4096-per-dimension allocation bound. A regression test
decodes larger keyframes from a small initial hint and rejects oversized declarations.

Validation:
- Application suites: **19/19 Release (85.15s), 19/19 Debug (86.18s)**.
- After final Busy/cancellation and unchanged-revision assertions, UI checks passed
  again: **6.41s Release, 6.84s Debug**.
- Native four-viewer/coordinator/decoder checks: Release **7.43s / 7.08s / 0.22s**;
  Debug **3/3, 15.07s**. Capture tests include the real five-second timeout, old-source
  continuity, bounded queue, stop cancellation and owner-thread destruction.
- Windows UI switches between two generated windows and confirms changed decoded
  dimensions. Release **8.14s**:
  `build/webrtc/source-switch-windows-ui/native-service-d36a14b9-8e65-4d78-8ff5-6b319896187a`.
- Final Debug Windows UI **8.39s**:
  `build/webrtc/source-switch-windows-final-debug/native-service-7060a11f-8531-4b0c-af91-4ad2a7207ab9`.
- Windows CLI scripted switch **6.77s**:
  `build/webrtc/source-switch-windows-cli/native-service-f9f58f72-3fc0-4ec8-9442-36fe96ca211f`.
- Logs: `build/webrtc/source-switch-*`. No physical input or audible audio; Windows
  capture tests ran outside the capture-restricted sandbox.

Success means first-frame readiness, not remote-display acknowledgement. Platform
startup/polling must stay bounded and can briefly delay acquisition. Switching D3D
devices can require readback/copy into the retained encoder device. Zero-copy,
hardware-only, remote latency/resource acceptance and live audio-device switching
remain open. No default UI cutover or later visual redesign was performed.

## Shared upload allocation and measured transport rates — 2026-09-16

The optional host upload allowance is integrated through StreamPreferences,
NativeRoomRuntime, sender parameters, shared JSON parsing, CLI status and the opt-in
host controls. Allocation reserves 20% and 128kbps audio per viewer, divides the
remaining video allowance equally and applies the smaller individual cap. Pending
negotiations reserve shares; membership/settings changes recompute allocation.
Insufficient shares deactivate video while retaining audio/session membership;
raising/removing the allowance restores video without replacing peers.

Status distinguishes allocated and applied caps. Independent per-generation native
stats mailboxes provide sampled WebRTC transport rates once per second, with one
request outstanding and a three-second freshness limit. First samples, counter
resets and transport replacements produce no fabricated rate. Late callbacks retain
only their mailbox. UI aggregation requires fresh samples for every viewer. This
adds no service requests or measured-rate adaptation loop.

Validation:
- Application suites: **19/19 Release (83.74s), 19/19 Debug (85.34s)**.
- Final Release UI/CLI focused checks after stronger pause/report assertions:
  **2/2 (12.35s)**. Debug's full run included those assertions.
- Native four-viewer plus settings/counter tests: **2/2 Release (7.48s)**,
  **2/2 Debug (7.55s)**. A 4Mbps allowance yields 672000bps video per viewer;
  departure redistributes to 938666bps for three, and rejoin restores four shares.
- Windows WGC/real-renderer UI pause/audio-continuation/resume: **6.36s**,
  `build/webrtc/upload-budget-windows-ui/native-service-30c21118-2e79-4e1c-a0ac-36d3e49446e2`.
- Windows WGC four-viewer budget/rejoin/rate proof: **8.28s**,
  `build/webrtc/upload-budget-windows-four-viewer/native-service-fd0c3dea-ba02-4cc2-8fc7-cc883bfa3aba`.
- Build/test logs: `build/webrtc/upload-budget-*`. Tests remained silent and used
  no physical input. Windows capture checks ran outside the capture sandbox.

This is an application allowance, not interface shaping. Tiny budgets may not
cover audio alone; probes/recovery/overhead can exceed estimates. Rejected sender
updates retain their previous caps with rejection reported, so application is not
atomic. Measured WebRTC transport counters exclude IP/interface overhead. Manual
resolution/FPS and congestion control remain intact; no bitrate floor or padding
was added. Remote latency/impairment, resource, hardware-only, cost and cutover
acceptance remain open.

## Room links and delayed acknowledgement recovery — 2026-09-16

Sessions expose a copyable `screenshare://room/v2/ROOM_ID` link. Browser admission
and the shared CLI/config parser accept it without changing the configured origin.
No password, membership token or service address is embedded. Strict validation
rejects credentials, unknown versions, queries/fragments, encoded IDs and invalid
identifier lengths before admission. Actual browser and CLI media tests join using
the link; clipboard checks use offscreen Qt only, preserving the real clipboard.

The previous missing-acknowledgement test gap is closed. A test-only Worker subclass
delays mutation 1's acknowledgement for 12 seconds and mutation 2's for 4 seconds,
while committing and pushing state immediately. The native session reaches its
real ten-second deadline, reports Unconfirmed without retry, continues delivering
frames and ignores the late first reply while the second request remains pending.
Production service code and deadline values are unchanged. The fixture records its
fault mode and bundle hash in each result artifact.

Validation:
- Application suites: **19/19 Release (81.81s), 19/19 Debug (83.95s)**.
- Final CLI link-join media test: **6.89s Release, 6.90s Debug**.
- Generated-window WGC/real-renderer Release browser/link scenario: **5.26s**,
  `build/webrtc/room-links-windows-release/native-service-d841b74e-14a6-445d-959d-08ae5db3be85`.
- Generated-window Debug delayed-ack media scenario: **15.61s**,
  `build/webrtc/room-links-windows-debug/native-service-416fd520-538c-40c2-ab08-08aaa93a2180`.
- Full-suite logs: `build/webrtc/room-links-{release,debug}-tests.log`.

Tests used silent synthetic audio and no physical input. The Windows checks ran
outside the capture-restricted sandbox. These are local integration checks, not
remote latency, hardware-only, resource, NAT or service-cost acceptance. Default
AppShell adoption remains open; opt-in windows reuse existing style/presentation
and do not start the later visual redesign.

## Live nickname and room-policy integration — 2026-09-16

The public RoomSession now exposes authoritative room revision, policy and members,
plus bounded acknowledged nickname/policy mutations. Shared validation and server
authorization remain authoritative. UI drafts retain their revision, survive
conflicts and require explicit reload/review. Unknown server outcomes are never
automatically retried. These operations use the existing socket and pushed cache;
no extra HTTP polling/preflight was introduced.

The real-Worker widget scenario verifies normalized live nicknames, stale-edit
conflicts, preserved drafts, pushed name/capacity/public-list changes, rejected
viewer policy edits and invalid names, with continued video. A deterministic native
signaling barrier verifies one-command capacity and pending-result completion during
stop. The ten-second missing-ack deadline still needs a dedicated fault fixture.

Validation:
- Release application suite: **18/18**, 65.82s; Debug: **18/18**, 67.87s.
- After final independent-draft UI refinement, focused UI test passed again:
  Release **4.28s**, Debug **4.44s**.
- Final generated-window WGC/real-renderer scenarios passed outside the capture
  sandbox: Release **4.88s**, Debug **5.15s**. Synthetic audio remained silent;
  no physical keyboard/mouse input was used.
- Logs: `build/webrtc/room-mutations-{release,debug}-tests.log`.
- Windows artifacts:
  `build/webrtc/room-mutations-windows-release/native-service-de42ba13-c6a7-402b-8a1f-30243030ee93`
  and `build/webrtc/room-mutations-windows-debug/native-service-dcc0a217-1e48-4f60-b213-a4bcfc6f8e22`.

Milestone 2 and default cutover remain open. This is local integration evidence,
not remote latency, hardware-only encoding, resource or service-cost acceptance.
Session nickname edits are intentionally separate from browser nickname persistence.

## Pushed room browser and saved nickname — 2026-09-16

`ScreenShareUi --room-v2-browser HTTPS_ORIGIN` adds opt-in create/join forms,
display/window selection, default system/microphone audio, public/unlisted
visibility, optional password, direct room-ID join and a pushed public list.
It launches the shared session window and returns after asynchronous close/drain.
The default application shell remains unchanged. No deployment was performed.

Backend RoomDirectory owns a RoomNetwork subscription, filters stale handles and
generations and consumes the existing revision-validated cache. Its control timer
drains only local events; there is no room-list HTTP polling or admission preflight.
The directory stops while the browser is hidden, resumes with a fresh snapshot,
and stops automatic reconnection after three attempts in a rolling minute. Selected
joins are disabled for stale/full/reconnecting listings; direct admission remains
authoritative. Repeated Start on a healthy origin does not reopen the socket.

RoomProfile persists only the nickname and reuses the wire validator's Unicode,
length and control/bidi rules. Malformed stored names fall back to Guest; invalid
edits do not replace valid data. Passwords/room IDs/service origins/tokens are not
persisted by this profile. Password fields clear after launch, and directory titles
use plain table text. Live profile mutation remains outstanding.

The extended widget test verifies canonical nickname persistence, invalid-profile
fallback, nickname-only stored keys, public listing, literal room titles, password
rejection/recovery, join/media, pushed counts/removal, hidden subscription shutdown,
rapid hide/show and idempotent start. An independent subscriber observes updates
while both browser windows are hidden. Production plaintext rejection opens no
socket. All tests use synthetic silent audio and no physical input.

Application suites pass 18/18 Debug and Release; CLI-only Release passes 12/12.
Final focused Debug UI check after retry-button adjustment passes in 3.86 s.
Logs: `build/webrtc/room-browser-app-{debug,release}.log` and
`build/webrtc/room-browser-cli-release.log`. Final generated-window WGC plus
real-renderer runs pass outside the capture-restricted sandbox:

- Release, 4.32 s: `build/webrtc/room-browser-windows-final-release/native-service-5437735e-b13f-4a60-a7fd-8bc393009fba`
- Debug, 4.56 s: `build/webrtc/room-browser-windows-final-debug/native-service-226eabf1-546b-4e87-bea3-46d1f7216978`

These are local correctness checks, not remote latency/NAT/TLS, service-cost or
resource acceptance. Default-shell adoption, live profile/policy mutations, room
links, input consent/control, aggregate allocation and source switching remain.
The requested appearance/feel/usability redesign is separately scheduled as
TODO milestone 6 after the existing milestones; current integration is not deferred.

## Opt-in Qt session adoption — 2026-09-16

ScreenShareUi accepts `--room-v2 CONFIG.json` and opens RoomSessionWindow using
QtRoomSession, the shared v2 backend and existing video widget/style. CLI parsing
was extracted into frontend/shared/RoomSessionConfig and a common frontend target.
Both entry paths enforce the same configuration/security rules. Default startup
and the normal room browser remain unchanged.

The Qt owner keeps one settings operation in flight and one latest pending edit,
checks local status without service requests, and consumes bounded latest frames.
Stop/close drain asynchronously while Qt continues processing events; the window
closes only after completion. Finished owners can start a new session incarnation.
Pending media/settings references are released on drain; cancellation preserves
admission uncertainty. Host controls expose presets, canvas, FPS, bitrate and Auto
limits with pending/applied/rejected state. Remote input is deliberately unavailable
until its authenticated consent/control path is integrated.

The offscreen widget scenario hosts/joins the isolated Worker, observes original
and reduced frames, rejects odd dimensions, coalesces 101 valid edits into one
revision, closes the viewer after drain, stops the host and reuses an owner. A
held media-stop future proves Qt heartbeat timers keep dispatching and finished
fires once per incarnation. No physical input is injected; audio is synthetic.

Final application suites pass 18/18 in Debug and Release; CLI-only Release passes
12/12. Logs: `build/webrtc/room-ui-app-debug.log`, `room-ui-app-release.log`,
`room-ui-cli-release.log`. Final Windows UI proof passes outside the capture-
restricted sandbox with WGC, the real renderer, 20 original + 20 reduced frames
and silent synthetic audio in 2.72 s. Artifact:
`build/webrtc/room-ui-windows-final/native-service-14cc707a-61ed-400d-8c42-69da41cbaed3`.

[ROOM-UI.md](ROOM-UI.md) documents invocation, lifecycle and test commands. These
results do not establish physical display/input latency, zero-copy presentation,
NAT/TLS, resource or service-cost acceptance. Normal create/join, directory/profile,
remote controls, source changes and aggregate allocation remain in milestone 2.

## Opt-in CLI host/viewer and bounded presentation — 2026-09-16

`ScreenShare --room-v2 CONFIG.json` now routes to a separate frontend using the
shared RoomSession/WindowsRoomRuntime. It supports host/join, display or window
capture, audio source/device selection, stream preferences, ordered timed live
changes, JSON status and responsive timed/console/window-close shutdown. Its
configuration parser rejects unknown fields, wrong types, invalid preferences,
oversized files and non-HTTPS production origins. Status/error output excludes
passwords and membership credentials; cancellation preserves admission uncertainty.

LatestRoomVideoFrame retains at most one decoded frame, replacing stale pending
work. The preview owner converts the consumed frame to NV12 and renders through
the existing D3D preview; decoder callbacks never render or queue unbounded frames.
CPU conversion is intentional for this first integration; zero-copy presentation
and measured gaming latency remain outstanding. Settings/status checks use local
snapshots and do not introduce service polling.

The same parser/controller is exercised against the isolated Worker with synthetic
H.264/Opus, a mid-session 320x180 -> 160x90 update, source/sender revision reports,
decoded pixel/NV12 validation, 100-frame presentation backlog replacement,
cancellation and failed admission. Real executable entry tests verify dispatch,
argument validation, production plaintext rejection and secret-free errors.
The deployment check starts with only the CLI executable in a fresh directory,
deploys and verifies Core/Network/WebSockets plus Windows TLS, and runs entry
checks. CLI runtime deployment no longer relies on tests or the Qt UI target.

Final application suites pass 17/17 in Release and Debug; CLI-only Release passes
12/12. Logs: `build/webrtc/room-cli-app-release.log`, `room-cli-app-debug.log` and
`room-cli-only-release.log`. All routine audio coverage uses silent endpoints.

The explicit Windows variant passes with generated-window WGC capture and actual
D3D preview presentation, silent synthetic audio and no physical input. Final
Release evidence: 53 original-resolution and 63 reduced-resolution frames,
6.78 s, at `build/webrtc/room-cli-windows-final/native-service-82755916-3ee6-4343-b16f-179c8866454f`.
This ran outside the capture-restricted sandbox. It does not measure physical
display latency or prove hardware-only codec use.

Usage, configuration and reproducible commands: [ROOM-CLI.md](ROOM-CLI.md).
Default UI/other CLI sessions remain legacy. This is opt-in frontend adoption,
not milestone 2 closeout: UI/profile/directory, remote input, source switching,
aggregate allocation, ICE-server configuration and all acceptance gates remain.

## Public live stream settings and silent tests — 2026-09-16

RoomSession now accepts validated host settings through a bounded asynchronous
command (one queued update; Busy tells callers to retry their latest choice).
Requests after stop/before Active are Unavailable; viewer runtimes return
Unsupported. Successful acceptance returns a monotonic runtime revision. Status
publishes desired preferences and each peer's sender-applied/source-observed
revision, current source dimensions and rejection. Acceptance does not claim
remote decode/presentation. Stop clears peer observations after media drains.

NativeRoomRuntime applies each revision once per ready sender. Existing peers keep
their prior working sender on live rejection, without a 5 ms retry loop; the next
explicit revision can retry. Initial failure still retires the affected peer.
New joins inherit the latest preferences. No renegotiation or room rebuild is
required, and the existing manual bitrate policy never installs a bitrate floor.
This changes per-viewer adaptation, not capture-device configuration or aggregate
bandwidth allocation. Increasing output FPS cannot exceed available capture FPS.

The public four-viewer proof changes to 320x180/20 FPS/manual 1 Mbps, checks all
sender/source revisions and reduced decoded frames, restarts, rejoins at the new
resolution, restores 640x360/30 FPS/Auto, and checks invalid/viewer/stopped commands.
Release and Debug media suites pass 31/31 with audio-device tests OFF; application
Release passes 14/14. Final Release public-session rerun after clearing stopped
peer status passes in 7.28 s. Logs: `build/webrtc/live-settings-{release,debug,app}.log`.
The Windows WGC variant passes outside the sandbox with synthetic audio in 9.96 s:
`build/webrtc/live-settings-windows-release/native-service-6ca5748f-03e3-452a-b069-7ba2de39dcbd`.

The runner rejects `-AudioDevice` unless `-AllowAudibleTests` is also supplied;
the rejection was checked before configuration/device startup. Routine runs omit
both and retain synthetic audio decoding coverage without speaker output.
No latency, achieved bitrate/FPS, resource, NAT/TLS or service-cost gate is closed
by these tests. Normal UI/CLI still use the legacy session pending adoption.

## Production runtime composition and Windows binding — 2026-09-16

NativeRoomRuntime replaces PublicRoomSessionProof's diagnostic peer/capture owner.
It composes MediaEngine/MediaPeer, HostMediaSession, HostPeerRegistry and
RoomManagedPeer. Host offers wait for attachment; restart requests pass through
the existing per-peer recovery budget. Viewer recovery emits an authenticated
restart request to the host. All native references survive capture retirement;
normal shutdown advances asynchronously until the registry/capture barrier drains.
Failures are reported to RoomMediaSession so status and authoritative membership
agree; failed delivery subscriptions cannot silently leave a connected blank peer.

Initial StreamPreferences are validated and applied to each source before delivery;
RTP settings are applied once negotiation is ready. Sources/senders are independent
per viewer. Live preference changes, aggregate allocation and UI settings remain
milestone 2 work. Source-start failure now reaches the public facade as Media,
rather than being misclassified as a room transport error.

WindowsRoomRuntimeFactory binds WindowsCaptureSource and WasapiPcmEndpoints,
with injectable audio for tests and a shared presentation sink/channel callback.
The host waits for its first capture device before building MF codec factories.
Device retirement retains the existing software fallback; automatic hardware
reenablement and actual driver-removal acceptance remain open. Source/sink objects
and the application WindowsMediaRuntime/SSL lease must outlive joined teardown.
ScreenShareMediaAdapters is now compiled by both application and proof targets;
NativeRoomRuntimeTests links the Windows factory through ScreenShareCore and checks
idle cancellation without opening capture/audio devices.

Full suites pass **33/33 media Debug/Release**, **14/14 application Release**,
**9/9 CLI-only Release**. Logs: `build/webrtc/native-runtime-debug.log`,
`native-runtime-integrated-release.log`, `native-runtime-app-final.log`,
`native-runtime-cli-final.log`. Final focused public-runtime checks with restart,
rejoin and healthy-peer progress after injected delivery failure pass in Release
(5.86 s) and Debug (5.95 s).

The generated-window Windows public-runtime proof passes outside the sandbox in
Release (5.76 s) and Debug (5.95 s), including four independent H.264/Opus viewers,
restart/rejoin and shutdown. Artifact roots:
`build/webrtc/native-runtime-windows-release-recovery` and
`build/webrtc/native-runtime-windows-debug-recovery`. Inside the sandbox WGC
CreateForWindow fails with “The specified service does not exist as an installed
service”; this is an environment limitation, not a passed test. No physical input
or desktop audio is used. These runs permit software fallback; hardware-only
encoding, external latency, two-hour soak and service-cost acceptance are not proven.

Milestone 1's local runtime integration deliverable is complete. Next is milestone
2: normal UI/CLI adoption, live settings, capture selection/audio configuration and
presentation through this factory. Legacy default removal still depends on the
original acceptance gates; normal application sessions have not been switched yet.

## Public owned session lifecycle — 2026-09-16

api/RoomSession.h now exposes asynchronous Start/Stop and thread-safe Status with
portable options/results. It owns RoomNetwork, SignalingExecutor, admission,
RoomSessionCoordinator, RoomMediaSession and an injected RoomRuntime. Native
factory construction and destruction happen on signaling. Start resolves on the
first authenticated snapshot, not the first decoded media frame. Repeated Start
is Busy; Stop is coalesced and also cancels pending admission. Cancelled in-flight
admission conservatively reports an unconfirmed outcome and is never retried.
Membership credentials remain internal and are cleared after shutdown.

Normal Stop sends peer.leave, waits up to one second for acknowledgement/closure,
then closes sockets. Media BeginStop completion gates runtime destruction while
Advance continues servicing cleanup. Stop/Status stay responsive during a held
media barrier. External destruction joins only after this barrier; never destroy
the facade inside its runtime callbacks. Runtime Ready/Remove are nonthrowing,
RoomSend is signaling-only and reports queue acceptance, and BeginStop must return
a valid completion future. A new admission uses a new RoomSession object.

Transport recovery retains the existing socket implementation and adds a ceiling
of three reconnect attempts per rolling minute and 35 seconds without a fresh
snapshot. Protocol/backpressure failures are terminal. The lifecycle timer stops
while healthy/idle; the existing coordinator continues media/event dispatch.
Recovery policy exists but injected real transport impairment remains unverified.

PublicRoomSessionProof uses only public Start/Stop/Status from the caller. Its
injected diagnostic runtime combines shared native engine/peers/negotiation with
HostMediaSession and synthetic source/audio. Four independent viewer engines each
validate decoded H.264 and audible Opus. Coverage also includes duplicate Start,
cancelled admission, unlisted create, graceful viewer departure, host closure,
production TLS enforcement, coalesced stop and a deliberately held drain barrier.
No physical input or caller Qt/WebRTC event pumping is involved.

Full media suites pass **33/33 Debug and Release**. Logs:
`build/webrtc/public-session-debug.log` and `public-session-release.log`.
Application Release **13/13** and CLI-only Release **8/8** also pass; logs
`build/webrtc/public-session-app.log` and `public-session-cli.log`.
After final shutdown/cancellation/security cases, focused public-session checks
pass again (Release 2.81 s, Debug 3.01 s; Testing/Temporary/LastTest.log).
This is correctness evidence, not latency, NAT, resource or service-cost acceptance.

Remaining: standard Windows RoomRuntimeFactory with production capture settings,
audio and presentation, and normal UI/CLI adoption. The synthetic diagnostic
composition is deliberately not installed as the default application backend.

## Shared room/media session composition — 2026-09-16

RoomMediaSession, in ScreenShareRoomSession, now consumes authenticated room
snapshots and signals on signaling. Host sessions reconcile connected viewers;
viewer sessions reconcile the connected host. The event queue is bounded to
256 entries / 512 KiB. Socket generation and snapshot revision barriers reject
stale updates; transport loss retires peers and requires a newer generation to
resume. Closed/stopped or overflow-failed sessions cannot revive from later input.
An invalid/rejected negotiation retires only its sender, and unrelated profile
revisions do not create retry storms. Unknown/nonmember signals are ignored.

RoomPeerRoster now distinguishes pending additions from failed ones. A readiness
hook gates slot reuse until asynchronous capture cleanup and native retirement
finish. Only pending additions retry on coordinator ticks. Authoritative removal
or shutdown cancels pending additions; failed construction waits for leave/rejoin.
Ready/Remove callbacks must not throw or reenter; Add owns cleanup on failure.

The real four-viewer proof uses a host and four viewer RoomMediaSessions. Its
diagnostic packet queue, role filtering and roster revision dispatcher have been
removed. The coordinator services these sessions after capture retirement. Native
construction hooks and media evidence remain in the proof. Viewer shutdown may
precede host membership delivery; health checks now respect that suspended state.
Production facades should create the session before opening its socket, route all
socket events through OnEvent, and call Advance from the existing coordinator.

Full media suites pass **32/32 Debug and Release**, application Release **13/13**,
CLI-only Release **8/8**. Logs: `build/webrtc/room-session-debug.log`,
`room-session-release-final.log`, `room-session-app.log`, `room-session-cli.log`.
Dedicated tests cover pending rejoin, cancellation, failure isolation, stale events,
terminal queue pressure and startup errors. These results establish correctness,
not latency/resource acceptance or a deployed-service free-tier cost improvement.
After final startup/terminal-close and pending-cancellation assertions, focused
room-network/session/authenticated-media checks pass **3/3** in both configurations
(Release 35.00 s, Debug 35.03 s; build Testing/Temporary/LastTest.log files).

Still open: a public facade owning admission, start/join/stop, all native resources,
recovery status and UI/CLI adoption. Normal application media remains legacy.

## Shared native engine and peer ownership — 2026-09-16

MediaEngine and MediaPeer are production components in ScreenShareNegotiation,
compiled by both application and diagnostic builds. The engine owns network and
worker threads, factory/Opus setup, caller-supplied audio/video implementations,
independent peer creation, host track attachment and data-channel creation.
Caller ICE configuration is preserved while Unified Plan/MaxBundle are enforced.
Failed audio attachment rolls back the video attachment; rollback failure closes
the peer. There is no synthetic source or diagnostic observer inside the engine.

MediaPeer owns ICE-state lifecycle updates, native negotiation, the incoming
video sink and a maximum of three channels. Unknown/duplicate labels or incorrect
reliability policies are closed before reaching application callbacks. Control
is ordered/reliable; input-state and telemetry are unordered/no-retransmit.
Close disconnects callbacks, removes the video sink, closes channels and native
connection, and cannot be undone by late ICE callbacks. A replaced incoming video
track first removes the old sink. Construction/use/destruction belong to signaling;
frame delivery belongs to WebRTC's delivery thread. Derived evidence sinks must
call Close before destroying their members. All peers/tracks must be released
before engine destruction, then the signaling executor may stop. SSL remains
application-owned.

RoomMediaProof no longer imports the old diagnostic Peer/MediaLink implementation.
Its remaining observers only validate decoded pixels/messages. Actual room-backed
four-viewer media, socket reconnect, budgeted restart, kick/rejoin, autonomous
timeout, cancellation and shutdown now exercise these shared native objects.

Media suites pass **31/31 Debug and Release**; application Release **13/13**, CLI-only
Release **8/8**. Logs: `build/webrtc/media-peer-debug.log`,
`media-peer-release.log`, `media-peer-app.log`, `media-peer-cli.log`.
The new lifecycle test covers ten engine lifetimes, rejected dependencies/thread
access, ICE-policy preservation, partial-track rollback, channel constraints and
late-event rejection. These are correctness checks, not handle/latency acceptance.
After final observer/header cleanup and the additional channel-policy assertions,
focused engine + authenticated-room tests pass **2/2** again in each configuration
(Release 35.19 s, Debug 35.33 s; each build's Testing/Temporary/LastTest.log).

Remaining milestone 1 work: move the outer room roster/session composition and
recovery policy into the shared facade, expose asynchronous commands/status, and
adopt it in UI/CLI. Normal application sessions still run the legacy backend.
Do not reopen completed factory, peer-lifecycle or dispatch extraction work.

## Autonomous room/media dispatch — 2026-09-16

ScreenShareRoomSession is a shared application/proof build target containing
RoomSessionCoordinator and RoomSignalCodec. The coordinator runs on signaling,
consumes the RoomNetwork mailbox automatically, resolves socket-open futures,
delivers generation-filtered events and advances media lifecycle hooks. Sends
return queue acceptance immediately; later network failures become typed error
events. Pending sends are bounded to 128 operations / 512 KiB, and a late failure
from a retired connection generation cannot fail its replacement. Close removes
callbacks before asynchronous transport shutdown; Stop cancels unresolved opens
and invalidates delayed work. Callbacks may send responses but must not mutate
socket lifetimes or destroy the coordinator.

RoomPeerSignal is now a portable media type. Shared wire conversion validates
incoming event structure and explicitly rejects non-signal events; authentication
and role authorization remain RoomSocket's responsibility.

RoomMediaProof no longer drains network events or forwards signaling from its
outer wait loop. Scenario commands and observations cross the executor boundary;
backend scheduling drives roster reconciliation, SDP/ICE and send completions.
The test pauses all observation for five seconds after requesting ICE restart,
then verifies negotiation already completed. The pause demonstrates autonomous
dispatch, not a latency acceptance measurement. Queue metrics now describe the
coordinator's pending sends rather than the retired diagnostic packet queue.

RoomSessionCoordinatorTest covers thread affinity, late send failure, operation/
byte bounds, retired callback suppression, cancelling an unresolved open and
no callbacks after stop/destruction. The normal ScreenShareSession facade remains
legacy: peer-factory/session composition and public commands/status still need
production adoption, followed by settings/presentation/input integration.

Validation: full media suites passed 30/30 in Debug and Release; Release application
13/13 and CLI-only 8/8. Final focused dispatch/media checks use
build/webrtc/coordinator-verified-{debug,release}.log; full logs are
coordinator-final-release.log, coordinator-full-debug.log, coordinator-final-app.log
and coordinator-final-cli.log in the same directory. Callback-generated responses
are tested outside pending-send iteration to avoid iterator invalidation.

## Managed room peers and asynchronous capture shutdown — 2026-09-16

RoomManagedPeer connects authenticated room negotiation to HostPeerOwner's
scheduled lifecycle, restart budget and capture-aware retirement. The initial
offer waits for successful asynchronous capture attachment; failed attachment
is handled as a peer operation failure. Restart IDs include the peer incarnation
and restart revision. Room membership removal initiates capture cleanup without
waiting on signaling, and native peer resources remain retained until cleanup
completes.

HostPeerOwner.BeginStop returns a shared completion future. The registry starts
capture shutdown, rejects new peers/restarts/removals while stopping, and polls
completion without blocking signaling. It closes/releases remaining peers only
after capture callbacks have joined. Repeated stop requests share completion;
capture errors are returned rather than leaving the future pending. Synchronous
Stop remains a final-teardown fallback; runtime coordinators must await BeginStop
before destroying the owner.

RoomMediaProof now uses the shared managed adapter for four real room-backed
peers, scheduled ICE restart, socket loss/reconnect and kick/rejoin. Capture
startup waits outside signaling; per-viewer attachment/removal no longer call
future.get on signaling. Normal shutdown also awaits the asynchronous owner.
HostPeerOwnerTest deliberately holds capture delivery open and verifies that
signaling commands still run and peer resources remain retained until release.

Normal UI/CLI facade adoption and automatic cross-executor event dispatch remain
unfinished. This does not establish external latency or capture resource gates.

Validation: full media suites 29/29 in Debug and Release; application 13/13 and
CLI-only 8/8 in Release. Final capture-startup placement is verified by the
room-backed media and scheduled-owner tests in both configurations. Evidence logs:
build/webrtc/managed-room-{debug,release,app,cli}.log and
build/webrtc/managed-room-verified-{debug,release}.log.

## Owned room networking and roster integration — 2026-09-16

RoomNetwork owns a dedicated Qt event loop shared by admission and up to 64 room
sockets. Commands return futures; no caller Qt event pump is required. Normal
commands are bounded to 128 / 512 KiB; events to 256 / 512 KiB. Event overflow
retires all sockets and cancels admission, exposing a terminal backpressure marker
instead of silently dropping SDP/ICE. Stop requests coalesce outside normal queue
capacity; StopAll cancels admission and closes sockets asynchronously. Destructor
cancellation resolves pending admission before joining the thread. Socket handles
increase across attempts/rejoins, preventing old queued events from being applied
to replacement sockets. A terminal overflow owner must be replaced, not restarted.

RoomPeerRoster consumes complete authenticated snapshots on the control/signaling
executor. It validates the full peer set before mutation, ignores old revisions/
generations, isolates failed additions without retrying on unrelated revisions,
and retires peers on membership removal or transport loss. A lost transport
generation cannot revive peers; a newer authoritative snapshot is required.
Lifecycle hooks retain responsibility for native ownership and cleanup barriers.

The real workerd media scenario uses these shared components: room sockets run
off the main thread; host roster changes attach/retire native media, kick/rejoin
is automatic, and room closure removes subscriptions/peers. Socket disconnect
and reconnect exercise replacement incarnations with fresh signaling IDs.
RoomNetworkTest covers admission cancellation through StopAll and destruction,
20 owner lifecycles, coalesced stops, oversized commands, event overflow, and
roster stale-state/failed-peer behavior without pumping a caller event loop.

This does not complete the normal UI/CLI facade. The diagnostic still waits on
command futures and capture teardown; production facade commands must instead
observe completion asynchronously and preserve cleanup/generation barriers.
Latency, resource, remote-network and service-cost gates remain open.

Validation: full Debug/Release media suites passed 29/29; Release application
13/13 and CLI-only 8/8. Final affected tests are `room-network-ownership` and
`room-backed-four-peer-media`; logs live under build/webrtc/network-verified-*.log.
The latter records `owned_network_loop`, `roster_driven_peers` and
`socket_reconnect` alongside existing media/recovery metrics. Test-owned waits
remain distinct from a production asynchronous coordinator.

## Source ownership and autonomous negotiation — 2026-09-16

Native sources now live in backend/ and frontend/; CMake and proof consumers use
their respective include roots. The room Worker stays independently deployable.
This is an ownership cleanup, not a claim that legacy UI/CLI routing has changed.

RoomPeerNegotiation now schedules completion/deadline processing on its signaling
thread while negotiation is active. Ready/idle peers have no recurring timer.
Close, destruction and a new negotiation generation invalidate delayed callbacks.
The real-room proof no longer calls Poll. It additionally destroys a peer before
its first tick and withholds an answer from another peer, checking that its 20s
deadline closes it automatically while established media continues.

Validation after relocating all 161 sources: full Debug and Release media suites
28/28 each; full Debug and Release application suites 13/13 each; Release CLI-only
8/8. Logs are build/webrtc/layout-{proof,app}-{debug,release}.log and
build/webrtc/layout-cli-release.log. All moved file contents match the previous
commit except the two intentional RoomPeerNegotiation implementation/header edits.
UI assets compile and deploy through their unchanged repository-relative paths.

## Authenticated four-viewer media — 2026-09-16

RoomPeerNegotiation now connects native asynchronous SDP operations and bounded
trickle ICE to authenticated room messages. It runs on the signaling executor,
uses fresh wire connection IDs, gates candidates behind descriptions, ignores
retired candidates/answers, and closes failed/timed-out peers. Local credentials
are cleared before creating a new description so retired ICE callbacks cannot be
assigned to the next generation. The transport callback must enqueue without
blocking; higher-level ownership must propagate later socket failures and enforce
the restart budget.

RoomMediaProof creates a real local workerd room through RoomAdmission and runs
one host plus four RoomSockets. Actual H.264 and synthetic Opus travel through
four native PeerConnections; SDP/ICE travel through the Worker. Coverage includes
a slow capture subscriber, twelve encrypted data channels, fresh-ID ICE restart,
stale candidate rejection, duplicate ID rejection, host kick, fresh admission and
media rejoin, and room shutdown. The proof records frames, mixed audible PCM,
candidate count and outbound signaling queue peaks in hashed evidence artifacts.

Debug and Release full media suites passed 28/28; Release application passed
13/13 and CLI-only passed 8/8. See HEADLESS-TESTING.md for the standalone command.
These are synthetic/local correctness results, not desktop capture, independent
per-viewer audio, NAT, input latency, resource acceptance or free-tier cost evidence.

The adapter and room library are shared production components. Diagnostic outer
orchestration still pumps operations and waits; do not copy those waits into UI
commands. The scheduled shared facade, automatic roster/recovery ownership and
normal UI/CLI adoption remain open. Matching service handlers now exist and are
exercised here. Work is grouped by deliverable in TODO.md; detailed original gates
remain in DETAIL-CHECKS.md.

## Scheduled peer ownership and concurrent negotiation - 2026-09-15

HostPeerOwner now binds the capture-aware registry to SignalingExecutor in the
shared application/proof library. A single weak delayed callback runs every
20 ms on signaling, advancing deadlines, restart actions, ready peer operations
and capture cleanup. No external Tick call is required. This is local lifecycle
scheduling, not a network poll or a timer on image/input delivery.

IMediaPeer::Poll consumes ready asynchronous work only. False/throw closes the
affected peer and records operationFailed; other peers keep running. Terminal
lifecycle failure takes precedence over pending work. Closed rows retain their
failure/cleanup status until removed. The registry remains the owner of peers.

The real four-viewer scenario now admits connections in generation order before
waiting for any answers. Its adapter advances local SDP creation, remote apply,
candidate readiness and answer creation through the owner's completion hook.
The same path handles restart and replacement. Restart no longer uses a pending
flag consumed by synchronous test-side SDP exchange, and no peer callback waits
or pumps messages. DescriptionTransfer is a diagnostic local-delivery adapter;
authenticated room delivery still needs a production adapter. Outer diagnostic
waits still pump while observing the scheduled owner.

Embedding requirements:

- Construct, invoke and destroy the owner on its supplied executor. UI/CLI calls
  must enqueue through the executor. It checks thread affinity at its boundary.
- Capture and executor outlive the owner. Stop joins capture before closing peers;
  remove remains asynchronous. Complete owner teardown before executor shutdown.
- Delayed callbacks hold only weak state and are inert after Stop/destruction.
  Do not destroy/reenter the owner from a peer's Poll/Close/restart method.
- A failed lifecycle snapshot can precede scheduled cleanup. peerClosed explicitly
  distinguishes completed native close from a newly observed failure. Wait for
  peerClosed with captureCleanupPending false, or removal's row disappearance,
  before treating cleanup as complete. The first full run caught this assumption
  in the proof; completion now has an explicit snapshot field.
- The 20 ms cadence is an initial control scheduling default, not a gaming
  latency claim. Native/driver hangs still require the process watchdog.

HostPeerOwnerTest uses the actual executor without manual message pumping. It
checks an expired initial connection, scheduled restart, failed-completion
isolation, healthy capture progress, stale requests, joined stop, owner-thread
destruction and 25 recreated owners while weak timers remain queued.

Final evidence: `build/webrtc/owner-verified-sdk-proof-{debug,release}.log`,
`owner-verified-sdk-app-{debug,release}.log`, and
`owner-verified-headless-{debug,release}/result.json`. Earlier runs before the
explicit peerClosed field are retained under `owner-concurrent-*` and
`owner-headless-*` for comparison.
Final Debug/Release media suites passed 27/27 each; both application suites
passed 10/10. Final Debug headless smoke passed 12/12 and Release regression
passed 33/33, including 20 single-viewer and three concurrent four-viewer runs.
The normal UI/CLI backend, authenticated room transport, external latency and
known capture handle-growth acceptance remain open.

## Owned signaling event loop - 2026-09-15

SignalingExecutor is now part of the shared ScreenShareNegotiation library.
Its portable header exposes commands/futures and typed operation results; the
implementation owns a native WebRTC thread. All single-peer, hardware/audio and
four-peer media scenarios run on that executor instead of wrapping main in an
AutoThread. The proof's registry and native peer lifetimes remain inside the
executor task. The normal application facade has not switched backends.

At most 64 commands may wait behind the running command. Capacity, invalid,
closed, cancelled and task-failure results are explicit. Exceptions are contained
without exposing arbitrary exception text. Only one application command is
posted to the native queue at a time, allowing native callbacks between commands
and avoiding an unbounded native queue of application work.

Shutdown/embedding contract:

- Post initiates work; production commands return without waiting on later
  executor commands or native completion. Complete pending negotiation through
  asynchronous owner dispatch, not the diagnostic's nested message waits.
- RequestStop is nonblocking, rejects new commands and cancels pending commands
  after the active command returns. Accepted closures, including cancelled ones,
  are destroyed on signaling before their future completes. Rejected closures
  are destroyed on the submitting thread and must be safe there.
- Stop joins from an external owner and may be repeated/concurrently called.
  Self-join rejects; a task may call RequestStop but may not destroy the executor.
- Close/destroy native peers, negotiation adapters and factories on signaling
  before stopping the executor. Keep SSL/Winsock alive until it joins. Stop does
  not preempt an active native call or implicitly close independently owned peers;
  retain a process watchdog for a hung native/driver call.

The dedicated executor test verifies ordering, current-thread identity, ordinary
native callback progress, exception containment, full-queue rejection, 64 pending
cancellations, owner-thread closure release and shutdown behavior. The
negotiation-only scenario additionally creates two native peers, exchanges and
applies offer/answer SDP across separate commands, waits only from the external
caller, then destroys all native state on signaling. No manual message pumping
is used in that scenario. Existing media diagnostics still use nested waits.

Validation evidence is recorded in `build/webrtc/executor-final-{debug,release}.log`,
`executor-app-{debug,release}.log` and `executor-headless-{debug,release}/result.json`.
Debug and Release media suites passed 26/26 each; application suites passed
10/10 each. Debug headless smoke passed 11/11 and Release regression passed
32/32, including 20 single-viewer and three four-viewer runs. The only later
source edit reformatted the diagnostic dispatch indentation without changing code.
Application ownership/completion dispatch, authenticated room messages, actual
network impairment and UI/CLI cutover remain open. This change makes no new
latency/resource-growth acceptance claim.

## Shared asynchronous SDP negotiation - 2026-09-15

Added PeerNegotiation in the private WebRTC implementation module and the shared
ScreenShareNegotiation CMake target. Application and proof builds compile the
same implementation. The proof's CreatedDescription/AppliedDescription helpers
are removed; single-peer, hardware/Opus and four-peer ICE-restart paths now use
the shared adapter for local offer/answer creation/application and remote SDP.

Operations return futures immediately, carry operation/connection identities
and allow only one in-flight request per adapter. Overlap returns Busy instead
of accumulating a queue. Wrong generations reject before native mutation.
SDP is bounded to 60 KiB and rejects empty/NUL-containing data, matching the
room protocol's payload bound; the room transport must still validate the full
serialized envelope. Native error descriptions are not echoed with credentials.

Local SDP is serialized before gathering, but a successful future is delivered
only after SetLocalDescription succeeds. Local ICE credentials become available
before native candidate callbacks, preserving the existing retired-credential
filter. Creation/application observers hold weak state and operation identities.
Close resolves an outstanding future as Cancelled and rejects further work;
late callbacks cannot apply a cancelled created description or complete a newer
operation. Cancellation does not roll back an already-issued native SDP apply;
the peer owner closes the native connection during lifetime cancellation.

All adapter methods/destruction belong to the peer's signaling executor. There
is no message pumping, blocking wait or internal thread in this component. The
local diagnostic driver still pumps its event loop to observe futures; wiring
the application executor and room transport remains open. This milestone builds
the library into the application dependency graph without switching UI/CLI media.

The dedicated negotiation scenario uses actual PeerConnections: 25 pending
offer cancellations and owner destructions, bounded overlapping requests, stale
remote requests, malformed/oversized/NUL SDP, failed answer creation, successful
replacement negotiation after late callbacks, and remote application on a closed
native connection. Existing encrypted media, restart, settings, failure cleanup
and rejoin checks now run through the same adapter. No private signaling is logged.

Debug/Release media suites pass **25/25** each and application builds/suites
pass **10/10** each. Evidence logs:
`build/webrtc/async-negotiation-sdk-proof-debug.log`,
`build/webrtc/async-negotiation-sdk-proof-release.log`,
`build/webrtc/async-negotiation-sdk-app-debug.log` and
`build/webrtc/async-negotiation-sdk-app-release.log`.
Use the existing run-webrtc-proof helper when CMake regenerates so the pinned
MSVC/Windows SDK environment is initialized. Existing AutoThread warnings remain.

Headless Debug smoke passes **10/10** runs and Release regression **31/31**,
including the dedicated cancellation test, 20 single-peer lifecycles and three
four-peer restart/failure-cleanup/rejoin scenarios. Reports with executable
hashes and watchdog outcomes:
`build/webrtc/async-negotiation-headless-debug/result.json` and
`build/webrtc/async-negotiation-headless-release/result.json`.

## Automatic peer-failure capture cleanup - 2026-09-15

HostPeerRegistry can now bind to HostMediaSession and its host generation.
Terminal peer failure and explicit removal enqueue connection-scoped capture
removal. Tick polls the future without waiting; Capacity/Cancelled results stay
pending and retry on subsequent ticks. None/StaleGeneration completes cleanup,
because a stale host/connection must never remove a newer subscription.

Snapshots expose cleanup-pending and the last cleanup error while preserving
the original peer failure. Remove returning true means accepted; with capture
bound, callers tick until the row disappears before reusing that viewer slot.
Failed rows remain available for diagnosis until explicitly removed. Retained
cleanup futures stay bounded by the registry's 63-peer admission cap.

Full Stop joins bound capture through its priority stop command before closing
and releasing remaining peers. The capture owner must outlive the registry;
declare it first. Stop may report an unexpected capture-stop operation failure;
the owning executor must serialize shutdown and must not race competing stop
commands. Existing process watchdogs remain necessary for native calls. Peer
Close cancels its network callbacks while retained capture source references
remain valid until asynchronous delivery cleanup joins them.

The deterministic owner test blocks one viewer callback, verifies healthy frame
progress, fills the capture command queue, observes Capacity on another removal
and verifies eventual cleanup after release. The real four-peer proof injects
a terminal peer failure, waits for automatic detachment, retains the failure
reason and rejoins with a fresh connection generation.

Application executor scheduling, asynchronous room/WebRTC adapter integration,
UI/CLI replacement and field/network validation remain open. No legacy path is
kept as a permanent alternate backend. Native handle-growth and measured gaming
latency acceptance are unchanged.

Debug and Release media suites pass **24/24** each; logs:
`build/webrtc/peer-cleanup-debug.log` and `build/webrtc/peer-cleanup-release.log`.
Application binaries were not rebuilt: the changed registry header currently
enters the proof path, and normal UI/CLI behavior is unchanged.

Headless Debug smoke passes **9/9** and Release regression passes **30/30**,
including three four-peer terminal-failure cleanup/rejoin runs. Reports with
binary hashes, watchdog outcomes and timing:
`build/webrtc/peer-cleanup-headless-debug/result.json` and
`build/webrtc/peer-cleanup-headless-release/result.json`.

## Connection-scoped capture cleanup - 2026-09-15

Capture subscriptions previously validated only the host-session generation.
A delayed cleanup for a departed connection could therefore remove a replacement
subscription with the same viewer ID. HostMediaSession now requires a connection
generation for AddViewer and RemoveViewer, retains that identity in subscription
snapshots and reports it with subscriber failures. Admission uses strictly
increasing generations within the host session, matching the peer registry;
retired attachments cannot resurrect a subscription after removal. Removing an
absent subscriber remains idempotent, while a mismatched live connection is
rejected before changing delivery.

The coordinator test replaces a subscriber, rejects an old attachment/removal
and waits for new frames. The real four-peer rejoin proof uses actual connection
generations and rejects delayed capture cleanup before checking continued
decoding. This fixes a prerequisite for automatic peer-failure cleanup; the
application executor/room adapter integration remains open.

Validation: Debug and Release media suites pass **24/24** each, and application
builds/suites pass **10/10** each. Logs are
`build/webrtc/connection-guard-sdk-proof-debug.log`,
`build/webrtc/connection-guard-sdk-proof-release.log`,
`build/webrtc/connection-guard-sdk-app-debug.log` and
`build/webrtc/connection-guard-sdk-app-release.log`.

Headless Debug smoke passes **9/9** runs and Release regression passes **30/30**.
Reports (including hashes and watchdog outcomes):
`build/webrtc/connection-guard-headless-debug/result.json` and
`build/webrtc/connection-guard-headless-release/result.json`.

## Host capture/membership coordinator — 2026-09-15

`backend/media/HostMediaSession` now owns CaptureSession and CaptureDistributor on
a serialized control worker. Public futures return operation IDs, session
generations and typed errors; snapshots expose capture state, device generation,
active operation and per-viewer delivery counters. Capture and delivery stay
off the command queue. The portable public interface has no Qt/WebRTC types.

Start accepts an asynchronous source startup; successful command completion
does not guarantee capture is already running. Running means capture with
subscribers, not a connected remote peer. Check snapshots for terminal source
failure. Factory construction and destruction stay on the capture owner thread.

Normal commands have a 64-entry queue limit. A current-generation Stop cancels
queued commands and is accepted despite queue pressure; a stale Stop rejects
without cancelling newer work. Stop joins capture and delivery workers before
returning. Failed subscribers are removed independently. A callback must not
wait synchronously for a command that joins it or destroy its owning session.
Native calls still need the process watchdog; cancellation cannot preempt a
blocked driver call or arbitrary callback.

The four-peer proof now uses this coordinator for capture and subscriber
start/remove/rejoin/stop. Peer construction/signaling, shared audio and settings
application remain in the proof layer. The normal application remains legacy.
This milestone does not complete the production facade or all of Checkpoint B.

### Validation

- Debug and Release native media suites: **21/21 each**, including hardware and
  audio-device targets. Logs: `build/webrtc/coordinator-debug.log` and
  `build/webrtc/coordinator-release.log`.
- Debug and Release application builds and suites: **10/10 each**. Logs:
  `build/webrtc/coordinator-app-debug.log` and
  `build/webrtc/coordinator-app-release.log`.
- Headless Debug smoke: **6/6 child runs**. Artifact:
  `build/webrtc/coordinator-headless-debug/result.json`.
- Headless Release regression: **27/27 child runs**, including 20 single-peer
  process lifecycles and three four-peer scenarios. Artifact:
  `build/webrtc/coordinator-headless-release/result.json`.
- Coordinator test covers 100 session restarts, source owner-thread destruction,
  stale generations, repeated stop, startup failure, isolated failed viewer and
  stop under queue pressure (64 cancelled commands and 16 capacity rejections).

Commands use `scripts/run-webrtc-proof.ps1` with the existing Debug/Release SDK
paths documented in BUILD.md, `-Hardware -AudioDevice` for media and
`-Application` for application builds. Headless commands and limitations are in
[HEADLESS-TESTING.md](HEADLESS-TESTING.md).

### Remaining work

Next integrate peer/signaling ownership, connection generations and asynchronous
settings/events, then expose the complete engine through the shared UI/CLI
facade. Keep the realistic headless path using these same production APIs.
Separate processes, authorized test-owned input and network impairment remain
unimplemented. Existing current-binary native handle-growth failures (+76/+10)
remain open; these synthetic lifecycle tests do not resolve them. See
[CLOSEOUT-A.md](CLOSEOUT-A.md).

No comparative performance improvement is claimed. Matched workloads, external
gaming image/input latency, quality and room-service cost checks are specified
in [COMPARISON.md](COMPARISON.md).

## Trickle ICE candidate handoff — 2026-09-15

Added portable `IceCandidateHandoff`, used by the actual WebRTC media proofs.
Candidates are buffered until local and remote description application succeeds,
then delivered in order without waiting for ICE gathering to complete. The proof
serializes SDP before gathering and asserts it contains no candidate lines.
Thus successful H.264/Opus/data connectivity now depends on separate candidate
handoff, rather than the previous bundled-SDP shortcut.

Each directional handoff has a fresh negotiation identity; stale pushes and
readiness notifications cannot mutate it. Limit each handoff to 64 accepted
candidates over its entire lifetime, with 4096-byte candidate and 64-byte MID
bounds and NUL/index validation. Overflow, malformed data and application failure
close delivery and release queued candidates/callback ownership. Teardown closes
both directions before closing the PeerConnection. All calls stay on the proof
signaling executor; the class is not a thread-safe room transport. The room
adapter must map authenticated connection identities and validate wire messages
before using it. Negotiation failure requires a fresh handoff.

Debug and Release media suites pass **22/22** (`build/webrtc/trickle-debug.log`
and `build/webrtc/trickle-release.log`). Dedicated coverage includes ordering,
readiness barriers, stale/closed callbacks, the 64-candidate bound, invalid
fields and throwing delivery callbacks. Application binaries were not rebuilt:
this milestone changes the reusable header and proof integration, not the
current application path.

This is still local in-process signaling without STUN configuration. Production
peer ownership, authenticated room routing, end-of-candidates wire mapping,
connection deadlines, bounded ICE restart/backoff and actual NAT/network tests
remain open. Native handle-growth failures and comparative latency acceptance
are unchanged. Do not mark the full PeerConnections checklist complete.

Headless Debug smoke passes **7/7** child runs; Release regression passes
**28/28**, including 20 single-peer processes and three four-peer/rejoin runs.
Artifacts: `build/webrtc/trickle-headless-debug/result.json` and
`build/webrtc/trickle-headless-release/result.json`. Both include executable
hashes, watchdog outcomes and component metrics. No manual input was needed.

## Connection lifecycle and real ICE restart - 2026-09-15

Added portable `PeerConnectionLifecycle` with a 20-second initial deadline,
typed direct-connect/remote-close/restart-limit failures and stale connection
event rejection. Disconnections use 500 ms/1 s/2 s backoff and at most three
restart attempts per rolling minute. Repeated notifications do not postpone
backoff; successful reconnection does not reset the rate budget. Transient
recovery before backoff expires avoids a restart. Restart attempts each have a
20-second deadline; expiry reschedules within the rolling budget. This is a
rate bound, not a maximum lifetime attempt count for a long-lived connection.

The proof feeds standards-compliant ICE state callbacks into the policy, checks
initial deadlines throughout description negotiation, and closes the policy
before peer teardown. One four-peer viewer receives an explicit restart request;
the host creates an ICE-restart offer and the viewer answers using fresh
credentials and fresh candidate handoffs. Candidates from retired local
credentials are filtered. The scenario verifies old handoff rejection, unchanged
200 kbps viewer settings, resumed frame delivery and continued healthy viewers,
then performs the existing full leave/rejoin. Credentials are never logged.

The deterministic policy test covers deadline boundaries, late/stale success,
duplicate disconnection, backoff, rolling-budget exhaustion/expiry, attempt
timeout, transient recovery, remote close and isolation from a healthy peer.

Timing investigation: the first combined state/media assertion took about
16 seconds despite negotiation finishing in about 0.6 seconds. Splitting the
assertion showed the 20-frame media check passed at 0.66 seconds while ICE state
confirmation arrived around 16.3 seconds. These are local scenario checks, not
an outage duration or external input/display measurement. The final proof uses
`OnStandardizedIceConnectionChange` instead of the legacy combined ICE/DTLS
callback documented in the pinned header. Keep negotiation, media-check and
state-check timings separate in JSON; do not gate frame delivery on state UI.

Production peer ownership/action dispatch, authenticated restart requests,
remote wire-generation binding, STUN/NAT and real outage/interface-change tests
remain open. This proof explicitly requests recovery; it does not simulate a
network outage or implement automatic application reconnection. Native handle
growth and external latency acceptance are unchanged. Application binaries were
not rebuilt because the new portable header is currently integrated in the proof
path; normal UI/CLI behavior remains legacy.

Final Debug and Release native media suites pass **23/23** each. Logs:
`build/webrtc/peer-lifecycle-final-debug.log` and
`build/webrtc/peer-lifecycle-final-release.log`. The final standardized-callback
Debug smoke measured 578 ms negotiation, 648 ms for the frame-count check and
16,283 ms for state confirmation. Switching callback did not remove the state
confirmation delay; its cause remains open, and it must not be described as a
16-second video outage. Existing AutoThread deprecation warnings are unchanged.

Final headless Debug smoke passes **8/8** runs; Release regression passes
**29/29**, including three complete four-peer restart/rejoin scenarios. Artifacts:
`build/webrtc/peer-lifecycle-final-headless-debug/result.json` and
`build/webrtc/peer-lifecycle-final-headless-release/result.json`. Executable
hashes and watchdog results are retained. Earlier timing runs remain historical
artifacts; use these final paths for the committed implementation.

## Owning peer registry and recovery dispatch - 2026-09-15

Added portable `IMediaPeer` and `HostPeerRegistry`. The registry owns at most
63 peer objects on one signaling/control executor, drives lifecycle actions,
queues asynchronous ICE restart requests and closes timed-out or failed peers.
A failed dispatch closes only that peer and records a distinct dispatch-failure
flag. Terminal snapshots retain their failure reason after native Close changes
the lifecycle state. Remove/Stop/destruction close each accepted peer once and
release ownership; rejected admission closes the supplied peer too.

Admission requires fresh, strictly increasing connection generations. Admit
peers in generation order before starting asynchronous negotiation in the
production adapter; do not order admission by asynchronous completion. This
keeps retired-generation rejection bounded without an ever-growing tombstone
history. Stop is terminal for that registry; a new host session needs a new
registry. Failed rows count toward capacity until removed. Implementations must
keep their lifecycle object alive through Close, cancel queued work on Close,
and never synchronously reenter registry methods from restart/close callbacks.

The four-peer proof now transfers actual peer-pair ownership into this registry.
Its adapter queues a restart; the existing local SDP driver consumes that work
outside Tick, preserving the nonblocking registry contract. The scenario removes
and replaces a peer through the owner, rejects a restart targeting the retired
incarnation and stops capture before releasing all peers. Peer shutdown is
idempotent to support explicit Close followed by object destruction.

Dedicated deterministic coverage checks one-shot restart dispatch, stale
requests/removal, rejected reused generations, throwing dispatch isolation,
initial timeout, repeated stop and close/destruction counts. The fake adapter
in that unit test is separate from the real media scenario.

Remaining: application signaling-executor scheduling, full asynchronous WebRTC
adapter/room delivery, automatic capture subscription cleanup on peer failure,
and UI/CLI facade integration. This does not establish remote network recovery,
resolve the native handle-growth defect or prove better external latency.

Debug and Release media builds/suites pass **24/24** each. Evidence:
`build/webrtc/peer-owner-debug.log` and `build/webrtc/peer-owner-release.log`.
The new owner currently enters the proof build through its portable header;
application binaries were not rebuilt and normal application behavior is
unchanged. Existing AutoThread deprecation warnings remain.

Headless Debug smoke passes **9/9** runs; Release regression passes **30/30**,
including 20 single-peer processes and three four-peer restart/rejoin scenarios.
Artifacts: `build/webrtc/peer-owner-headless-debug/result.json` and
`build/webrtc/peer-owner-headless-release/result.json`. Reports retain executable
hashes, watchdog outcomes and restart timing diagnostics. No manual input was
required. The existing delayed ICE-state confirmation remains unchanged.
## Sender diagnostics continuation — 2026-09-17

Integrated host per-viewer UI rows/inline details and shared CLI status vocabulary.
Requested preferences/revisions, applied caps, source observation and measured
transport remain distinct. Local 1 Hz samples become unknown/stale at three
seconds; no service traffic was added. Receiver telemetry and remote latency
remain open. DETAIL-CHECKS separates this implementation from those requirements.

Release and Debug builds and five-case silent regression matrices passed, as did
the Release stream-settings collector proof. Optional Windows CLI presentation
timed out twice before diagnostics integration; see HEADLESS-TESTING for evidence.
This batch does not close desktop GPU or external performance acceptance.

## Local renderer diagnostics continuation — 2026-09-17

The shared renderer now distinguishes busy, occluded, minimized, unavailable,
backoff, presented and failed outcomes. Viewer UI and live/final CLI JSON expose
drop counters and retained graphics codes; no network telemetry was added. Normal
drops do not spend recovery attempts, and explicit clear resets the current error.
Timeout evidence is actionable rather than an anonymous assertion line.

The previously failing Windows CLI test passed before behavior changes; subsequent
Release and Debug desktop-inclusive matrices passed. This is not a root-cause fix
for the prior intermittent timeout. See HEADLESS-TESTING for exact evidence and
the remaining physical-device, receiver-telemetry and latency limitations.

## Receiver decoder telemetry continuation — 2026-09-17

The native runtime now owns bounded versioned decoder reports over its encrypted
unreliable telemetry channel. Reports bind to negotiation generations, reject
replay/out-of-order/malformed/excess messages, and expire after three seconds.
Only the latest sample is retained, stats requests run at 1 Hz, and no service
requests were added. Host UI/CLI distinguish decoded dimensions/count/FPS from
source observations and presentation. See RECEIVER-TELEMETRY.md for the full contract.

Release/Debug five-case headless matrices and protocol tests passed; the Release
collector proof and four-viewer expiry/recovery/restart/rejoin proof passed.
The CLI preview visibility invariant is corrected and tested, but the optional
desktop matrix still reports DXGI occlusion with no graphics errors. Hardware/
desktop acceptance, remote presentation telemetry and external latency remain open.

## Sender/network details continuation — 2026-09-17

The existing 1 Hz host collector now exposes typed payload rate/encoded FPS,
selected-path RTT/bandwidth estimate, linked RTCP loss/jitter and WebRTC limiting
reasons. Invalid/missing/stale values remain unknown and negotiation changes
replace counter mailboxes. Receiver age uses local monotonic receipt time.
No service requests, adaptation loops or new stats collectors were added.

PeerDiagnosticsWidget now owns the table/inline details and a live nonmodal
peer-pinned popup, removing that logic from RoomSessionWindow. Departure clears
the popup. Duplicate nicknames include peer IDs; host capacity above four warns
about added upload/encoding work. NETWORK-DIAGNOSTICS.md records field semantics
and remaining capture, remote-presentation, codec and input measurements.
