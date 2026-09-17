# Backend v2 — detailed checks and historical evidence

Last reconciled: 2026-09-17 (through `6a69998`, plus sender/network details integration). Implementation status: **Gate A passed for native integration/build proof; Checkpoint B in progress**.

Specification: [PLAN.md](PLAN.md). The plan is authoritative; this checklist tracks execution and evidence.

Checked implementation rows below apply to the shared v2 backend and opt-in
frontends, not default-shell cutover or field acceptance. Mixed requirements are
split into completed implementation and remaining validation. All original
physical-device, resource, network, cost, latency and cutover gates remain open
until their own evidence passes. Dated continuation notes at the end are historical;
the reconciled checkpoint rows and [TODO.md](TODO.md) define current work.

## Shared application shell integration — 2026-09-17

- [x] Reuse AppShell for both existing v2 UI entry points; browser and session pages
  stay inside one top-level window and release old pages when returning.
- [x] Defer shell close until media/directory drain, disable actions during close,
  and preserve responsive cancellation during admission and active streaming.
- [x] Reuse screen-awake lifecycle and reserve the revoke hotkey only with a handler.
- [ ] Migrate ordinary home/create/join/control actions and existing CLI commands
  after feature parity; this shell integration does not close default adoption.

## Latest integration: shared UI/CLI presentation — 2026-09-17

- [x] Expose local presentation outcomes and separate busy, occluded, minimized,
  unavailable and recovery-backoff drop counters through the shared renderer,
  thread-safe Qt snapshot, viewer diagnostics and periodic/final CLI JSON.
- [x] Retain typed graphics failure codes through recovery/terminal state without
  logging raw exception text; unknown exceptions use generic E_FAIL.
- [x] Improve Windows timeout evidence with caller stage, counters, error code,
  outcome and window visibility. A later pass does not establish the cause of
  the previously recorded timeout or prove physical driver recovery.

- [x] Move presentation ownership/recovery into backend/render; both frontends use
  the same native renderer, typed errors, three-rebuild budget and backoff.
- [x] Remove the duplicate CLI D3D/shader/texture/swap-chain/viewport implementation.
- [x] Preserve preview sizing, fit/1:1, fullscreen, minimize/restore, audio callbacks
  and legacy/retained frame entry points; avoid extra frame copies/queues.
- [x] Add CLI terminal title/JSON diagnostics, bounded error reporting, callback
  exception containment, resource release before HWND destruction and local close.
- [x] Verify actual UI/CLI GPU resource recreation and lifecycle under injected
  failures, including resize failures and two-window close isolation.
- [ ] Complete physical driver loss/hangs, hardware decode/GPU zero-copy and
  external image/input latency acceptance; correctness tests do not close them.

## Bounded UI presentation recovery — 2026-09-16

- [x] Adopt the existing recovery policy in the actual UI worker, including attach,
  resize and presentation errors; three lifetime rebuilds and 250 ms drop-only backoff.
- [x] Stop nonrecoverable/exhausted renderer calls and surface a rejoin instruction.
- [x] Verify one-slot ownership under 1,000-frame pressure, owner-thread disposal,
  successful-frame budget persistence, explicit clear and shutdown during backoff.
- [x] Exercise injected loss with actual GPU resource recreation in Windows UI tests.
- [ ] Validate physical driver removal/hangs and capture/encoder recovery acceptance.

## Retained presentation — 2026-09-16

- [x] Retain packed decoded NV12 through the actual UI/CLI without a planar round
  trip or second UI pixel copy; explicitly handle padded NV12 and other formats.
- [x] Preserve immutable frame ownership through overwrite, consumption and stop;
  test pointer identity, plane values, fallback, rotation and late callbacks.
- [x] Enable and measure one-frame DXGI queue configuration in v2 UI/CLI, use
  nonblocking present, and count busy/occluded frames as drops.
- [x] Verify the retained path with real media and generated-window Windows
  renderers; expose conversion/repack and presentation counters for comparisons.
- [ ] Complete hardware decoding/GPU zero-copy and physical/remote latency
  acceptance. CPU handoff improvements do not satisfy these broader gates.

Evidence: [CHECKPOINT-B.md](CHECKPOINT-B.md). Milestone 2 remains open.

## Latest integration: viewer playback — 2026-09-16

- [x] Integrate viewer output-device selection, volume and mute through public
  session, native Windows adapter, actual Qt widgets and timed CLI configuration.
- [x] Preserve old output/settings on replacement startup or first-write failure;
  bound pending work, cancel on stop and retain settings across playout restart.
