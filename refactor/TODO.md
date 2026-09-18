# Backend v2 — delivery milestones

The approved [PLAN.md](PLAN.md) remains authoritative. The exhaustive checklist and
historical evidence are preserved in [DETAIL-CHECKS.md](DETAIL-CHECKS.md). Individual
checks are implementation details, **not separate user turns**.

## Working agreement

Backend feature scope is frozen for closeout. Finish these groups in order:

1. Paired legacy/v2 measurements and an explicit per-metric scorecard (COMPARISON.md).
2. Resolve measured regressions, initial congestion backlog and resource acceptance.
3. One consolidated two-PC/device/network acceptance pass, preserving physical latency gates.
4. Stage 5 default cutover and obsolete-code removal, then Stage 6 frontend redesign.

Do not add unrelated backend features or repeat completed v2-only harness work.

The first paired comparison is superseded: its custom unpaced silent sink distorted
CPU and synchronized-video measurements (COMPARISON.md). Use the corrected shared
paced sink and repeat the comparisons. Default-pacing high-motion sender delay
still reproduces after that correction. Reference-load pacing and background-timer
fixes pass final local validation. The corrected 24-run configuration comparison
favors v2 on image age and CPU, but does not isolate architecture from hardware
codec/timer/pacing choices. Portable precise waits and the single-submission
hardware deadline are now shared with legacy; its old raw encoder queue is
removed. Use the updated controls in COMPARISON.md / HARDWARE-ENCODING.md.
The latency and software-throughput follow-ups below resolve the main reference
delivery regressions. Prioritize four-viewer resources, then normal GPU/two-PC
and physical acceptance. Do not rebuild the comparison harness or claim an
architectural win from fixes that also improve legacy.

Capture decision completed: retain the already-shared `DesktopCapturer` behind
v2's modular owned-frame interface. Capture-only measurements did not reproduce
the whole-pipeline gap. Codec-stage traces instead found a retained decoder
frame; the complete-picture input and verified low-latency fix are shared with
legacy. See CAPTURE-LATENCY.md and the newest COMPARISON.md scorecard. Continue
four-viewer resources and the existing acceptance gates without
reopening the capture rewrite merely from whole-pipeline latency numbers.

Software-throughput group completed: shared software CABAC, stable cadence and
bounded GPU readback scratch restore reference delivery to 52.9/48.2 fresh FPS
for one/four v2 viewers. All sixteen fair controls pass with CABAC in legacy too;
hardware v2 now has lower local image age and CPU than tuned hardware legacy.
See SOFTWARE-THROUGHPUT.md and COMPARISON.md. Next measured resource issue:
four software viewers still use 43% more CPU and 85% more private memory than
legacy; four hardware viewers use 31% more private memory. Keep resource,
congestion and physical acceptance open before cutover/frontend redesign.

