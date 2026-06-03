# TODO

## Build Work

1. Performance optimization pass.
   - [x] Add or tighten hot-path timing so reports clearly show capture time, encode time, UDP queue delay, decoder time, preview queue, dropped frames, viewer queue, audio queue, and likely CPU/GPU bottlenecks.
   - [ ] Optimize the sender GPU path so display/window capture, scaling, NV12 conversion, and hardware encoding avoid CPU readback unless diagnostics require it.
   - [ ] Reduce encoder latency with low-latency settings, bounded queues, stale-frame dropping before visible lag, and safer hardware-to-software fallback when hardware queues misbehave.
   - [ ] Keep receiver preview fast by avoiding extra frame copies into Qt, reducing resize/present stalls, and keeping embedded/fullscreen presentation on the same efficient path.
   - [ ] Optimize UDP fanout and packet queues for multiple viewers, including allocation reuse, queue caps, feedback handling, and keyframe recovery after drops.
   - [ ] Keep audio capture/playback and Opus queues bounded and event-driven so audio cannot build long live latency.
   - [ ] Review release build settings for safe speed wins such as optimized release flags and optional LTO, without mixing this up with DLL/static-link packaging.

2. User-facing diagnostics pass.
   - [ ] Map known runtime/report states to plain UI messages, starting with waiting for stream, password/encryption mismatch, UDP hole-punch failure, host left, and host idle.
   - [ ] Show an actionable next step in the active Share/Watch screens when setup is not healthy.
   - [ ] Keep warnings driven by real runtime/report signals, not guesses.

3. Runtime state polish.
   - [ ] Make active-session wording freshness-aware so connected, idle, disconnected, and waiting states do not stay sticky after packets or feedback stop.
   - [ ] Keep Share and Watch state transitions consistent when viewers join, leave, rejoin, or when the host leaves.
   - [ ] Clean up NAT/feedback status summaries so they match the current session state, not only the last successful event.

4. Report summary polish.
   - [ ] Add the smallest useful report fields needed by the new UI diagnostics.
   - [ ] Warn about likely silent or wrong-device audio capture only when transport is healthy but audio evidence looks wrong.
   - [ ] Promote items from Report-Driven Follow-Ups into build work only after reports reproduce them.

## Backlog / Not Now

These are real ideas, but they should stay behind the current diagnostics/state/report polish pass.

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