- [x] Keep output ownership on the existing worker without extra queues/clocks;
  refresh actual device buffering and engine-period diagnostics after changes.
- [x] Validate gain, mute, rollback, Busy, cancellation, restart, owner destruction,
  actual-widget video continuity and CLI configuration/results with silent output.
- [ ] Accept physical device unplug/recovery, driver-hang handling and measured
  end-to-end latency. Synthetic endpoints do not establish these properties.

Evidence: [CHECKPOINT-B.md](CHECKPOINT-B.md). Milestone 2 is still open.

## Latest integration: live shared-audio selection — 2026-09-16

- [x] Integrate bounded host audio selection through public session, Windows native
  capture, actual Qt controls and timed CLI config, without membership requests.
- [x] Preserve a healthy old source on startup/first-PCM failure, retain successful
  selection across recording restart, and cancel pending operations on stop.
- [x] Keep application capture buffering at 30ms across WASAPI and the new handoff;
  support cancellation of process-loopback activation and safely own its event.
- [x] Verify decoded silence/resume with four viewers and actual widgets, continuing
  video, failed selection, Busy, timeout, shutdown and owner-thread destruction.
- [x] Implement live playback-device selection through session/UI/CLI; see viewer playback above.
- [ ] Validate physical audio-device switches/unplug/recovery. Synthetic proofs do not establish physical latency.

Evidence and timings: [CHECKPOINT-B.md](CHECKPOINT-B.md). Remaining grouped work is
tracked in [TODO.md](TODO.md). The checkpoint rows below are reconciled requirements;
dated continuation notes preserve historical evidence rather than defining new batch goals.

## Start/resume instructions

1. Read the complete plan before changing implementation.
2. Read applicable repository instructions and inspect the current working-tree diff.
3. Use the delivery milestones in TODO.md to select the work batch. The individual checks here are acceptance details, not turn-sized goals or stopping points. Preserve the original checkpoint gates.
4. Preserve unrelated changes. On 2026-09-14 the user authorized removing unnecessary legacy edits; those UDP adaptation/recovery edits were archived under `build/pre-v2-legacy-edits` and removed on `refactor/backend-v2`.
5. Keep implementation, automated validation and real-machine validation separately recorded.
6. Check a task only when its stated work is complete. Missing hardware/network access means a test remains unchecked.
7. Record commands, outcomes, artifact paths and limitations in the evidence log. Update the plan if measured evidence requires changing a tuning default.
8. Keep this checklist and `agents/todo.md` synchronized once implementation begins.

Current next action: **Finish the remaining grouped adoption/input and acceptance milestones in TODO.md.** Public RoomSession, NativeRoomRuntime, automatic dispatch, asynchronous capture drain, recovery ownership and opt-in UI/CLI integration are implemented. Remaining work includes gaming input/consent, hardware decode/GPU zero-copy, default-shell adoption and physical-device/resource/remote acceptance. The +76/+10 capture-handle regressions remain blockers before cutover. See [CHECKPOINT-B.md](CHECKPOINT-B.md), [CLOSEOUT-A.md](CLOSEOUT-A.md) and [COMPARISON.md](COMPARISON.md).

## Planning handoff

- [x] Shared RoomSessionCoordinator drives network events, bounded asynchronous
  send completion and media hooks on signaling, without a diagnostic event pump.
  Shared RoomSignalCodec replaces diagnostic-only SDP/ICE conversion; restart
  finishes while the caller makes no commands/status queries for five seconds.
- [x] Extract peer-factory/session composition into NativeRoomRuntime and adopt public RoomSession commands/status in opt-in UI/CLI. Default-shell adoption remains under D.

- [x] Owned room networking with bounded cross-thread queues, cancellation and
  no caller Qt event pump; validated by RoomNetworkTest and real four-viewer media.
- [x] Shared roster reconciliation drives peer creation/removal in the real-room
  proof, including socket loss/reconnect, kick/rejoin and room closure.
- [x] Integrate owners into scheduled RoomSession dispatch, asynchronous capture cleanup, shared recovery budgets and public command/status reporting.

- [x] RoomManagedPeer integrates capture-attachment futures, scheduled recovery
  and asynchronous retirement in the real-room scenario. HostPeerOwner.BeginStop
  keeps signaling responsive while capture drains; a held-callback test validates
  retention and repeated completion. Public facade adoption is now implemented; default-shell adoption remains open.