Current priority: finish Stage 2–4, preserving the
physical/remote gates listed in [STAGE-2-4-ACCEPTANCE.md](STAGE-2-4-ACCEPTANCE.md).
The user is back with a Windows laptop and GameSir Nova Lite. Use the concise
[FIELD-TESTING.md](FIELD-TESTING.md) handoff for real-machine acceptance. An isolated
ten-room v2 service is now approved and deployed; both PCs pass HTTPS health.
Laptop SSH and portable startup work; see FIELD-TESTING.md for actual test limits.
First two-PC viewing/resize works by user observation. It exposed a native sizing
bug with misleading presentation counters, reproduced and fixed locally; see
[PRESENTATION-SIZING.md](PRESENTATION-SIZING.md). The user confirmed the updated viewer
works; fresh laptop telemetry now records 8,977 presented frames and 59 fps hardware
decoding. Physical latency and controller acceptance remain open.
The event-driven four-viewer native lifecycle now passes against the deployed
HTTPS/WSS service on both PCs; see [LIVE-SERVICE.md](LIVE-SERVICE.md). Earlier
timed CLI failures are now reproduced and resolved as fixture ordering errors.
Bidirectional LAN media, restart/rejoin, settings and synthetic input-response
checks pass; see [TWO-MACHINE.md](TWO-MACHINE.md). NAT/interface-change, physical
devices and external-latency tests are still open.
The expanded Release desktop matrix passes 17/17, including delayed viewer,
generated-window capture, hardware presentation and native UI/CLI control flows.
The unattended batch results are in [UNATTENDED-RESULTS.md](UNATTENDED-RESULTS.md):
two-hour software evidence is collected; its Release bandwidth-recovery failure
was investigated in the following batch. Device/resource/field gates remain. Do not repeat
completed reporting, input diagnostics or cost-model implementation.
The subsequent investigation found the missing connection bitrate budget behind
the 5 Mbps probe ceiling; see [CONGESTION-RECOVERY.md](CONGESTION-RECOVERY.md).
Corrected Release packet scenarios pass 5/5 and Debug collapse passes, with traced
before/after evidence. Receiver latency, full queue age and
the original hardware/resource/field gates remain separate acceptance items.
Gaming now requests short (10 ms) video playout using per-frame preset metadata;
Quality restores adaptive buffering. See [PLAYOUT-RECOVERY.md](PLAYOUT-RECOVERY.md)
for matched repeated input-response evidence and the severe-collapse FPS tradeoff.
The final nonzero request avoids a pinned-SDK preset-switch freeze; laptop
preset/resize/rejoin and direct CPU/GPU decoder resize checks pass without changing
the decoder's existing resize behavior.
Continue transient sender/network backlog, reference-load latency and physical
A/V acceptance; do not equate reduced receiver buffering with completed Gaming latency.
Decoded-frame handoff ownership/timing now covers failed conversion, stop discard
and in-flight work, with shared UI/CLI diagnostics and strict stress/network
evidence checks. See [FRAME-HANDOFF.md](FRAME-HANDOFF.md). This measures one local
queue; complete queue-age and external latency acceptance remain open.
UI/CLI reports and local input timing/pressure diagnostics are now implemented
and pass both 12-case headless matrices; see [SESSION-REPORTS.md](SESSION-REPORTS.md).

Native code is organized into root-level `backend/` and `frontend/`; the room
service remains `signaling-worker/`. Backend targets must not depend on frontend
headers. Folder organization does not imply the legacy runtime has been replaced.

- Continue through an end-to-end deliverable, including integration, meaningful
  failure tests, documentation and logical commits.
- A helper, interface, isolated test or successful build is not a stopping point.
- Intermediate commits are checkpoints within a work batch; keep working afterward.
- Preserve every original security, latency, resource and cutover gate. Never mark
  hardware/remote tests passed using localhost evidence. Report unfinished scope.

## 1. Room-backed media session — complete for local integration

**Deliverable:** a reusable native session path and one-command headless scenario
that create/join real v2 rooms, negotiate actual media through authenticated room
messages, deliver paced frames/audio and stop cleanly.

The public RoomSession now owns admission, networking/signaling executors,
authenticated membership/signal routing, status and asynchronous shutdown.
NativeRoomRuntime composes capture, per-viewer sources, native peers, negotiation,
initial stream preferences and the shared restart/retirement budget.
WindowsRoomRuntimeFactory binds WGC capture and WASAPI audio, selects the capture
device before constructing hardware codecs, and retains device-retirement fallback.

The public headless scenario uses the production runtime with synthetic endpoints.
It validates four independent H.264/Opus viewers, authenticated restart, fresh
rejoin, admission cancellation, media-drain barriers, capture startup failure and
single-viewer delivery-failure isolation. The Windows variant also passes using
a generated WGC window and synthetic audio, without physical input. Application
and CLI builds link the same native adapters; no diagnostic runtime implementation
is needed by the backend.

Evidence and scope: [CHECKPOINT-B.md](CHECKPOINT-B.md) and
[HEADLESS-TESTING.md](HEADLESS-TESTING.md). This closes local composition/integration,
not latency, resource, NAT/TLS, service-cost or cutover acceptance. Default UI and
existing CLI commands remain legacy until adoption and the required acceptance gates.

- [x] Complete the integrated media-session deliverable and headless scenarios.

