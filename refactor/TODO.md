# Backend v2 — implementation checklist

Saved: 2026-09-14. Implementation status: **Checkpoint A in progress; Gate A not passed**.

Specification: [PLAN.md](PLAN.md). The plan is authoritative; this checklist tracks execution and evidence.

## Start/resume instructions

1. Read the complete plan before changing implementation.
2. Read applicable repository instructions and inspect the current working-tree diff.
3. Resume the first unfinished checkpoint below. Do not skip a gate because compilation or localhost streaming works.
4. Preserve unrelated changes. On 2026-09-14 the user authorized removing unnecessary legacy edits; those UDP adaptation/recovery edits were archived under `build/pre-v2-legacy-edits` and removed on `refactor/backend-v2`.
5. Keep implementation, automated validation and real-machine validation separately recorded.
6. Check a task only when its stated work is complete. Missing hardware/network access means a test remains unchecked.
7. Record commands, outcomes, artifact paths and limitations in the evidence log. Update the plan if measured evidence requires changing a tuning default.
8. Keep this checklist and `agents/todo.md` synchronized once implementation begins.

Current next action: **Checkpoint A — production capture-owner/session integration, external gaming latency baseline and installer/distribution evidence.** Scoped application COM lifetime now passes 100 full hardware/recovery cycles and two 100-cycle rapid-close variants within the handle bound. This resolves the reproduced linear event growth, not every resource/lifecycle gate. The GraphicsCapture.dll pin remains. See [CHECKPOINT-A.md](CHECKPOINT-A.md) and [BUILD.md](BUILD.md).

## Planning handoff

- [x] Save the complete approved implementation plan in this folder.
- [x] Save the ordered implementation, testing, acceptance and deferred-work checklist.
- [x] Synchronize the project roadmap with this refactor when implementation begins.

## Checkpoint A — baseline and build proof

Plan references: Sections 1, 4.3 and 5 / Checkpoint A.

### Baseline and repository grounding

- [x] Review applicable instructions, repository memory and current implementation.
- [x] Inspect/preserve the existing CMake/runtime edits and untracked adaptation/recovery modules/tests.
- [x] Record the starting commit and relevant dirty-tree changes with the baseline.
- [x] Record the single-machine proof host/viewer hardware, OS, GPU/driver, generated source, resolution/FPS/bitrate and local network conditions; external LAN/game reference remains separate.
- [ ] Capture current-backend one-viewer behavior, queue/latency/CPU/GPU data and available multi-viewer results.
- [x] Record existing validation failures separately from refactor regressions.
- [x] Write the v2 architecture/protocol reference and shared message fixtures under the repository's documentation/test structure.
- [x] Add the room wire/ownership reference, matching native/Worker client-command validators and shared malformed-message/Unicode/byte-boundary fixtures.
- [x] Validate independent subscription revisions, one resync per gap and stale-callback rejection with a shared native/TypeScript lifecycle trace.
- [x] Complete exact server snapshot/delta/acknowledgement schemas and shared outbound fixtures before wiring the room transport.
- [x] Add atomic native room/directory state caches with bound identities, generation checks, full-state validation and one resync for inconsistent deltas.

### Reproducible native dependencies

- [x] Resolve one official WebRTC commit and record source/toolchain/dependency revisions.
- [x] Verify required APIs and GN flags against that pinned source.
- [x] Record architecture, debug/release GN arguments, compiler, CRT, standard-library, RTTI and exception settings.
- [x] Produce matching debug/release WebRTC artifacts.
- [x] Add CMake/Ninja clang-cl presets using MSVC-compatible Qt, including Network and WebSockets.
- [x] Add the imported WebRTC target and verified artifact cache identity, including relocatable headers, generated headers, compiler builtins, and full file inventory.
- [x] Support an explicit local artifact directory for offline development.
- [x] Separate application builds from dependency builds; reject mismatched artifacts clearly.
- [x] Build the UI and CLI with the new toolchain.
- [ ] Verify dependency notices and portable/installer staging requirements.
- [x] Generate WebRTC dependency notices including FFmpeg/OpenH264/compiler-rt and bundle them with the SDK and native portable package.
- [x] Verify native Release portable CLI, UI self-test and Windows GUI startup from an extracted zip with developer paths removed; confirm package-local Qt plugins.
- [ ] Complete remaining distribution obligations/notices (including Qt), installer staging and a fresh-machine run before release.