- [x] Add one-command production room/UI/CLI regression with repeated paced media, machine-readable evidence, bounded logs and process-tree watchdog cleanup. See HEADLESS-TESTING.md; generated WGC/GPU windows remain an explicit desktop option.
- [ ] Extend those scenarios with authorized test-owned gaming input, network impairment and separate host/viewer processes.

Resumed at user request on 2026-09-15. See [HEADLESS-TESTING.md](HEADLESS-TESTING.md) for the first runnable headless smoke/regression commands.

- [x] Save the complete approved implementation plan in this folder.
- [x] Save the ordered implementation, testing, acceptance and deferred-work checklist.
- [x] Synchronize the project roadmap with this refactor when implementation begins.

## Checkpoint A — baseline and build proof

Plan references: Sections 1, 4.3 and 5 / Checkpoint A.

The original gate is native codec/audio integration and reproducible builds. Full production teardown, hardware/latency acceptance and release distribution checks remain explicitly open under B, E and cutover below; see [CLOSEOUT-A.md](CLOSEOUT-A.md).

### Baseline and repository grounding

- [x] Review applicable instructions, repository memory and current implementation.
- [x] Inspect/preserve the existing CMake/runtime edits and untracked adaptation/recovery modules/tests.
- [x] Record the starting commit and relevant dirty-tree changes with the baseline.
- [x] Record the single-machine proof host/viewer hardware, OS, GPU/driver, generated source, resolution/FPS/bitrate and local network conditions; external LAN/game reference remains separate.
- [x] Record available legacy one-viewer baseline: settings, CPU time, capture/encode timing and queue behavior; explicitly retain missing GPU/external-latency/multi-viewer measurements in Checkpoint E.
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
- [x] Audit native dependency notices and portable/installer staging requirements; verify extracted package launch. Outstanding release obligations remain below.
- [x] Generate WebRTC dependency notices including FFmpeg/OpenH264/compiler-rt and bundle them with the SDK and native portable package.
- [x] Verify native Release portable CLI, UI self-test and Windows GUI startup from an extracted zip with developer paths removed; confirm package-local Qt plugins.

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
- [x] Add automatic proof capture recovery with a three-rebuild budget, cancellable backoff and per-device retirement across replacement generations; verify injected losses and terminal exhaustion.
- [x] Reproduce and diagnose the intermittent Release access violation in unloaded GraphicsCapture.dll; add a process-lifetime system-module pin and verify 100 full Release cycles.
- [x] Mitigate rapid source-close StopCapture hang with capture-owner dispatch; complete 100 rapid-close cycles and retain the watchdog regression. Resource acceptance remains separate.
- [x] Diagnose reproduced hardware/recovery handle growth and pass the 100-cycle handle bound using adapter ownership, capture dispatch and scoped application COM lifetime. Full Release median handles 380 → 379; complete resource teardown remains a separate gate.
- [x] Prove original-window WGC device reconstruction, shared encoder-device retirement and fresh-device software IDR recovery without reading retired textures (explicit reconstruction/invalidation; automatic session recovery and real driver removal remain pending).
- [x] Add bounded receiver presentation recovery and verify policy plus GPU resource recreation with injected device-loss HRESULTs (actual driver removal and capture/encoder recovery remain untested).
- [x] Prove owned live WGC capture through hardware H.264 PeerConnections alongside Opus and data channels.
- [x] Prove GPU presentation of CPU-decoded NV12, one pending frame, fixed aspect ratio and receiver-window resize (GPU decode remains pending).
- [x] Prove WASAPI-to-WebRTC PCM capture/playout through the Audio Device Module (process capture through Opus; physical playout tested separately).
- [x] Record integration limitations and exact source/API findings.

**Gate A**

- [x] Native codec/audio integration and reproducible builds pass. Evidence and scope reconciliation are recorded in CLOSEOUT-A.md; wider implementation may begin. Full resource acceptance is not passed.

## Checkpoint B — native media engine

Plan references: Sections 2.1–2.6 and 5 / Checkpoint B.

### Interfaces, ownership and lifecycle

- [x] Scope capture attachment/removal and failure snapshots to the viewer connection generation; reject retired attachments and delayed removals after same-viewer rejoin.

- [x] Add a portable owning peer registry with restart/close dispatch, terminal failure snapshots, bounded membership and generation validation; drive actual four-peer ownership through it.
- [x] Bind peer failure/removal to asynchronous capture cleanup, retry queue pressure, retain the connection identity until cleanup completes, and join capture before full peer-owner shutdown.
- [x] Connect the bound peer owner to the shared application signaling executor with automatic deadline/restart, ready-operation and capture-cleanup scheduling. The same owner is now composed by NativeRoomRuntime and the public facade.

