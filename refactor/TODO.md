# Backend v2 — delivery milestones

The approved [PLAN.md](PLAN.md) remains authoritative. The exhaustive checklist and
historical evidence are preserved in [DETAIL-CHECKS.md](DETAIL-CHECKS.md). Individual
checks are implementation details, **not separate user turns**.

## Working agreement

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
explicit `--desktop` extension. Separate native host/viewer processes, scripted
authorized input, network impairment and continuous soak acceptance remain open.

## 2. Complete user experience (B + D)

The existing v2 browser and config-session entry points now run inside the normal
AppShell, with in-window navigation, shared screen-awake handling and asynchronous
close across media/directory workers. Repeated sessions release their pages; failed
join returns to the browser. Ordinary home/create/join and CLI action routing still
needs migration and must preserve control features. See ROOM-UI.md.

**Remaining work:** ordinary UI/CLI adoption, video pipeline/recovery completion,
audio processing/physical-device acceptance, and remaining diagnostics/integration
checks. [STAGE-2-REMAINING.md](STAGE-2-REMAINING.md) separates these four groups from
Stage 3 gaming, Stage 4 acceptance and Stage 5 cutover dependencies.

Detected audio startup/live failures now release the endpoint and preserve video
through paced silence/discard. Public health, UI retry feedback and CLI status/
scripted same-device recovery are integrated. No automatic device retry or service
polling is added. Physical driver acceptance remains open; see AUDIO-RECOVERY.md.

Per-viewer GPU NV12 scaling/letterboxing is integrated with owned textures, four
in-flight submissions per device, drop-on-pressure and quarantined CPU fallback.
UI/CLI show the actual source scaling path and active image rectangle. Dropped
frames no longer acknowledge unapplied dimensions. [GPU-SCALING.md](GPU-SCALING.md)
documents ownership and validation; gaming coordinate mapping remains open.

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
Remote presentation telemetry, capture/input reasons and latency remain unfinished; sender
observation must not be reported as remote display acknowledgement.

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
Default-shell adoption, input consent/control, hardware decode/GPU zero-copy
presentation and physical audio-device loss/recovery acceptance remain.
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

- [ ] Complete input integration and adversarial/headless control scenarios.

## 4. Stability, performance and service acceptance (B + C + E)

**Deliverable:** reproducible evidence against the original targets and a matched
legacy/v2 comparison, with failures resolved or explicitly blocking cutover.

Include impairment/slow-viewer/device recovery, 100 start/stops, two-hour four-viewer
soak, current capture handle-growth failures (+76/+10; bound 8), queue pressure,
hibernation, service caps/cost/headroom, real TLS/NAT and external gaming image/input
latency. See [COMPARISON.md](COMPARISON.md).

- [ ] Pass implementation-side stress and service/resource acceptance.
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
