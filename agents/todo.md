# TODO

This file tracks unfinished work only. Completed release milestones belong in Git history and release notes.

## Active Priorities

### Backend v2 refactor — current work

Execute the grouped delivery milestones in [refactor/TODO.md](../refactor/TODO.md).
Native source roots are now backend/ and frontend/. RoomPeerNegotiation schedules
its own completion/deadline work. RoomNetwork owns the dedicated networking loop;
RoomPeerRoster now drives actual media membership in the authenticated proof.
Continue with automatic cross-executor dispatch, asynchronous capture cleanup,
recovery budgets and public events in the normal shared facade.

RoomManagedPeer now connects attachment, retirement and restart policy in the
real-room proof. HostPeerOwner.BeginStop drains capture asynchronously; runtime
commands must use it and await completion before owner destruction. Finish normal
facade dispatch/public events rather than rebuilding these completed primitives.

RoomSessionCoordinator and RoomSignalCodec now provide automatic cross-executor
dispatch and signaling conversion in the real-media proof and shared build target.
Next move peer-factory/session composition out of the diagnostic and adopt the
normal facade's public commands/status; do not recreate manual polling/SDP relay.
The checks below are implementation details within those batches, not individual
turn goals. Preserve all original checks in [DETAIL-CHECKS.md](../refactor/DETAIL-CHECKS.md).

- [ ] Complete the shared scheduled session facade around RoomAdmission/RoomSocket and RoomPeerNegotiation. Real authenticated four-viewer media now passes local workerd with restart, kick/rejoin, slow-viewer isolation and encrypted channels. Normal UI/CLI orchestration, production hibernation, load, queue pressure and latency acceptance remain open.

- [ ] Integrate the implemented authenticated media adapter into automatic roster-driven ownership and socket-failure recovery. Preserve fresh wire IDs, description-before-candidate ordering and generation barriers; validate pressure, hibernation and cost/load beyond the local real-media proof.

- [ ] Finish configurable service caps and browser CORS/preflight support if needed; retain closed-by-default Origin handling and required bindings. Validate hibernation reconstruction and real alarm timing beyond injected expiry tests before deployment/cutover.

- [ ] Adopt RoomAdmission/RoomSocket in the application coordinator and connect media connection-generation dispatch. Combined native-to-workerd validation now exists; normal UI/CLI adoption remains open. See refactor/CHECKPOINT-C.md.

- [ ] Add easy, one-command headless testing of realistic live sessions through production APIs, without manual mouse/keyboard input. Include paced media, scripted control to a test-owned sink, network/recovery scenarios, metrics and watchdog cleanup; distinguish synthetic headless from desktop/GPU-dependent capture checks. Full requirement: refactor/PLAN.md. Implementation resumed: shared capture ownership, bounded independent viewer delivery, paced synthetic media, four real peer connections with slow-handoff/rejoin coverage and one-command headless smoke/regression now exist (refactor/HEADLESS-TESTING.md); Auto/Manual source/RTP settings and revision validation now exist; complete facade, separate processes, input and network scenarios remain.

- [ ] Embed scheduled HostPeerOwner in the shared UI/CLI facade and supply a room-backed peer adapter. The shared signaling executor now drives deadlines, completion polling, restart and capture cleanup automatically; real four-peer setup/restart/rejoin uses it. Preserve the rolling restart budget and investigate measured restart timing before claiming responsive outage recovery.
- [ ] Carry the authenticated RoomPeerNegotiation adapter into scheduled production ownership and restart policy. Its bounded candidate handoff, SDP ordering and deadlines now run against workerd with actual media. STUN/NAT remain open. Do not copy outer diagnostic waits into application commands.
- [ ] Continue Checkpoint B with PeerConnection/signaling ownership and shared UI/CLI facade integration. Portable host capture/membership coordination now passes 100 restarts, stale-operation and queue-pressure cancellation tests and drives the four-peer proof; see refactor/CHECKPOINT-B.md. A is closed against its original integration/build criterion; see refactor/CLOSEOUT-A.md.
- [ ] Run matched legacy/v2 workloads and external gaming image/input latency measurements as integration becomes available. Record per-requirement verdicts using refactor/COMPARISON.md; current headless passes do not prove the replacement is better.
- [ ] Investigate current-binary handle-growth failures: +76 over 100 full capture cycles and +10 over 20 rapid-close cycles (bound 8). Cycles complete, resource acceptance fails; resolve before production cutover.
- [ ] Complete Checkpoint E external latency/comparative baseline and release installer/distribution evidence. Shared v2 command/event validation now exists in native C++ and TypeScript; native room/directory caches validate complete state atomically with generation/revision traces; see refactor/ROOM-PROTOCOL.md. Content-addressed Debug/Release SDK exports, relocated SDK consumers and native portable launch pass. The unloaded-GraphicsCapture.dll crash now has a module-lifetime mitigation with 100 full Release cycles passing; capture-owner dispatch now completes 100 rapid-close cycles without the old hang; scoped application MTA ownership previously passed full and rapid-close 100-cycle handle bounds, but the current closeout runs fail those resource bounds; see CLOSEOUT-A.md.
- [ ] Complete capture/encoder device recovery and actual device-removal validation. Receiver presentation now rebuilds resources with a three-rebuild session budget and 250 ms backoff; injected-error policy and desktop resource-recreation proofs pass.
- [ ] Validate remaining production session/resource lifecycle with current closeout handle-growth failures tracked explicitly; prior MTA runs passed but do not establish current clean teardown. Capture-owner dispatcher integration mitigates the rapid StopCapture hang; dedicated owner-thread and caller-queue contracts are documented. The earlier Release crash was reproduced in unloaded GraphicsCapture.dll and mitigated by a one-time system-module pin; 100 full cycles pass. Shared CaptureSession now owns both WGC and synthetic sources with joined callback teardown; connect it to the complete production session coordinator with downstream generation barriers; proof now has three rebuilds, cancellable backoff and per-device retirement across replacement generations.
- [ ] Add device-loss recovery and forced HWND reuse stress coverage; external process exit, minimize/restore and permanent source closure now pass; GPU presentation of owned CPU NV12, bounded pending frames and receiver resize now pass; WGC shutdown regression and live PeerConnection delivery now pass; WASAPI ADM/Opus, hardware/software MF, bounded submission, missing-output fallback and owned GPU frame proofs now pass. Complete release delivery evidence and broader audio mode/recovery validation.
- [ ] Continue the ordered implementation and validation in [refactor/TODO.md](../refactor/TODO.md), using [PLAN.md](../refactor/PLAN.md) as the specification.

The v2 checklist supersedes legacy transport/adaptation work below where replacement is planned. User authorized removing the uncommitted legacy UDP adaptation/recovery work; it was archived locally and removed on `refactor/backend-v2`.

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