### Integration proof

- [x] Establish two local PeerConnections using H.264, Opus and encrypted data channels.
- [x] Prove Media Foundation software encoder/decoder adapter integration with the pinned factories.
- [x] Prove software encoder rate updates, zero-rate suspension, keyframe requests, reset and release.
- [x] Prove software encoder one-pending-frame replacement under a deterministic 100-input burst.
- [x] Extend these codec proofs to hardware, including rate/keyframe health and fallback.
- [x] Prove bounded asynchronous encoder input and missing-output stall detection/cancellation (not preemption of hung driver calls).
- [x] Prove owned synthetic GPU frame lifetime, serialized readback and measurable CPU fallback.
- [x] Implement owned live capture snapshots, producer GPU completion and hardware encoder import; validate fixed-output resize and retained pixels.
- [x] Correct queued-frame draining and session-before-pool shutdown; verify the reproduced WGC closure regression.
- [x] Validate external source-process exit, minimize/restore and permanent closure with replacement windows.
- [ ] Validate forced HWND reuse and device-loss recovery.
- [x] Add automatic proof capture recovery with a three-rebuild budget, cancellable backoff and per-device retirement across replacement generations; verify injected losses and terminal exhaustion.
- [x] Reproduce and diagnose the intermittent Release access violation in unloaded GraphicsCapture.dll; add a process-lifetime system-module pin and verify 100 full Release cycles.
- [x] Mitigate rapid source-close StopCapture hang with capture-owner dispatch; complete 100 rapid-close cycles and retain the watchdog regression. Resource acceptance remains separate.
- [x] Diagnose reproduced hardware/recovery handle growth and pass the 100-cycle handle bound using adapter ownership, capture dispatch and scoped application COM lifetime. Full Release median handles 380 → 379; complete resource teardown remains a separate gate.
- [x] Prove original-window WGC device reconstruction, shared encoder-device retirement and fresh-device software IDR recovery without reading retired textures (explicit reconstruction/invalidation; automatic session recovery and real driver removal remain pending).
- [x] Add bounded receiver presentation recovery and verify policy plus GPU resource recreation with injected device-loss HRESULTs (actual driver removal and capture/encoder recovery remain untested).
- [x] Prove owned live WGC capture through hardware H.264 PeerConnections alongside Opus and data channels.
- [x] Prove GPU presentation of CPU-decoded NV12, one pending frame, fixed aspect ratio and receiver-window resize (GPU decode remains pending).
- [x] Prove WASAPI-to-WebRTC PCM capture/playout through the Audio Device Module (process capture through Opus; physical playout tested separately).
- [ ] Verify clean teardown without retained callbacks, textures, sockets or audio devices.
- [x] Record integration limitations and exact source/API findings.

**Gate A**

- [ ] Native codec/audio integration and reproducible builds pass. Evidence is recorded; wider migration may begin.

## Checkpoint B — native media engine

Plan references: Sections 2.1–2.6 and 5 / Checkpoint B.

### Interfaces, ownership and lifecycle

- [ ] Implement the separated session, media, capture/audio, presentation, input and diagnostic interfaces.
- [ ] Keep Windows/WebRTC types out of portable public headers.
- [ ] Add profile, room policy, stream preferences, viewer status, operation result, session snapshot and owned frame types.
- [ ] Implement the serialized control executor and session/viewer generations.
- [ ] Implement state transitions, idempotent stop, cancellation and complete thread/resource shutdown.
- [ ] Prevent callbacks from old generations and UI/worker shutdown deadlocks.
- [ ] Preserve update-only settings without resetting unrelated fields.
- [ ] Add a synthetic video/audio diagnostic harness using the same backend.

### PeerConnections