`scripts/test-room-regression.py` now runs the production room/UI/CLI and mutation
recovery scenarios as one silent matrix, with repeated rounds, process-tree cleanup,
bounded logs and hashed JSON evidence. Generated Windows capture/GPU tests are an
explicit `--desktop` extension. Separate native host/viewer processes and seeded
encrypted-packet impairments now pass in both builds; see NETWORK-IMPAIRMENT.md.
Scripted authorized input uses recording sinks. Physical input, external latency
and continuous soak acceptance remain separate gates.

## 2. Complete user experience (B + D)

The existing v2 browser and config-session entry points now run inside the normal
AppShell, with in-window navigation, shared screen-awake handling and asynchronous
close across media/directory workers. Repeated sessions release their pages; failed
join returns to the browser. Explicit `--backend v2` now routes the existing normal
Home share/join/quick-join actions and no-JSON CLI create/join commands through
the shared backend. Defaults, one directory subscription, update scheduling and
shutdown are preserved. See ADOPTION.md; full control/reporting parity remains gated.

**Remaining work:** full adoption parity, supported-desktop/physical video
acceptance and physical audio-device/format/quality acceptance.
[STAGE-2-REMAINING.md](STAGE-2-REMAINING.md) separates these three groups from
Stage 3 gaming, Stage 4 acceptance and Stage 5 cutover dependencies.

Hardware decode and GPU receive presentation are integrated through both v2
frontends, including fixed-size software fallback, bounded keyframe recovery,
visible-aperture ownership and delayed-frame validation during resize. See
[GPU-RECEIVE.md](GPU-RECEIVE.md). Display-only fallback, pinned-source recovery,
GPU cursor composition and minimized/closed lifecycle states are now implemented;
see [CAPTURE-RECOVERY.md](CAPTURE-RECOVERY.md). Supported-desktop DXGI, physical
privacy/identity/HDR/driver and latency acceptance remain open. The current
Screen-saver block was observed in the capture batch; the interactive desktop is
available again for adoption checks, but DXGI still reports unsupported. Stage 3
mouse/keyboard mapping, confinement and UI/CLI consent are now integrated; see
DESKTOP-INPUT.md. Physical input acceptance, full parity and gaming latency remain
open; Stage 2 is not complete.

Detected audio startup/live failures now release the endpoint and preserve video
through paced silence/discard. Public health, UI retry feedback and CLI status/
scripted same-device recovery are integrated. No automatic device retry or service
polling is added. Physical driver acceptance remains open; see AUDIO-RECOVERY.md.

Microphone speech processing and explicit native-channel stereo downmix are now
implemented, with live UI/CLI processing status, failure/retry and switch-away
tests. System/process/None bypass the speech processor. See AUDIO-PROCESSING.md;
the physical audio acceptance group remains open rather than being marked passed
by synthetic tests.

Per-viewer GPU NV12 scaling/letterboxing is integrated with owned textures, four
in-flight submissions per device, drop-on-pressure and quarantined CPU fallback.
UI/CLI show the actual source scaling path and active image rectangle. Dropped
frames no longer acknowledge unapplied dimensions. [GPU-SCALING.md](GPU-SCALING.md)
documents ownership and validation; gaming coordinate mapping is integrated in DESKTOP-INPUT.md.

Device-free **No shared audio** now works at startup and through live host UI/CLI
changes. Capture factories are bypassed, the old endpoint is released, and failed
resume retains silence; the negotiated track permits resuming without reconnecting.
Silent UI/CLI and four-viewer tests verify continued video and resumed audio.
This does not close physical endpoint recovery, microphone processing or latency gates.

Per-viewer sender/network measurements and the live details popup are integrated.
Diagnostics are now a separate snapshot-only UI component; duplicate member names
show peer IDs and host capacity above four gets a cost warning. WebRTC-reported
limiting reasons remain distinct from application settings states. See
[NETWORK-DIAGNOSTICS.md](NETWORK-DIAGNOSTICS.md) for fields, validation and remaining
capture/remote-presentation/input metrics. No competing adaptation loop was added.

