# TODO

## Release Readiness

- [ ] Deploy the signaling Worker with the new `RATE_LIMITER` Durable Object binding and migration `v3`, then validate the cross-colo limiter, room-count cap, and directory sweep against the live Cloudflare account.
- [ ] Exercise an encrypted session through a real NAT rebind before relying on automatic retargeting in the field.

## Build Work

1. Performance / latency optimization pass (top priority, once the mouse/keyboard remote-control feature is finished).
   - [ ] Profile and cut latency + CPU across the live pipeline: capture -> encode -> send, and receive -> decode -> present.
   - [ ] **Multi-viewer stalls:** with more than one viewer the stream sometimes gets stuck for viewers. Investigate the fanout send path (shared encoder output to multiple `UdpSender`s, per-target queue/pacing, keyframe delivery per target) — a slow/late target or per-target backlog may stall delivery.
   - [ ] Low-latency mode follow-ups: live toggle in the active Share window, optional viewer-side audio-buffer trim, and confirm the encoder never holds frames for lookahead.

2. User-facing diagnostics pass.
   - [ ] Map known runtime/report states to plain UI messages, starting with waiting for stream, password/encryption mismatch, UDP hole-punch failure, host left, and host idle.
   - [ ] Show an actionable next step in the active Share/Watch screens when setup is not healthy.
   - [ ] Keep warnings driven by real runtime/report signals, not guesses.

2. Runtime state polish.
   - [ ] Make active-session wording freshness-aware so connected, idle, disconnected, and waiting states do not stay sticky after packets or feedback stop.
   - [ ] Keep Share and Watch state transitions consistent when viewers join, leave, rejoin, or when the host leaves.
   - [ ] Clean up NAT/feedback status summaries so they match the current session state, not only the last successful event.

3. Report summary polish.
   - [ ] Add the smallest useful report fields needed by the new UI diagnostics.
   - [ ] Warn about likely silent or wrong-device audio capture only when transport is healthy but audio evidence looks wrong.
   - [ ] Promote items from Report-Driven Follow-Ups into build work only after reports reproduce them.

## Remote Control & Optimizations

Remote viewer control (mouse/keyboard, host-permissioned, per-input-type) landed for v0.2.0.
Gamepad is deferred to v2 (ViGEm). Follow-ups and performance work:

- [ ] Optimization pass across the live pipeline (capture/encode/send + receive/decode/present) to cut latency and CPU.
- [ ] **Multi-viewer stalls:** with more than one viewer, the stream sometimes gets stuck for viewers. Investigate the fanout send path (shared encoder output to multiple `UdpSender`s, per-target queue/pacing, keyframe delivery per target) — a late/slow target or per-target backlog may be stalling delivery.
- [ ] Low-latency mode follow-ups: live toggle in the active Share window (currently applied via room/stream settings), optional viewer-side audio-buffer trim, and confirm the encoder never holds frames for lookahead.
- [ ] Gamepad (v2): viewer XInput capture -> control packets (already in the wire protocol) -> host virtual pad via ViGEmClient (needs the ViGEmBus driver) + enable the Share gamepad toggle.

### Remote-control review follow-ups (PR #143 code review; deferred, non-blocking)

- [ ] **Window-capture remote input:** `refreshRemoteControlBounds` only maps display bounds, so remote mouse control is a no-op while sharing a *window* (bounds are cleared on switch-to-window). Add client-rect-in-screen-space mapping for `CaptureSourceType::Window` (track window move/resize).
- [ ] **Lazy mouse hook:** `RemoteInputInjector` installs a system-wide `WH_MOUSE_LL` hook + pump thread for *every* host session, even view-only ones. Construct the injector/monitor lazily only once a mouse capability is first granted.
- [ ] **Scroll targeting:** `InjectMouseScroll` sends `MOUSEEVENTF_WHEEL` without repositioning the cursor, so the wheel hits whatever window is under the *host's* real cursor. Carry the cursor position with scroll and reposition first (like clicks do).
- [ ] **Stuck keys/modifiers:** a lost key-up datagram (or releasing control mid-hold) leaves a key logically held on the host. Release all currently-held viewer keys/buttons when control ends or the controller changes.
- [ ] **"Requesting..." soft-lock:** if the host never answers, the viewer button stays "Requesting..." with no timeout/cancel. Add a timeout back to "Request Control" and let a click cancel a pending request.
- [ ] **Mid-session low-latency:** the live in-room toggle flips send pacing but not `udpMaxQueueMs` (which room-creation sets to 0 for low latency), so a live toggle doesn't drop already-queued buffer depth the way starting fresh does.
- [ ] **Cleanup:** dedup the control-peer upsert block in `UdpSender` (feedback path vs `ProcessControlPacket`) into one `UpsertControlPeerLocked` helper; input auto-repeat and hi-res-wheel handling for `VideoFrameWidget` (classic mice already correct).

## Backlog / Not Now

These are real ideas, but they should stay behind the current diagnostics/state/report polish pass.

- Fix custom title bar buttons so minimize, maximize/restore, and close keep working on the active Watch screen, maximized windows, and embedded preview surfaces.
- Keep drag-to-move, drag-to-maximize, resize borders, and rounded-corner outline behavior consistent across normal, maximized, and fullscreen transitions.
- Add adaptive FPS only after there is a real runtime policy for when to raise/lower FPS.
- Add encoder preference/preset switching only after the runtime can change encoders safely mid-session.
- Promote advanced CLI-only Share/Watch diagnostic flags into typed configs only if they become normal app controls.
- Split diagnostic-only commands out of `ScreenShareCLI.cpp` only if they start crowding the parser again.
- Consider optional UPnP/NAT-PMP port mapping only if repeated reports show direct Worker/STUN rooms failing on otherwise important networks.
- Add a richer debug overlay only if title telemetry or UI diagnostics become too dense.

## Report-Driven Follow-Ups

Keep these dormant until real reports show the pattern. Normal audio, no-audio fallback, freezes, and current Worker rooms are currently behaving well in user testing.

- Include receiver A/V sync/playout fields in receiver feedback summaries so sender reports can diagnose title values like `av -600ms` without separately collecting the receiver log.
- Investigate small-drift audio time-stretching only if A/V catch-up drops become audible or too frequent; keep hard drops for large real-time recovery.
- Improve the title text if users confuse diagnostic A/V estimates with true audible drift.
- Make `--stream-encoder auto` smarter by detecting hardware encoder input drops and falling back to software or marking that hardware encoder as unhealthy.
- Measure and report max encoder input queue age in milliseconds.
- Prefer dropping stale queued encoder input before it becomes visible latency.
- Revisit UDP pacing if future reports show backlog with fixed 125% pacing headroom; make headroom adaptive instead of fixed if needed.
- Avoid arbitrary UDP queue media drops inside a GOP unless paired with a keyframe strategy.
- Consider keyframe request/force after sender skips frames.

## Conditional / Deferred

These are intentionally outside the current UI/runtime polish loop.

- Revisit synced audio rejoin after automatic video-only fallback only if a real report shows audio appears late and needs to become synced automatically.
- Consider making CLI plaintext mode require `--allow-plaintext` after the encrypted/invite UX is smooth enough to avoid surprising users.
- Keep helper CLI diagnostics only where they are genuinely diagnostic, not normal UI data paths.