- [ ] Implement one host-to-viewer PeerConnection, separate track/source adaptation wrapper and encoder per viewer.
- [ ] Share source capture/audio without sharing viewer adaptation restrictions.
- [ ] Use host-offerer Unified Plan, one-way audio/video and bundled transport/data.
- [ ] Implement direct ICE/STUN, trickled candidates and bounded pre-description candidate queues.
- [ ] Scope SDP/ICE to connection generations; discard stale messages.
- [ ] Implement the 20-second connection deadline and bounded ICE restart attempts.
- [ ] Keep viewer failure/recovery isolated from other viewers.

### Windows video and presentation

- [ ] Adapt WGC and appropriate display-only DXGI fallback.
- [ ] Preserve selected-source privacy, cursor behavior, HDR-to-SDR and source identity.
- [ ] Handle source closure/minimization/resize and D3D device loss explicitly.
- [ ] Implement per-viewer GPU scaling and owned GPU frames.
- [ ] Implement encoder worker/event handling and one pending raw-frame slot.
- [ ] Honor WebRTC rates, keyframes and timestamps without custom congestion logic.
- [ ] Probe hardware health; quarantine failed implementations and fall back per viewer.
- [ ] Preserve fixed-resolution semantics when fallback cannot sustain the configuration.
- [ ] Adapt decoding and GPU presentation, with reported CPU fallback.
- [ ] Preserve coded dimensions, visible aperture and aspect ratio.
- [ ] Keep one replaceable pending presentation frame; remove old A/V gating from the new path.

### Audio

- [ ] Implement 48 kHz / 10 ms PCM exchange and consistent multichannel downmix.
- [ ] Preserve system, microphone, selected device and process-loopback modes.
- [ ] Use event-driven WASAPI and bounded handoffs with measured device buffering.
- [ ] Isolate microphone processing from system/process audio.
- [ ] Preserve mute/volume, video-only operation and explicit device-error behavior.
- [ ] Use monotonic local clocks and WebRTC synchronization.

### Stream settings

- [ ] Implement explicit Auto/Manual modes plus native-size resolution behavior.
- [ ] Implement fresh-profile Gaming / Auto resolution / 1080p maximum / 60 FPS target / Auto bitrate defaults.
- [ ] Implement the documented Auto maximum calculation and conservative startup rate.
- [ ] Map all resolution/FPS mode combinations to the intended WebRTC adaptation preference.
- [ ] Keep manual resolution fixed and manual bitrate subject to congestion control.
- [ ] Fit/letterbox fixed dimensions and expose the active image rectangle for input mapping.
- [ ] Preserve manual settings when switching presets.
- [ ] Add settings revisions, prevalidation and per-viewer pending/applied/error state.
- [ ] Retain/recover working settings on failed reconfiguration; report partial application honestly.
- [ ] Implement optional aggregate media budgeting with overhead/audio reserve and equal per-viewer allocation.
- [ ] Show actual wire usage separately; do not add a second bandwidth-control loop.

### Automated validation

- [ ] Verify manual resolution under congestion and manual bitrate under WebRTC rate reduction.
- [ ] Verify one viewer's adaptation cannot alter another viewer's restrictions.
- [ ] Verify encoder failure and hardware-to-software fallback isolation.
- [ ] Verify bounded queues with slow encoding/rendering.
- [ ] Verify frame ownership, visible dimensions, aspect ratios and source changes.
- [ ] Verify silence/missing audio cannot deadlock video.
- [ ] Verify cancellation, repeated stop and rejection of old callbacks.

**Gate B**

- [ ] Native engine behavior and focused tests pass with evidence; no custom competing bitrate/playout loop remains in the v2 path.

## Checkpoint C — room service v2

Plan references: Section 3 and 5 / Checkpoint C.

### Protocol and native client