Host per-viewer diagnostics now integrate UI rows/inline details and CLI JSON:
requested preferences, applied allocation, source observation and transport sample
freshness. They reuse local 1 Hz WebRTC stats without service traffic. Receiver
decoder reports now reach host UI/CLI over the encrypted peer telemetry channel.
The diagnostics/integration group now adds remote presentation/drop/buffering,
codec/fallback and capture/recovery observations, retained applied preferences,
partial-application/retry feedback and preset-preservation tests. See
[DIAGNOSTICS.md](DIAGNOSTICS.md). Input and physical latency remain unfinished;
sender/renderer observations must not be reported as physical display proof.

Viewer-local presentation diagnostics now expose actual drop reasons, retained
graphics errors and recovery state in UI/CLI. Windows timeout failures now record
the stage, counters and window state.

The timeout later reproduced with `visible=0`, `outcome=occluded`, zero errors.
Explicit CLI preview creation now ensures visibility when the launcher's hidden
startup state overrides its first ShowWindow call. Tests assert visibility before
waiting for frames. Decoder telemetry implementation details and remaining scope:
[RECEIVER-TELEMETRY.md](RECEIVER-TELEMETRY.md).
The subsequent desktop check confirmed visibility but still reported DXGI
occlusion and zero presented frames/errors. Visibility correction alone does not
resolve desktop presentation acceptance; do not count that timeout as passed.
The Qt renderer fixture's separate hidden-startup failure is now diagnosed and
corrected. Production rendering also recognizes minimized root windows behind
child surfaces and drops hidden/minimized frames before GPU work. This does not
establish a fix for the earlier visible-but-occluded CLI case. See ROOM-UI.md.
Desktop validation also exposed and fixed GPU device destruction invoking its
worker from WebRTC's restricted signaling executor. Final release now joins first;
the targeted regression keeps all invoke restrictions enabled. See GPU-SCALING.md.

**Deliverable:** existing UI and CLI use the shared v2 backend, including capture/
audio selection, presentation, Auto/Manual/Gaming settings, saved nickname, live
directory, room policy and viewer diagnostics.

Include pending/applied/error settings, aggregate upload allocation, recovery,
profile persistence, room links, source changes and cancellation. Preserve appearance
and supported behavior. Do not change the default before required coverage exists.

- [ ] Complete UI/CLI adoption and settings/profile/presentation integration.

Integrated foundation: the public session accepts bounded host live-settings
commands and exposes per-peer applied/source-observed revisions and rejection.
The production runtime updates existing senders and gives joining peers the latest
preferences. Four-viewer headless coverage exercises resolution/FPS/bitrate changes,
restart/rejoin and invalid/unauthorized/stopped commands.

The opt-in `ScreenShare --room-v2 CONFIG.json` frontend now uses the shared backend
for host/watch, source/audio selection, timed settings, status and graceful stop.
Its viewer preview has a one-frame latest-value handoff and owner-thread NV12
conversion/rendering. Synthetic and generated-window Windows scenarios validate
the same CLI controller; see [ROOM-CLI.md](ROOM-CLI.md).

The opt-in `ScreenShareUi --room-v2 CONFIG.json` window now uses the same parser,
backend and existing video widget/style. Its Qt owner coalesces live edits, reports
pending/applied/rejected settings, and drains asynchronously before window close.
Offscreen and generated-window tests exercise actual widgets without physical
input; see [ROOM-UI.md](ROOM-UI.md).

