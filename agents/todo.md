# TODO

This file tracks unfinished work only. Completed release milestones belong in Git history and release notes.

## Active Priorities

### Backend v2 refactor — grouped delivery work

Priority correction: the user requests Stage 2–4 completion before
cutover. Read refactor/STAGE-2-4-ACCEPTANCE.md for the remaining acceptance boundary.
The user returned with a Windows laptop and GameSir Nova Lite. The read-only field
scene and concise portable-test workflow are prepared; see refactor/FIELD-TESTING.md.
Isolated ten-room v2 deployment needs the pending explicit approval. Do not deploy
the v1 service or install a controller driver.
UI/CLI redacted JSON reports and input pressure/local timing diagnostics are
implemented and verified (SESSION-REPORTS.md); do not reimplement them.
Unattended results: refactor/UNATTENDED-RESULTS.md. The two-hour software run is now
collected and validated after a timing-check correction; final short runtime
checks pass both builds. Final network evidence retains a Release recovery failure.
Continue that performance investigation and the listed physical/resource/field
gates, not another reporting/cost-harness implementation or an unqualified soak rerun.

Current continuation: the shared input service, controller and mouse/keyboard
UI/CLI/device groups are implemented. See [DESKTOP-INPUT.md](../refactor/DESKTOP-INPUT.md).
Capture stress now has a four-mode runner and production Windows owner checks;
see [RESOURCE-STRESS.md](../refactor/RESOURCE-STRESS.md). Full-room restart and
continuous runners are also implemented; see [ROOM-STRESS.md](../refactor/ROOM-STRESS.md).
The reproduced retained handles are WGC/RPC ALPC ports; typed tracing and separate
idle observations are implemented (CAPTURE-HANDLES.md). Normal-policy handles
return below warm-up after five minutes idle. Do not repeat that investigation or
add speculative apartment/thread changes. Full-room ownership/heap accounting and
the real UI/CLI one-frame presentation impairment scenario are now implemented.
See ROOM-STRESS.md for results and limitations. The sustained receive-rate decline
was traced to capture timeout silence followed by immediate late PCM, overfeeding
audio and delaying synchronized video. The fixed output cadence and regression
are implemented; see RECEIVER-PACING.md. Two-hour software evidence is collected;
finish remaining resource and network acceptance with synchronization enabled.
Do not add another frame queue or compact memory to
improve the measurements. Keep the immediate +8 bound; delayed OS cleanup
does not waive it. Preserve Stage 2
physical/parity and Stage 3 physical input gates. Recording-device integration
does not establish physical confinement, controller behavior or gaming latency.

Use [refactor/TODO.md](../refactor/TODO.md) for delivery milestones and
[DETAIL-CHECKS.md](../refactor/DETAIL-CHECKS.md) for the full acceptance requirements.
Complete integrated features with backend/UI/CLI wiring, silent headless tests,
documentation and logical commits. Do not split each helper/check into a user turn.

The shared scheduled room facade, authenticated media composition, opt-in UI/CLI,
live directory/profile/policy, source/audio/playback changes, aggregate allocation
and retained NV12 handoff are integrated. UI and CLI now share a backend-owned
renderer/session with bounded recovery; the duplicate CLI GPU pipeline is removed.
The detailed checklist is reconciled, and test-room-regression.py runs the complete
silent production UI/CLI/service scenario matrix with process-tree watchdogs.
Validated local stream/playback defaults and stable randomized Guest nicknames are
integrated into browser-created sessions with explicit save controls; do not rebuild
profile persistence. Config-file sessions remain explicit and independent.
Do not rebuild those foundations or return to manual SDP relays/polling.
Use refactor/STAGE-2-REMAINING.md for the three remaining Stage 2 delivery groups.
The existing v2 UI entry points now use the normal AppShell with one-window page
navigation and coordinated media/directory shutdown. Reuse RoomApplication for
further adoption; ordinary legacy action routing and CLI migration remain open.
Native presentation now handles minimized root windows behind child surfaces and
skips hidden-target GPU work. Windows UI/CLI scenarios pass after hidden-startup
and WGC letterbox fixture corrections; earlier visible-but-occluded cases still
need acceptance. Do not conflate these with hardware decode/zero-copy completion.
Reported audio endpoint failures now preserve video with explicit same-device
recovery and visible UI/CLI health; do not reimplement recovery. Physical device
acceptance remains open. Microphone-only processing and native-channel stereo
downmix are implemented; see refactor/AUDIO-PROCESSING.md. Do not repeat these
software tasks or mark physical format/quality/unplug checks passed synthetically.
Per-viewer GPU scaling/letterboxing now preserves owned NV12 frames into hardware
encoding, drops excess GPU submissions, and quarantines to CPU fallback on failure.
The active image rectangle is exposed in source diagnostics; authorized input
mapping now follows each encoded/decoded/presented frame. Hardware decode/GPU presentation and bounded decoder
fallback are now integrated; see refactor/GPU-RECEIVE.md. Remaining video work is
display capture fallback, source identity/privacy/cursor/HDR/state completion and
physical/occlusion acceptance. Do not recreate the receive pipeline.
Device-free `none` audio is integrated across startup/live runtime/UI/CLI selection;
it releases host capture and preserves the track for resumption. Physical recovery,
physical audio quality and audio latency remain open.
Host per-peer sender diagnostics now share UI/CLI states and expose transport
freshness. Receiver decoder telemetry now uses the encrypted peer channel with
bounded versioned reports, generation/sequence validation and three-second expiry.
Remote presentation/drop/buffering, allowlisted codecs, shared hardware fallback,
capture/recovery states and retained per-viewer applied settings are integrated.
Preset preservation and partial application/retry now have native plus UI tests.
See refactor/DIAGNOSTICS.md; do not rebuild this group. Input timing and physical
latency, capture/presentation rate and encoder pending-age measurement remain open.
Local renderer outcomes/drop reasons/error codes now reach viewer UI and CLI;
keep them distinct from receiver-to-host telemetry and end-to-end latency.
Normal AppShell/legacy CLI cutover remains gated; later visual redesign follows
milestones 1–5.