- [ ] Implement the six documented v2 HTTP/WebSocket endpoints.
- [ ] Implement create/join credentials, provisional membership expiry and publish-after-host-attach.
- [ ] Use one Qt-backed native networking event loop for UI and CLI.
- [ ] Implement all documented socket commands and typed errors.
- [ ] Implement authoritative sender identity, targeted signaling and generation checks.
- [ ] Implement separate room/directory revisions; exclude directed signaling from state revisions.
- [ ] Implement duplicate suppression, one-shot gap resync, expected-revision conflicts and reconnect snapshots.
- [ ] Enforce message-byte limits, candidate limits and bounded queues.
- [ ] Add shared client/server protocol fixture tests.

### Persistence, hibernation and directory

- [ ] Add separate SQLite Durable Object room/directory namespaces for v2.
- [ ] Implement socket attachments and replacement-generation handling.
- [ ] Implement automatic ping/pong and auto-response timestamp liveness.
- [ ] Implement documented client ping/pong deadlines and jittered reconnect backoff.
- [ ] Implement provisional/membership expiry and immediate leave/kick.
- [ ] Handle host reconnecting/expiry without host election or new joins during disconnection.
- [ ] Implement safe directory summaries and snapshot/delta subscriptions.
- [ ] Remove per-room listing verification and automatic HTTP polling from v2.
- [ ] Implement 60-second lease renewals, 180-second expiry and directory cleanup.
- [ ] Avoid visible updates for lease-only renewals.
- [ ] Persist pending directory updates; retry failures and closure removal safely.
- [ ] Enforce summary versions and fail-closed reservations/admission.

### Profile and authorization

- [ ] Persist local nickname/preferences; use random Guest defaults rather than OS identity.
- [ ] Validate nickname normalization, code points/UTF-8 size and forbidden controls.
- [ ] Implement duplicate-name disambiguation with peer IDs.
- [ ] Implement public/unlisted rooms, creation password and live name/visibility/viewer-limit edits.
- [ ] Preserve existing viewers when the limit is lowered; block new admissions.
- [ ] Implement v2 room links without secrets.
- [ ] Issue 256-bit membership tokens, store hashes and enforce role/target/socket authorization.
- [ ] Preserve versioned salted PBKDF2 work factor and HTTPS-only secret handling.
- [ ] Add admission and per-socket rate limits without cross-object checks for every message.
- [ ] Preserve configurable room/participant caps, CORS restrictions and certificate validation.
- [ ] Implement kick invalidation and avoid claims of permanent accountless bans.

### Worker/native integration tests

- [ ] Add Worker typecheck/test scripts and execute tests in the local Cloudflare runtime.
- [ ] Test simultaneous joins, provisional expiry and capacity races.
- [ ] Test passwords, invalid/expired tokens, replay and unauthorized commands.
- [ ] Test old-socket close after replacement and hibernation heartbeat freshness.
- [ ] Test duplicate/gapped revisions and stale connection candidates.
- [ ] Test directory write/removal failures, retries and leases.
- [ ] Test host crash/leave, public/unlisted visibility and viewer-limit changes.
- [ ] Test malformed/oversized messages, Unicode handling and signaling floods.
- [ ] Verify listing has zero per-room fanout and heartbeat has zero per-peer storage writes.

**Gate C**

- [ ] Real native-client/local-Worker integration and security/lifecycle tests pass with evidence.

## Checkpoint D — UI and gaming controls

Plan references: Sections 2.7, 4.1–4.2 and 5 / Checkpoint D.

### Product integration

- [ ] Route normal UI/CLI sessions and room operations through the new shared interfaces.
- [ ] Add Auto/Manual controls, Gaming/Quality presets and honest apply status.
- [ ] Add local nickname settings and server-pushed room browsing.
- [ ] Close directory subscriptions when hidden and mark disconnected listings stale.
- [ ] Add adjustable viewer capacity with warning above four.
- [ ] Add per-viewer summary rows and details popup with requested/applied/actual values.
- [ ] Refresh statistics once per second and mark receiver data stale after three seconds.
- [ ] Keep receiver telemetry on the peer data channel, not Cloudflare.
- [ ] Add typed limiting reasons and unknown-valued metrics.
- [ ] Preserve fullscreen/preview interaction, report paths and signed updater behavior.
- [ ] Redact secrets/SDP/addresses and reset rate baselines across generations.