The opt-in `--room-v2-browser HTTPS_ORIGIN` path now adds create/join forms, capture
selection, public/unlisted visibility, passwords, a saved validated nickname and
a pushed room directory. It stops subscribing when hidden for a session and
resubscribes on return; no separate admission preflight or list polling is added.
Live session nickname and host policy edits now use bounded, acknowledged backend
commands with revision conflicts, pushed member/policy state and explicit uncertain
outcomes. UI drafts survive conflicts; directory names, capacity and visibility
update through the existing push subscription while media keeps running.
Versioned room-ID links now support copy/paste browser joins and shared CLI/config
parsing without credentials or service switching. Delayed-ack fault coverage verifies
the real timeout, no retry, late-response isolation and continued media.
Optional shared upload allocation is integrated through runtime, UI and CLI: audio/
overhead reservations, equal video shares, individual caps, membership redistribution,
video pause/resume and separate measured transport rates. Four-viewer and actual-widget
tests exercise these behaviors; external congestion/latency acceptance remains open.
Live display/window switching now preserves the room and media peers, commits on
the replacement's first frame, and retains a healthy previous source on failure.
UI selection and timed CLI/config changes share the bounded backend operation.
Live host system/microphone/process audio switching now uses a bounded first-PCM
handover with rollback, timeout, cancellation and a 30ms application capture bound.
UI selection and timed CLI audioChanges use the same public operation. Silent
headless tests cover actual decoded audio, video continuity and endpoint ownership.
Viewer-local output-device, volume and mute changes are now integrated through the
same session/UI/CLI path. They preserve video and membership, add no playback queue,
retain the old output on replacement failure, and report independent application.
The UI/CLI now retain packed decoded NV12 directly through presentation, removing
the NV12/I420 round trip and UI pixel copy. One-frame DXGI queue configuration and
nonblocking present/drop accounting are tested with actual Windows renderers.
The actual UI presentation worker now uses the shared three-rebuild/250 ms recovery
policy, releases failed frames, stops on exhaustion/nonrecoverable errors, and
reports a visible rejoin instruction. Headless pressure/failure tests and injected
loss with real GPU resource recreation cover this path; physical driver loss remains open.
UI and CLI now share the backend-owned renderer/session and recovery policy.
The duplicate CLI D3D pipeline has been removed; preview controls, legacy frame
entry points, terminal diagnostics, resize recovery and independent window closure
are covered by the integrated presentation tests. Do not recreate a second renderer.
Guarded normal-shell adoption, input consent/control and GPU presentation are now
integrated. Full parity and physical audio-device loss/recovery acceptance remain.
This milestone is not complete and defaults are unchanged.

Local profile defaults now include all stream modes/limits and viewer volume/mute,
with explicit save controls in browser-launched sessions and stable randomized Guest
names for new/invalid profiles. New sessions consume validated defaults; saving a
draft does not apply it to active media. Config-file UI/CLI sessions stay explicit.
Corruption/write-failure tests preserve safe defaults and exclude credentials or
device/source identifiers. Profile persistence is no longer an open implementation item.

## 3. Gaming controls end to end (D)

**Deliverable:** authorized mouse/keyboard/gamepads over encrypted data channels,
with consent/revoke/confinement, generation/sequence checks, bounded queues and
watchdog neutralization. Headless input targets only a test-owned sink.

Validate input isolation and input-to-frame response under media pressure; preserve
installer-managed drivers and the three-pad/local-slot policy.

- [x] Integrate the portable input service, encrypted channels and public input
  port; validate adversarial protocol/safety cases and a test-owned input-to-image
  response with four actual media viewers. See [INPUT.md](INPUT.md).
- [x] Complete Windows mouse/keyboard mapping, confinement and normal UI/CLI consent
  together, including panic revoke and focus/source transitions. See
  [DESKTOP-INPUT.md](DESKTOP-INPUT.md) for exact-frame mapping and recording-sink tests.
- [ ] Complete physical input/confinement and impaired-network/latency acceptance.
  Stage 3 and Gate D remain open; local integration does not establish these gates.
- [x] Deliver controller devices, selected-device polling, UI/CLI consent and
  revoke, panic/focus/source/unplug handling, and injected end-to-end tests.
  See [CONTROLLERS.md](CONTROLLERS.md). Physical controller acceptance stays open.
  Next implementation group: Stage 4 stress/resource and impairment acceptance.

## 4. Stability, performance and service acceptance (B + C + E)

**Deliverable:** reproducible evidence against the original targets and a matched
legacy/v2 comparison, with failures resolved or explicitly blocking cutover.

