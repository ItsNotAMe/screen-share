# TODO

This file tracks unfinished work only. Completed release milestones belong in Git history and release notes.

## Active Priorities

### Backend v2 refactor — current work

- [ ] Complete Checkpoint A baseline and pinned native WebRTC codec/audio/build proof before wider migration.
- [ ] Complete capture/encoder device recovery and actual device-removal validation. Receiver presentation now rebuilds resources with a three-rebuild session budget and 250 ms backoff; injected-error policy and desktop resource-recreation proofs pass.
- [ ] Add device-loss recovery and forced HWND reuse stress coverage; external process exit, minimize/restore and permanent source closure now pass; GPU presentation of owned CPU NV12, bounded pending frames and receiver resize now pass; WGC shutdown regression and live PeerConnection delivery now pass; WASAPI ADM/Opus, hardware/software MF, bounded submission, missing-output fallback and owned GPU frame proofs now pass. Complete remaining Gate A delivery evidence and broader audio mode/recovery validation.
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