### Input channels and Windows backends

- [ ] Implement reliable ordered control, unreliable unordered input state and replaceable telemetry channels.
- [ ] Implement explicit versioned input serialization and validation.
- [ ] Include connection/permission generations and sequence checks.
- [ ] Implement pointer coalescing, gamepad polling and 100 ms state keepalives.
- [ ] Bound reliable queues and handle backpressure without stale input replay.
- [ ] Preserve exclusive mouse/keyboard ownership and up to three remote gamepads with local-slot reservation.
- [ ] Implement 300 ms watchdog neutralization and fresh-generation recovery.
- [ ] Preserve consent, persistent indicators, panic revoke and window confinement.
- [ ] Neutralize input on source change and rebuild active-image coordinate mapping.
- [ ] Preserve XInput/PlayStation reports and installer-only virtual-driver lifecycle.

### UI/input validation

- [ ] Verify accurate pending/applied/error states and per-viewer metrics.
- [ ] Verify no input backlog under video saturation, retransmissions or keyframe bursts.
- [ ] Verify lost/late/reordered states and reliable events after watchdog expiry.
- [ ] Verify revoke, disconnect, controller unplug and backend failure neutralization.
- [ ] Verify window focus/confinement, letterboxing and source-change mapping.
- [ ] Verify independent remote pads and preserved local slots.
- [ ] Verify missing driver disables only unavailable gamepad functionality.

**Gate D**

- [ ] UI/CLI integration and gaming/control regression tests pass with evidence.

## Checkpoint E — performance and field validation

Plan references: Section 5 / Checkpoint E and Free-tier validation.

### Harness and impairment scenarios

- [ ] Extend the multi-viewer harness for the new CLI and four-viewer runs.
- [ ] Add seeded WebRTC network-simulation scenarios and record configurations/seeds.
- [ ] Run healthy 1080p60 with one viewer and four viewers.
- [ ] Run one-viewer-path bandwidth collapse/recovery: 20 → 4 → 20 Mbps.
- [ ] Run 2% and 5% loss with up to 50 ms jitter.
- [ ] Run reordering/duplication without injected loss.
- [ ] Run slow decode/presentation on one viewer.
- [ ] Run host encoder exhaustion/hardware failure.
- [ ] Run late join, kick, leave/rejoin and host restart.
- [ ] Run interface change, ICE restart and blocked direct UDP.
- [ ] Run input state loss, delayed reliable events and revoke during congestion.

### Real-machine acceptance

- [ ] Test actual two-machine LAN and Internet sessions; record host/viewer hardware and drivers.
- [ ] Measure capture-to-display externally and input-to-visible-response with a deterministic host scene.
- [ ] Record p50/p95/p99, sample counts and measurement method.
- [ ] Meet Gaming p95 capture-to-display < 80 ms on the reference healthy LAN at 1080p60.
- [ ] Meet Gaming p95 input-to-visible-response < 120 ms under the same conditions.
- [ ] Meet Quality p95 capture-to-display < 250 ms.
- [ ] Verify steady A/V skew within ±50 ms.
- [ ] Verify stale frame age stops growing after capacity reduction; target settling within three seconds at sustainable settings.
- [ ] Verify upward adaptation resumes without repeated resolution/encoder oscillation.
- [ ] Verify one impaired viewer does not lower healthy viewers without a documented shared resource/budget constraint.
- [ ] Complete a two-hour four-viewer soak without deadlock, sustained memory growth or accumulating queues.
- [ ] Complete 100 start/stop cycles without retained sessions, sockets, devices or callbacks.
- [ ] Verify healthy room/list changes appear within two seconds.
- [ ] Verify zero periodic HTTP membership/list polling.
- [ ] Verify zero per-room listing verification and zero per-peer heartbeat writes.

### Free-tier workload

- [ ] Model/test ten rooms × one host/four viewers × eight hours, plus ten directory subscribers.
- [ ] Count Worker HTTP requests/upgrades.
- [ ] Count Durable Object messages, cross-object calls and alarms.
- [ ] Count storage reads/writes, active duration and directory broadcasts.
- [ ] Verify at least 50% headroom against each applicable measured daily allowance.
- [ ] Record account-wide usage caveats and current Cloudflare limits/source date.