Include impairment/slow-viewer/device recovery, 100 start/stops, two-hour four-viewer
soak, historical capture handle-growth failures (+76/+10; bound 8), queue pressure,
hibernation, service caps/cost/headroom, real TLS/NAT and external gaming image/input
latency. See [COMPARISON.md](COMPARISON.md).

- [x] Add production Windows capture-owner lifecycle stress and a one-command
  four-mode resource matrix, retaining the +8 handle bound. See
  [RESOURCE-STRESS.md](RESOURCE-STRESS.md); repeated handle passes do not establish
  private-memory or complete room-session acceptance.
- [x] Add same-process full-room restart and continuous four-viewer harnesses;
  validate 100 complete rooms in both builds and short continuous media runs.
  See [ROOM-STRESS.md](ROOM-STRESS.md). The 500-cycle capture extension exposed
  delayed handle retention (+226); this failure remains an acceptance blocker.
- [x] Identify the reproduced retained handle allocation path (WGC/RPC ALPC
  connections), provide a repeatable typed-handle trace and separate bounded
  post-stop observations from restart acceptance. See [CAPTURE-HANDLES.md](CAPTURE-HANDLES.md).
  Normal-policy handles return below warm-up during idle; do not repeat that
  investigation. Continue with full-room soak and network impairment work while
  preserving the immediate capture failure as an open acceptance issue.
- [x] Add full-room weak-owner/endpoint/capture-resource accounting, read-only heap
  and virtual-memory snapshots, separate post-stop observations, and slow consumption
  through the actual UI/CLI presentation buffer. Validate per-viewer progress,
  one pending frame, replacement and recovery without catch-up bursts. See
  [ROOM-STRESS.md](ROOM-STRESS.md) for measured results and remaining limitations.
  The 180-second scenario exposed audio overproduction and consequent receive
  buffering; the capture cadence fix and buffering regression are now implemented.
  See [RECEIVER-PACING.md](RECEIVER-PACING.md). Proceed with full soak/network
  impairment acceptance, preserving A/V synchronization and both buffering/rate checks.
- [ ] Pass implementation-side stress and service/resource acceptance, including
  capture handle retention, full-room memory accounting, the complete two-hour
  four-viewer soak and impairment. Do not rebuild the completed stress harnesses.
- [ ] Complete required real-machine/network/external latency measurements.

## 5. Cutover, removal and release readiness (E)

**Deliverable:** validated default v2 behavior and removal of obsolete UDP, polling,
adaptation, NAT invite and old runner code; preserve useful platform/security tests.

Include upgrade behavior, separate namespaces, dependency/runtime packaging,
installer/fresh-machine checks and documentation. Deployment/publishing remain
separate authorized actions.

- [ ] Complete safe cutover and legacy cleanup after acceptance.

## 6. UI design and usability refactor — after the current refactor

**Deliverable:** improve the app's appearance, interaction and overall feel after
milestones 1–5 are finished. Keep current UI work focused on the integration already
planned above; this later phase is not a reason to defer necessary UI changes now.

Review the complete create/join/share/watch journey, then establish consistent
layout, typography, spacing, colors, controls and visual hierarchy. Improve clear
feedback for connecting, recovery, settings and errors; simplify common actions;
cover keyboard navigation, focus, accessibility, high DPI and window resizing.
Preserve low-latency presentation, input safety and the shared backend boundary.
Validate the redesign with representative end-to-end tasks, visual checks and
the headless UI scenarios before replacing the current interface.

- [ ] Complete the later UI appearance, feel and usability refactor after milestones 1–5.

## Foundation and evidence

Gate A passed its original integration/build criterion; resource/performance
acceptance remains open. Native Qt clients pass actual workerd protocol integration.
Neither fact means normal sessions have switched to v2.

- [CLOSEOUT-A.md](CLOSEOUT-A.md)
- [CHECKPOINT-B.md](CHECKPOINT-B.md)
- [CHECKPOINT-C.md](CHECKPOINT-C.md)
- [HEADLESS-TESTING.md](HEADLESS-TESTING.md)
- [DETAIL-CHECKS.md](DETAIL-CHECKS.md) — original checks, evidence and deferred work