- [x] Implement portable host capture/membership coordinator with operation IDs, session generations, bounded command queue, priority cancellation, isolated failed subscribers and joined capture/delivery teardown.
- [x] Route four-peer headless capture membership through the coordinator; test 100 restarts, stale operations and stop under queue pressure.
- [x] Extend coordinator ownership to peer/signaling generations, settings/events, audio and the production RoomSession facade; see the public-session and runtime-composition evidence in CHECKPOINT-B.md.

- [ ] Investigate the current-binary closeout resource regressions: full 100-cycle handles 386 → 462; rapid-close 20-cycle handles 329 → 339. Both exceed the bound of 8. Do not remove the existing MTA/dispatcher/module-lifetime mitigations without evidence.

- [ ] Verify complete production teardown without retained callbacks, textures, sockets or audio devices; account for native/driver caches separately. Carried from A integration-proof checklist, not marked passed.

- [x] Separate session, media, capture/audio, presentation and diagnostic boundaries.
- [ ] Complete the v2 input service and authorized input boundaries.
- [x] Keep Windows/WebRTC types out of the v2 public RoomSession control API and portable settings/status/frame ownership types. Native render/capture adapters remain explicitly Windows-specific.
- [x] Add profile, room policy, stream preferences, per-peer status, operation results, session snapshots and owned frame types.
- [x] Implement the serialized control executor and session/viewer generations.
- [x] Implement state transitions, idempotent asynchronous stop, cancellation and joined owner-thread shutdown.
- [ ] Pass complete production resource acceptance; the capture handle-growth failures remain open.
- [x] Reject stale-generation callbacks and test held-callback/queue-pressure cancellation and asynchronous UI close. Hung native-driver calls remain a separate acceptance limitation.
- [x] Preserve update-only settings without resetting unrelated fields.
- [x] Add synthetic video/audio scenarios using the same public session/native runtime as opt-in frontends.

### PeerConnections

- [x] Move asynchronous SDP create/local-apply/remote-apply into a shared application/proof library with typed operation results, cancellation, weak callbacks, single-operation admission and SDP bounds; remove the proof's old description observers.
- [x] Provide an owned signaling event loop in the shared application/proof library with bounded admission, typed completion/cancellation and joined shutdown; run real media proofs on it and verify SDP negotiation without caller message pumping.
- [x] Consume ready negotiation operations through the scheduled owner's peer interface; exercise concurrent four-peer setup, asynchronous restart and rejoin without nested waits inside peer callbacks.
- [x] Implement the room-backed peer adapter and authenticated SDP/ICE delivery through scheduled RoomPeerNegotiation/RoomNetwork ownership.

- [x] Implement a portable per-connection deadline/recovery policy with typed failures, stale-event rejection, 500 ms/1 s/2 s backoff and rolling three-per-minute restart budget.
- [x] Feed real peer ICE state into the policy and exercise a host-requested ICE restart with new credentials, settings preservation and four-peer media continuity.
- [x] Wire scheduled restart/close actions through production peer ownership and authenticated room signaling.
- [ ] Validate real network loss/interface changes; an explicit local restart request is not impairment evidence.

- [x] Add bounded generation-scoped candidate handoff and replace bundled-SDP gathering in real media proofs with trickle ICE, gated on successful description application.
- [x] Integrate bounded candidate handoff into authenticated room signaling/full peer ownership; test local restart generations and deadlines.
- [ ] Validate real STUN/NAT traversal and interface changes; local signaling does not establish these.

- [x] Implement one host-to-viewer PeerConnection, separate track/source adaptation wrapper and encoder per viewer.
- [x] Share source capture/audio without sharing viewer adaptation restrictions.
- [x] Use host-offerer Unified Plan, one-way audio/video and bundled transport/data.
- [x] Implement trickled direct ICE configuration and bounded pre-description candidate queues.
- [ ] Accept direct connectivity on real STUN/NAT paths.
- [x] Scope SDP/ICE to connection generations; discard stale messages.
- [x] Implement the 20-second connection deadline and bounded ICE restart attempts.
- [x] Keep viewer failure/recovery isolated from other viewers.

### Windows video and presentation