**Gate E**

- [ ] All automated and available real-machine requirements pass; unresolved manual checks are explicitly listed rather than marked passed.
- [ ] Final acceptance evidence supports completion. Any unmet required criterion remains an open task.

## Cutover preparation and post-cutover cleanup

Production deployment/release publishing is separate from implementation. Keep preparation and actual external cutover clearly distinguished.

- [ ] Build and validate a prerelease against separate v2 namespaces.
- [ ] Prepare the coordinated client/server upgrade procedure and old-client upgrade-required response.
- [ ] Confirm updater compatibility; v1 active rooms/keys are not migrated.
- [ ] Record the production cutover as a separate pending release/deployment step.
- [ ] After replacement validation and coordinated cutover, retire unused custom UDP/crypto/adaptation/polling/NAT-invite/runtime-control paths.
- [ ] Remove unused KV fallbacks and obsolete tests only after equivalent replacement behavior is verified.
- [ ] Preserve useful platform, security, controller and updater code/tests.
- [ ] Update README, build/usage instructions, dependency notices and stale repository memory.
- [ ] Publish a final validation summary with exact results and outstanding hardware/network limitations.

## Deferred backlog — outside this refactor

- [ ] Host approval queue for newcomers.
- [ ] Accounts and profiles across devices.
- [ ] Additional personalization.
- [ ] Actual Linux/macOS ports.
- [ ] Encoding reuse, simulcast or SVC optimization for larger rooms.
- [ ] Maintained replacement for the retired virtual-controller runtime.
- [ ] Live room-password rotation.
- [ ] Additional codecs after H.264/Opus performance is established.
- [ ] Optional relay support only if the direct-only/free requirement changes.

## Evidence log

Do not put passwords, membership tokens, SDP, ICE credentials or peer addresses here.