- [ ] Complete remaining user-experience adoption: default-shell integration,
  capture fallback/source behavior and actual device recovery, preserving existing
  source/playback controls and explicit pending/applied/rejected state.
- [ ] Deliver gaming input end to end: encrypted channels, first-use consent,
  ownership/generation validation, bounded reliable queues, coalesced state,
  300 ms neutralization, confinement, source-change release and panic revoke.
  Headless tests must target only owned sinks/windows; no physical input.
- [ ] Pass implementation stress/resource/service acceptance: resolve measured
  capture handle growth (+76 full cycles/+10 rapid close; bound 8), 100 restarts,
  two-hour four-viewer soak, slow peers, device recovery, queue pressure,
  hibernation/reconstruction, caps, alarm timing and service-cost headroom.
- [ ] Obtain matched legacy/v2 comparison and required real TLS/NAT and external
  image/input latency evidence. Local correctness/GPU recreation under injected
  errors does not establish physical driver-removal or latency acceptance.
- [ ] After acceptance, complete default cutover, obsolete transport/runner removal,
  upgrade/namespace/installer/runtime packaging and fresh-machine validation.
  Publishing/deployment remain separate actions.
- [ ] After milestones 1–5, complete the UI appearance/usability redesign.

Keep audio tests synthetic and silent. Audible endpoint tests require explicit
opt-in. Preserve installer-managed controller drivers, update signature checks,
platform/security tests and backend/frontend dependency separation.

### Release infrastructure

- [ ] Validate the cross-colo rate limiter, global room-count cap, and alarm-based directory sweep under multi-colo/live load.
- [ ] Exercise an encrypted session through a real NAT rebind before relying on automatic retargeting in the field.

### Performance and latency

- [ ] Profile and reduce latency and CPU use across capture, encode, send, receive, decode, and presentation.
- [ ] Evaluate trimming viewer audio buffering and confirm that the encoder never retains frames for lookahead.

### Session UX and diagnostics

- [ ] Map known runtime/report states to plain UI messages with actionable next steps, starting with waiting for stream, encryption mismatch, UDP hole-punch failure, host departure, and host idle.
- [ ] Make active-session wording and NAT/feedback summaries freshness-aware and consistent across join, leave, rejoin, and host departure.
- [ ] Add only the report fields required by these diagnostics. Warn about silent or incorrect audio devices only when transport is healthy and the evidence supports it.

## Deferred Engineering

### Window behavior

- [ ] Keep custom title-bar minimize, maximize/restore, and close behavior consistent on active Watch screens, maximized windows, and embedded preview surfaces.
- [ ] Keep drag-to-move, drag-to-maximize, resize borders, and rounded-corner outlines consistent across normal, maximized, and fullscreen transitions.

### Streaming features

- [ ] Add adaptive FPS after defining a runtime policy for when FPS should rise or fall.
- [ ] Add encoder preference and preset switching after the runtime can change encoders safely during a session.
- [ ] Consider optional UPnP/NAT-PMP port mapping only if repeated reports show direct Worker/STUN rooms failing on important networks.

### Platform dependencies

- [ ] Replace the retired ViGEm runtime with a maintained signed backend after validating XInput-only game compatibility and driver install/update costs.

### Code and tooling

- [ ] Promote advanced CLI-only Share/Watch diagnostic flags into typed configs only if they become normal application controls.
- [ ] Split diagnostic-only commands out of `ScreenShareCLI.cpp` if they begin crowding the parser.
- [ ] Add a richer debug overlay only if title telemetry or UI diagnostics become too dense.
- [ ] Consider making CLI plaintext mode require `--allow-plaintext` after the encrypted/invite flow is smooth enough to avoid surprising users.
- [ ] Keep helper CLI commands only where they are genuinely diagnostic rather than normal UI data paths.

## Evidence-Driven Follow-Ups

Do not schedule these without a report or measurement showing the relevant problem.

### Audio and A/V sync

- [ ] Include receiver A/V sync and playout fields in receiver feedback summaries when sender reports need to diagnose values such as `av -600ms`.
- [ ] Investigate small-drift audio time-stretching if A/V catch-up drops become audible or frequent; retain hard drops for large real-time recovery.

### Encoder and transport

- [ ] Improve `--stream-encoder auto` if reports show persistent hardware encoder input drops are not handled adequately.
- [ ] Measure and report maximum encoder input queue age when diagnosing visible latency.
- [ ] Prefer dropping stale queued encoder input before it becomes visible latency when measurements justify the policy.
- [ ] Revisit the fixed UDP pacing headroom if reports show backlog; make it adaptive only when measurements support doing so.
- [ ] Avoid arbitrary UDP media drops inside a GOP unless paired with a keyframe recovery strategy.
- [ ] Consider requesting or forcing a keyframe after sender-side frame skips if reports show recovery is too slow.