- [x] Adapt WGC through WindowsRoomRuntimeFactory, including generated-window media tests.
- [ ] Complete appropriate display-only DXGI fallback in the v2 runtime.
- [ ] Preserve selected-source privacy, cursor behavior, HDR-to-SDR and source identity.
- [ ] Handle source closure/minimization/resize and D3D device loss explicitly.
- [x] Implement per-viewer GPU scaling and owned GPU frames; bounded submission, fallback and hardware-encode integration are documented in GPU-SCALING.md.
- [x] Implement encoder worker/event handling and one pending raw-frame slot; burst/ownership evidence is recorded under A and CHECKPOINT-B.md.
- [x] Honor WebRTC rates, keyframes and timestamps without custom congestion logic.
- [x] Probe hardware health; quarantine failed implementations and fall back per viewer. Physical driver-hang preemption remains open.
- [ ] Preserve fixed-resolution semantics when fallback cannot sustain the configuration.
- [x] Integrate CPU NV12 decoding/upload and shared GPU presentation with conversion/repack and presentation diagnostics.
- [ ] Implement/accept hardware decoding and GPU zero-copy presentation; the current path still decodes to CPU memory.
- [ ] Preserve coded dimensions, visible aperture and aspect ratio.
- [x] Keep one replaceable pending presentation frame; remove old A/V gating from the new path.

### Audio

- [ ] Implement 48 kHz / 10 ms PCM exchange and consistent multichannel downmix.
- [x] Preserve system, microphone, selected device and process-loopback modes in shared runtime/UI/CLI selection; physical endpoint acceptance remains separate.
- [x] Use event-driven WASAPI and bounded handoffs with measured device buffering; shared-audio handover retains the 30 ms application capture bound.
- [ ] Isolate microphone processing from system/process audio.
- [x] Support device-free no-shared-audio startup/live selection in shared runtime/UI/CLI, with rollback, worker restart and silent end-to-end coverage.
- [ ] Preserve mute/volume, video-only operation and explicit device-error behavior.
  Reported startup/live capture and output failures now preserve video and support
  explicit same-device retry, with health surfaced in shared API/UI/CLI. Silent
  injected-device tests cover release, failure isolation and recovery; see
  AUDIO-RECOVERY.md. Native driver hangs and physical unplug acceptance remain open.
  Shared runtime/UI/CLI now support device-free `none` capture at startup and live,
  with capture-device release, failed-resume rollback and restart persistence.
  The Opus track remains negotiated; viewer output devices remain independent.
  Synthetic mute/volume/error and no-shared-audio coverage is implemented; physical
  unplug/recovery and full device-error acceptance keep this combined gate open.
- [ ] Use monotonic local clocks and WebRTC synchronization.

### Stream settings

- [x] Implement explicit Auto/Manual modes plus native-size resolution behavior.
- [x] Implement fresh-profile Gaming / Auto resolution / 1080p maximum / 60 FPS target / Auto bitrate defaults.
- [x] Implement the documented Auto maximum calculation and conservative startup rate.
- [x] Map all resolution/FPS mode combinations to the intended WebRTC adaptation preference.
- [x] Keep manual resolution fixed and manual bitrate subject to congestion control.
- [ ] Fit/letterbox fixed dimensions and expose the active image rectangle for input mapping.
  CPU/GPU fitting and backend/UI/CLI rectangle exposure are implemented. Receiver
  generation binding and actual input-coordinate mapping remain in milestone 3.
- [ ] Preserve manual settings when switching presets.
- [x] Add settings revisions, prevalidation and per-viewer pending/applied/error state.
- [ ] Retain/recover working settings on failed reconfiguration; report partial application honestly.
- [x] Implement optional aggregate media budgeting with overhead/audio reserve and equal per-viewer allocation.
- [ ] Show actual wire usage separately; do not add a second bandwidth-control loop.

### Automated validation

- [ ] Verify manual resolution under congestion and manual bitrate under WebRTC rate reduction.
- [x] Verify one viewer's adaptation cannot alter another viewer's restrictions.
- [ ] Verify encoder failure and hardware-to-software fallback isolation.
- [ ] Verify bounded queues with slow encoding/rendering.
- [ ] Verify frame ownership, visible dimensions, aspect ratios and source changes.
- [x] Verify silence/missing audio cannot deadlock video.
- [x] Verify cancellation, repeated stop and rejection of old callbacks.

**Gate B**

- [ ] Native engine behavior and focused tests pass with evidence; no custom competing bitrate/playout loop remains in the v2 path.

## Checkpoint C — room service v2

Plan references: Section 3 and 5 / Checkpoint C.

### Protocol and native client

- [x] Add authenticated directed signaling with host/viewer permissions, socket/offer generations, stale-ID rejection, candidate and recovery bounds, separate message/byte budgets and local workerd relay/rejection tests. Native media mapping is integrated; production load/queue-pressure acceptance remains open.