| Checkpoint | Date | Commit / working-tree state | Command or test method | Result and artifact path | Remaining limitations |
|---|---|---|---|---|---|
| Planning | 2026-09-14 | Documentation only; pre-existing code edits preserved | Save approved plan and TODO | Handoff saved; implementation not started | All implementation/validation checkpoints pending |
| A / inventory | 2026-09-14 | e8c82ae plus preserved pre-existing edits and baseline tooling | Build CLI/policy tests; collect-backend-baseline.ps1 -RunTests; official source/API audit | Build passed; 10/10 CTests passed; hardware and hashes in build/baseline/20260914-192244-855; see CHECKPOINT-A.md | No streaming latency measurements or WebRTC artifacts/integration yet |
| A / native foundation | 2026-09-14 | refactor/backend-v2; user-authorized legacy edits archived/removed | Debug/Release WebRTC and application builds, direct data-channel and MF probes, restored-backend loopback | Both application configurations 8/8; both probe configurations 2/2; two hardware codec cycles pass; BUILD.md and CHECKPOINT-A.md link logs | Actual MF WebRTC adapters, audio ADM, artifact cache/notices and full performance validation remain |
| A / software MF integration | 2026-09-14 | refactor/backend-v2; uncommitted adapters and proof expansion | MF H.264 through PeerConnections, codec lifecycle/rate/burst tests, native application regressions | Debug/Release proof suites 4/4 each; application suites 8/8 each; CHECKPOINT-A.md links evidence | Hardware/GPU ownership, WASAPI/Opus transmission, artifact delivery and performance gates remain |
| A / hardware and GPU ownership | 2026-09-14 | refactor/backend-v2; uncommitted hardware/GPU adapters and tests | Hardware PeerConnections, missing-output injection, quarantine/cancellation, GPU lifetime/readback and hardware burst/lifecycle tests | Hardware-enabled Debug/Release suites 8/8 each; application suites 8/8 each; CHECKPOINT-A.md links evidence | WASAPI/Opus, live capture/GPU presentation, driver-hang preemption, artifact delivery and performance evidence remain |
| A / PCM ADM and Opus | 2026-09-14 | refactor/backend-v2; uncommitted audio endpoints/ADM and tests | Debug/Release -Hardware -AudioDevice; native application regressions | Proof suites 11/11 each; application suites 8/8 each; process-loopback through Opus and separate physical playout pass; CHECKPOINT-A.md links logs | Live capture/presentation, broader audio modes/recovery, artifact delivery and performance remain |
| A / owned live capture | 2026-09-14 | refactor/backend-v2; uncommitted capture ownership/import proof | WGC generated-window hardware encode, resize, retained-pixel comparison and repeated closure | Individual Debug/Release live runs passed; repeated runs exposed intermittent WGC shutdown timeout; ordinary proof 11/11 and app 8/8 each | Capture shutdown remains unresolved; do not count live test or Gate A as passed; CHECKPOINT-A.md records stacks/logs |
| A / capture lifecycle and live peers | 2026-09-14 | refactor/backend-v2; uncommitted lifecycle corrections and live source proof | Queue draining/close order, WinRT cache lifetime, repeated WGC and actual PeerConnection video | Six shutdown runs and three same-process cycles per configuration pass; live peers receive 60 frames with zero sender readbacks; ordinary suites 11/11 and app 8/8 each | GPU presentation, external-source/device recovery, delivery and latency gates remain |
| A / receive GPU presentation | 2026-09-14 | refactor/backend-v2; uncommitted owned NV12/sink/presenter | 500-frame burst, cached fallback, live WebRTC receiver resize and visible center/bar checks | Debug/Release media suites 12/12, app suites 8/8; live GPU submissions 55/56 with no conversion/repack and one-frame DXGI limit | CPU decoding/upload remains; device recovery, external lifecycle, delivery and measured latency remain open |
| A / source lifecycle | 2026-09-14 | refactor/backend-v2; uncommitted closure/state guards and process proof | Minimize/restore, permanent closure, job-contained process exit and live video regressions | Debug/Release media suites 12/12 and app suites 8/8; three source lifecycle and three child-process exits each; live presentation passes | Forced handle reuse, device recovery and remaining Gate A evidence pending |

## Open blockers and handoff notes

Gate A remains open. Software/hardware MF H.264, bounded submission, missing-output fallback/quarantine and synthetic GPU lifetime tests pass. Live capture/GPU presentation, broader audio mode/recovery validation, immutable artifact cache/notices and broader performance evidence remain unfinished. See CHECKPOINT-A.md for exact resume work.

At each checkpoint handoff, record:

- Completed tasks and evidence.
- Current working-tree changes.
- Failed or unrun tests and why.
- Any measured tuning changes and corresponding plan updates.
- The exact next unfinished task.

### Latest lifecycle continuation — 2026-09-15

- [x] Select hardware MFTs on the actual input adapter and retain activation shutdown ownership.
- [x] Add optional handle-growth acceptance and isolated encoder/device-rebuild probes.
- [x] Resolve reproduced combined capture/encoder linear handle growth; final 100-cycle full Release run passes the handle bound.
- [x] Mitigate rapid source-close StopCapture hang with dispatcher integration; earlier close-order/state-release alternatives failed and were reverted.

See the latest CHECKPOINT-A.md section for the 100-cycle result and exact artifacts.

### Dispatcher milestone — 2026-09-15

- [x] Trace retained events to capture-item/COM creation; distinguish CPU-memory hardware tests from owned-GPU-input tests.
- [x] Add capture-owner dispatcher creation, message delivery and owned-queue shutdown; preserve caller queues and WM_QUIT.
- [x] Complete 100 rapid source-close cycles without the prior StopCapture hang on the reference machine.
- [x] Extend automated coverage for dispatcher lifecycle and hardware GPU input with FPS restarts.
- [x] Resolve residual source-close COM event growth with scoped application MTA ownership; 100 rapid-close cycles and 100 fresh-owner-thread cycles both show zero handle growth.
- [ ] Preserve native-call watchdog coverage and validate production capture-owner/session integration before claiming Gate A complete.
