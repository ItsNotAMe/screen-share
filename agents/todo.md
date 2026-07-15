# TODO

## Release Readiness

- [ ] Validate the cross-colo rate limiter, global room-count cap, and alarm-based directory sweep under multi-colo/live load.
- [ ] Exercise an encrypted session through a real NAT rebind before relying on automatic retargeting in the field.

## Next Updates

Controller support is the next major feature. Multi-viewer reliability ships first because signaling-room viewers currently share transport state in ways that can let one unhealthy endpoint disrupt the others; controller ownership and revocation also need stable per-viewer identity. Controller protocol/backend work can start while the multi-viewer failure is being reproduced, but the controller release should build on the transport fix.

### v0.2.3 — Multi-viewer reliability

Goal: one encoded 1080p60 stream serves multiple viewers without a slow, lossy, late-joining, leaving, or unreachable viewer freezing healthy viewers.

#### 1. Reproduce and instrument before changing policy

- [ ] Add a repeatable two/three-viewer release-build harness: one host, two healthy watchers, then variants with artificial loss/jitter, a slow watcher, late join, leave, and rejoin.
- [ ] Capture stable per-viewer identity and report, at minimum: queue depth/age, datagrams and bytes sent, drops, socket errors, feedback age/health, last completed frame, decoder resyncs, and join-to-first-frame time.
- [ ] Reproduce the current 1080p failure for at least 10 minutes and save host plus viewer reports as the baseline. Do not optimize by guesswork before identifying whether the first failure is send coupling, queue growth, feedback/adaptation, keyframe recovery, or receiver state.

#### 2. Isolate every viewer's send lifecycle

- [ ] Replace the signaling-room `additionalTargets` behavior where viewers share one fatal worker/queue outcome with per-viewer send lanes over the shared NAT-bound socket (or an equivalent design that preserves the required source port).
- [ ] Give each viewer independent queue limits, pacing timestamps, drop counters, health, and failure state. An error sending to one address must retire/retry only that viewer and must never clear another viewer's queued media or stop the shared sender.
- [ ] Make join/leave/rejoin atomically add, remove, and replace a viewer lane; expire stale endpoints and feedback without leaving duplicate destinations behind.
- [ ] Keep encode-once fanout. Do not add one encoder per viewer for this release.

#### 3. Make recovery deterministic

- [ ] Force/request an IDR keyframe when a viewer joins, rejoins, reports decoder resync/no completed video, or is reactivated after a stale interval; rate-limit requests so one bad viewer cannot force an IDR storm.
- [ ] Ensure queue dropping is frame/GOP-aware: discard stale whole frames for one viewer, preserve audio responsiveness, and make the next deliverable video frame decodable.
- [ ] Separate feedback freshness from the last successful feedback. A stale/bad viewer may reduce its own delivery quality or be retired after a grace period, but must not permanently hold global adaptation at a broken state.
- [ ] Define the single-encoder congestion policy explicitly: protect healthy viewers, apply a bounded global bitrate reduction only when multiple fresh viewers agree, and surface when one viewer is the outlier.

#### 4. UI and acceptance gate

- [ ] Show each viewer as `connecting`, `live`, `recovering`, `degraded`, or `disconnected`, driven by fresh per-viewer transport/feedback evidence.
- [ ] Add a host-side action to disconnect one unhealthy viewer without ending the share.
- [ ] Pass 30 minutes with two viewers at 1920x1080/60 with audio and no unexplained freezes; host queue age remains bounded and both viewers keep receiving fresh frames.
- [ ] Pass 15 minutes with three viewers at 1920x1080/60 on the same LAN.
- [ ] With one viewer under 5% loss plus 200 ms jitter/slow consumption, a healthy viewer remains live and within its normal queue/latency envelope.
- [ ] A late join/rejoin shows decodable video within 2 seconds; leaving one viewer does not interrupt the others; one unreachable endpoint does not fail the sender.
- [ ] Repeat the two-viewer test across real Internet/NAT paths and attach all sender/viewer reports before releasing v0.2.3.

### v0.3.0 — Controller support

Goal: a viewer can use one physical Windows controller to drive one host-side Xbox-compatible virtual controller, with the same explicit host consent, single-controller ownership, encryption, panic revoke, and disconnect safety as mouse/keyboard control.

#### 1. Backend decision and thin vertical slice

- [ ] Introduce a `VirtualGamepadBackend` interface so transport/runtime/UI code is independent of the driver choice.
- [ ] Implement an optional ViGEmClient backend for the first vertical slice only if ViGEmBus is already installed; do not silently install or bundle the retired driver. Detect missing/incompatible installations and show an actionable host message.
- [ ] In parallel, scope the maintained long-term backend as a signed KMDF virtual HID source driver using Microsoft's Virtual HID Framework (VHF). Record signing/install/update cost before deciding whether it replaces ViGEm for v0.3.0 or a later release.
- [ ] First proof: a local fixed controller state creates one virtual Xbox 360-compatible pad, moves both sticks/triggers, presses every button, returns to neutral, and disconnects cleanly.

#### 2. Protocol and viewer capture