- [x] Implement authenticated profile/policy mutations, leave/kick, bounded per-generation deduplication, revision conflicts and a local socket message budget; validate authorization and removal through actual workerd sockets. Shared signaling and pushed-directory dispatch are integrated in opt-in frontends.

- [x] Add isolated v2 Worker admission/room socket membership with hashed credentials, serialized capacity checks, provisional cleanup and actual local workerd tests. Partial service scope and remaining endpoints are recorded in CHECKPOINT-C.md.

- [x] Implement native create/join HTTP admission with canonical requests, exact response/token/identity validation, typed futures, cancellation and unconfirmed outcomes without application retries. Bind the resulting socket to the admitted role; validate real HTTP failures and role-conflicting snapshots locally.

- [x] Add shared native Qt room/directory transport independent of Widgets: authenticated headers, schema/identity/direction checks, bounded writes, snapshot readiness and stale-socket rejection. Validate live I/O on a dedicated networking thread; see [CHECKPOINT-C.md](CHECKPOINT-C.md).
- [x] Add live local socket tests and integrate local Worker signaling with native media connection-generation dispatch.

- [x] Implement the six documented v2 HTTP/WebSocket endpoints in the isolated service.
- [x] Implement create/join credentials, provisional membership expiry and publish-after-host-attach.
- [x] Use the shared owned Qt networking loop implementation from both opt-in UI and CLI, with bounded cross-executor dispatch.
- [x] Implement all documented socket commands and typed errors in the isolated service.
- [x] Implement authoritative sender identity, targeted signaling and generation checks.
- [x] Implement separate room/directory revisions; exclude directed signaling from state revisions.
- [x] Implement bounded duplicate suppression, one-shot gap resync, expected-revision conflicts and reconnect snapshots; native/service components tested separately.
- [ ] Enforce message-byte limits, candidate limits and bounded queues.
- [ ] Add shared client/server protocol fixture tests.

### Persistence, hibernation and directory

- [x] Add separate SQLite Durable Object room/directory namespaces for v2.
- [x] Implement socket attachments and replacement-generation handling; local workerd replacement/reconnect passes. Hibernation reconstruction still requires coverage.
- [x] Implement automatic ping/pong and auto-response timestamp liveness; real hibernation reconstruction still needs validation.
- [x] Implement native client ping/pong deadlines, one-shot resync and jittered reconnect backoff; exercise actual timeout/reconnect locally. Opt-in UI/CLI adoption is implemented; default-shell cutover remains pending.
- [x] Implement provisional/membership expiry and immediate leave/kick.
- [x] Handle host reconnecting/expiry without host election or new joins during disconnection.
- [x] Implement safe directory summaries and snapshot/delta subscriptions.
- [x] Remove per-room listing verification and automatic HTTP polling from v2; normal application remains v1.
- [x] Implement 60-second lease renewals, 180-second expiry and directory cleanup; real alarm timing still needs validation.
- [x] Avoid visible updates for lease-only renewals.
- [x] Persist pending directory updates; retry failures and closure removal safely.
- [x] Decouple directory publication and capacity I/O from room control dispatch; validate stalled delivery, real abort/retry and late-acknowledgement races in workerd.
- [x] Enforce summary versions and fail-closed reservations/admission.

### Profile and authorization

- [x] Persist and validate local nickname without reading OS identity.
- [x] Persist validated stream preferences and playback volume/mute for browser-created sessions; generate and save a random Guest nickname for missing/invalid profiles. Existing valid nicknames remain unchanged. Device/source identifiers and credentials are excluded.
- [x] Validate nickname normalization, code points/UTF-8 size and forbidden controls.
- [x] Implement duplicate-name disambiguation with peer IDs in session membership;
  per-peer diagnostic rows and the details dialog also retain explicit peer identity.
- [x] Implement public/unlisted rooms, creation password and acknowledged live name/visibility/viewer-limit edits.
- [x] Preserve existing viewers when the limit is lowered; block new admissions.
- [x] Implement versioned room-ID links without credentials or service switching; validate parsing/copy/paste across UI and CLI.
- [x] Issue 256-bit membership tokens, store hashes and enforce role/target/socket authorization.
- [x] Preserve versioned salted PBKDF2 work factor and HTTPS-only secret handling.
- [x] Add admission and per-socket rate limits without cross-object checks for every message; load tuning remains pending.
- [ ] Preserve configurable room/participant caps, CORS restrictions and certificate validation.
- [x] Implement kick invalidation and avoid claims of permanent accountless bans.