- [ ] Add a versioned `GamepadState` control command; the existing gamepad capability bit is only reserved and the wire message does not yet carry controller state.
- [ ] Carry controller slot/id, buttons, both triggers, and both signed stick axes in fixed-width validated fields. Keep the existing encrypted control channel and anti-replay sequence.
- [ ] Poll XInput on the viewer at a bounded rate (target 125 Hz), send changed state plus a low-rate keepalive, and coalesce queued states so old stick positions cannot build latency.
- [ ] Handle controller hotplug and allow the viewer to choose among connected controllers; default to the first active controller without changing mouse/keyboard capture behavior.
- [ ] Leave rumble, lightbar, gyro, touchpad, controller remapping, and multiple simultaneous virtual pads out of the MVP unless the vertical slice proves they are nearly free.

#### 3. Host authorization and fail-safe behavior

- [ ] Enable the host's existing Gamepad toggle and include it in first-grant consent, the persistent control indicator, status events, reports, and the global panic-revoke path.
- [ ] Keep one controlling viewer per session initially. A controller request/grant must identify the same stable viewer lane used by multi-viewer transport.
- [ ] Apply a gamepad-specific input-rate limit and validate every enum/axis/button value before it reaches the backend.
- [ ] Submit a neutral state and destroy/release the virtual pad on viewer release, host revoke, panic revoke, session end, viewer timeout/disconnect, controller unplug, backend error, or app crash recovery. No button, trigger, or stick may remain held.

#### 4. Controller acceptance gate

- [ ] Unit-test controller-state serialization/parsing, bounds validation, changed-state coalescing, replay rejection, and every neutralization path.
- [ ] Test Xbox/XInput-compatible and common PlayStation controllers on the viewer (through Windows' XInput mapping where available), including unplug/replug during a session.
- [ ] Validate the host virtual pad in Windows Game Controllers plus at least two real games, including analog range, diagonals, triggers, simultaneous buttons, and reconnect behavior.
- [ ] Pass a 60-minute controller session with no stuck input and no unbounded control queue; panic revoke and network loss neutralize the pad immediately.
- [ ] Validate controller use while two viewers are connected: only the granted viewer controls the pad, switching ownership neutralizes the previous viewer first, and video/audio remain stable.

## Follow-on Build Work

- [ ] Profile and cut latency + CPU across capture -> encode -> send and receive -> decode -> present after the multi-viewer transport is stable.
- [ ] Finish low-latency mode follow-ups: update `udpMaxQueueMs` on a live toggle, optionally trim viewer audio buffering, and confirm the encoder never holds frames for lookahead.
- [ ] Map known runtime/report states to plain UI messages with actionable next steps, starting with waiting for stream, encryption mismatch, UDP hole-punch failure, host left, and host idle.
- [ ] Make active-session wording and NAT/feedback summaries freshness-aware and consistent across join, leave, rejoin, and host departure.
- [ ] Add only the report fields required by these diagnostics; warn about silent/wrong-device audio only when transport is healthy and evidence supports it.

### Remote-control review follow-ups (PR #143 code review; deferred, non-blocking)

- [ ] **Lazy mouse hook:** `RemoteInputInjector` installs a system-wide `WH_MOUSE_LL` hook + pump thread for *every* host session, even view-only ones. Construct the injector/monitor lazily only once a mouse capability is first granted.
- [ ] **Stuck keys/modifiers:** a lost key-up datagram (or releasing control mid-hold) leaves a key logically held on the host. Release all currently-held viewer keys/buttons when control ends or the controller changes.
- [ ] **"Requesting..." soft-lock:** if the host never answers, the viewer button stays "Requesting..." with no timeout/cancel. Add a timeout back to "Request Control" and let a click cancel a pending request.
- [ ] **Mid-session low-latency:** the live in-room toggle flips send pacing but not `udpMaxQueueMs` (which room-creation sets to 0 for low latency), so a live toggle doesn't drop already-queued buffer depth the way starting fresh does.
- [ ] **Cleanup:** dedup the control-peer upsert block in `UdpSender` (feedback path vs `ProcessControlPacket`) into one `UpsertControlPeerLocked` helper; input auto-repeat and hi-res-wheel handling for `VideoFrameWidget` (classic mice already correct).

## Backlog / Not Now

These are real ideas, but they should stay behind the v0.2.3 multi-viewer and v0.3.0 controller milestones.

- Fix custom title bar buttons so minimize, maximize/restore, and close keep working on the active Watch screen, maximized windows, and embedded preview surfaces.
- Keep drag-to-move, drag-to-maximize, resize borders, and rounded-corner outline behavior consistent across normal, maximized, and fullscreen transitions.
- Add adaptive FPS only after there is a real runtime policy for when to raise/lower FPS.
- Add encoder preference/preset switching only after the runtime can change encoders safely mid-session.
- Promote advanced CLI-only Share/Watch diagnostic flags into typed configs only if they become normal app controls.
- Split diagnostic-only commands out of `ScreenShareCLI.cpp` only if they start crowding the parser again.
- Consider optional UPnP/NAT-PMP port mapping only if repeated reports show direct Worker/STUN rooms failing on otherwise important networks.
- Add a richer debug overlay only if title telemetry or UI diagnostics become too dense.

## Report-Driven Follow-Ups

Keep these dormant until real reports show the pattern. Normal audio, no-audio fallback, and single-viewer Worker rooms are currently behaving well in user testing; multi-viewer reliability is tracked above as active work.

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