### Worker/native integration tests

- [x] Exercise compiled Qt admission/socket clients against actual local workerd: directory state, admission, revision-bound mutations, directed signaling, reconnect, visibility, kick and closure. One-command execution has watchdogs and hashed artifacts. Additional UI/CLI scenarios now exercise actual production H.264/Opus media using synthetic endpoints.

- [x] Add Worker typecheck/test scripts and execute tests in the local Cloudflare runtime.
- [x] Test simultaneous joins, provisional expiry and capacity races; expiry uses injected persisted deadlines with the actual alarm handler.
- [ ] Test passwords, invalid/expired tokens, replay and unauthorized commands.
- [ ] Test old-socket close after replacement and hibernation heartbeat freshness.
- [ ] Test duplicate/gapped revisions and stale connection candidates.
- [x] Test directory write/removal failures, retries and leases in actual workerd with injected failures/deadlines.
- [ ] Test host crash/leave, public/unlisted visibility and viewer-limit changes.
- [ ] Test malformed/oversized messages, Unicode handling and signaling floods.
- [ ] Verify listing has zero per-room fanout and heartbeat has zero per-peer storage writes.

**Gate C**

- [ ] Real native-client/local-Worker integration and security/lifecycle tests pass with evidence.

## Checkpoint D — UI and gaming controls

Plan references: Sections 2.7, 4.1–4.2 and 5 / Checkpoint D.

### Product integration

- [ ] Route normal UI/CLI sessions and room operations through the new shared interfaces.
- [x] Add Auto/Manual controls, Gaming/Quality presets and honest apply status.
- [x] Add local nickname settings and server-pushed room browsing in opt-in v2 UI.
- [x] Close directory subscriptions during sessions and resubscribe on return; reject stale-list joins while disconnected.
- [x] Add acknowledged adjustable viewer capacity.
- [x] Add a warning for capacity above four in live host capacity controls.
- [x] Add host per-viewer summary rows and selectable inline details for requested
  preferences, applied caps, source-observed size/revisions and measured transport.
  UI and CLI share status names; absent/stale rates stay unknown, measured zero
  stays zero. Selection follows peer identity across refreshes.
- [x] Add receiver-reported decoded dimensions, frame count and optional FPS to
  host rows/details and CLI, separate from source observations and presentation.
- [x] Add a live nonmodal details popup pinned to peer identity; clear measurements
  on departure/stop. Extract the snapshot-only diagnostics widget from room controls.
- [ ] Add remaining receiver presentation/drop/buffering and codec/fallback metrics.
- [x] Sample local sender transport once per second and suppress samples at the
  three-second deadline; test exact freshness boundary, resets and peer isolation.
- [x] Sample receiver decoder statistics once per second and expire reports after
  three seconds; real four-viewer coverage pauses telemetry while media continues.
- [x] Keep decoder telemetry on the encrypted unreliable peer channel, not Cloudflare;
  validate version/length/ranges/sequence/generation and bound pending work.
- [x] Expose sender application states (pending, rejected, upload-paused,
  waiting-for-source, source-observed) and fresh/stale/unknown transport samples.
- [x] Add typed WebRTC limiting reasons, video payload/encoded FPS, selected-pair
  RTT/bandwidth estimate, linked RTCP loss/jitter and receiver report age. Reject
  invalid/missing values, expire measurements and replace mailboxes across negotiation.
- [ ] Add remaining receiver/capture/input limiting reasons and unknown-valued metrics.
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

- [ ] Complete missing legacy GPU utilization, external display/input latency and available multi-viewer comparative baseline; existing one-viewer CPU/queue evidence is in CLOSEOUT-A.md.
- [ ] Validate forced HWND reuse and actual device-loss recovery on hardware; injected recovery is proven, actual driver removal remains untested.

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

- [ ] Complete remaining distribution obligations/notices (including Qt license texts/source requirements), actual Inno installer compilation/staging and a fresh-machine run before release. Carried from A; not marked passed.

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

Gate A is closed against its original integration/build criterion. The closeout resource checks FAILED their handle-growth bounds despite completed cycles; prior passing runs are historical evidence, not a current no-leak guarantee. Track native resource ownership under B and full acceptance under E. Normal application media remains legacy until validated integration supports switching it. See CLOSEOUT-A.md for exact results and retained release/hardware limitations.

At each checkpoint handoff, record:

- Completed tasks and evidence.
- Current working-tree changes.
- Failed or unrun tests and why.
- Any measured tuning changes and corresponding plan updates.
- The exact next unfinished task.

### Historical lifecycle continuation — 2026-09-15

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
- [ ] Preserve native-call watchdog coverage and validate production capture-owner/session integration before production cutover; current handle-growth failures are recorded in CLOSEOUT-A.md.

### Shared capture session and headless media — 2026-09-15

- [x] Move capture-worker ownership and recovery from the proof into a portable production CaptureSession, with session/device generations, typed terminal failures and joined callback teardown.
- [x] Route WGC recovery/live PeerConnection proofs and paced synthetic PeerConnection video through that same owner.
- [x] Add headless 100-restart, recovery, slow-consumer, callback-failure, startup-timeout and cancellation scenarios.
- [x] Add one-command headless smoke/regression with watchdogs, executable hashes, JSON timing/results and preserved failure logs.
- [x] Extend automated scenarios to production RoomSession/NativeRoomRuntime, settings, real peer isolation and opt-in UI/CLI; see the 2026-09-16/17 integration evidence.
- [ ] Complete separate host/viewer processes, scripted authorized input and simulated network conditions.

Historical continuation evidence is in HEADLESS-TESTING.md and CHECKPOINT-A.md; the current gate decision is in CLOSEOUT-A.md.

### Independent capture delivery — 2026-09-15

- [x] Add a production bounded per-viewer handoff, keeping one pending frame per viewer and independent delivery workers.
- [x] Reject stale session/device-generation/sequence samples; join removed subscriptions before allowing replacement.
- [x] Route synthetic and WGC media proofs through the shared distributor.
- [x] Test four consumers, slow/failing viewer isolation, removal during acquisition and pending-frame replacement under deterministic blocking.
- [x] Validate local isolation through four actual PeerConnections with independent source restrictions and encoders; see authenticated/public four-viewer scenarios in CHECKPOINT-B.md. Network impairment acceptance remains separate.

### Four-peer headless media — 2026-09-15

- [x] Extract the reusable per-viewer WebRTC capture source, preserving acquisition timestamps and owned dimensions and declaring screen-content metadata.
- [x] Exercise four actual host/viewer PeerConnection pairs from one capture session with separate source wrappers and video senders.
- [x] Verify slow source-handoff isolation, per-viewer sender-limit independence, recovery and full connection leave/rejoin while healthy peers continue.
- [x] Include one four-peer scenario in headless smoke and three in the longer regression, with JSON results and a 60-second process watchdog.
- [x] Add production Auto/Manual mapping and per-viewer received Opus evidence.
- [ ] Complete decoder/network impairment and actual rate/latency acceptance; sender-parameter checks are not congestion proof.

### Stream settings core — 2026-09-15

- [x] Implement validated Auto/Manual preferences and Gaming/Quality degradation mapping; calculate default bitrate ceiling and conservative initial rate without a bitrate floor.
- [x] Apply single-viewer RTP limits and source settings with monotonically increasing revisions; reject invalid/stale updates before mutation.
- [x] Adapt each source independently through WebRTC VideoAdapter; preserve fixed canvases, native dimensions, aspect ratio and bounded FPS dropping.
- [x] Test fixed/manual versus adaptive sink requests, upward recovery, letterbox pixels and settings propagation through real PeerConnections.
- [x] Report and test GPU resize readback fallback and native-frame preservation at matching dimensions.
- [x] Replace routine GPU resize readback with bounded per-viewer NV12 GPU scaling; verify pixels, hardware encoder input, source diagnostics and four-viewer WGC integration. See GPU-SCALING.md and CHECKPOINT-B.md.
- [x] Wire initial bandwidth settings, aggregate upload allocation and public/UI per-peer sender/source-observed revisions.
- [ ] Complete capability failure recovery and remote displayed-state acceptance; sender/source application is not a remote-display acknowledgement.
- [ ] Verify real congestion-driven adaptation and GPU scaling performance; controlled sink requests alone do not satisfy impairment/latency gates.
# Integration update — 2026-09-16

Execution now follows grouped milestones in [TODO.md](TODO.md). Original detailed
checks below remain acceptance requirements, not individual turn goals.

- [x] Shared RoomPeerNegotiation routes actual SDP/ICE through authenticated room
  sockets, with description ordering, bounded candidates and generation barriers.
- [x] Real local workerd four-viewer H.264/synthetic Opus scenario exercises slow
  delivery, twelve encrypted channels, ICE restart, kick/rejoin and room shutdown.
- [x] Complete automatically scheduled shared session facade and opt-in UI/CLI adoption.
- [ ] Complete default UI/CLI cutover after remaining acceptance gates.
