# TODO

## Ordered Roadmap

1. Wire the UI to the Worker room flow: default to the simple room/internet path, hide low-level invite/manual concepts unless advanced mode is opened, show room connection state clearly, and expose the room link/password flow cleanly.
2. Replace visible "access code" UX with secure-by-default hidden room keys. The app should auto-generate the key, keep UDP encrypted even with no typed password, and only show a separate room password if the user explicitly enables one.
3. Validate NAT multi-viewer rooms with real computers using the signaling flow. Follow-ups after testing: per-viewer connection/health display, optional per-viewer bandwidth policy, and better fallback UX for watchers whose NAT still needs direct invite/manual help.
4. Build the stage-2 native UI after the core share/watch/session flow settles. Keep it modern, simple, dark-mode friendly, and integrated into the program itself rather than just a launcher shell. Favor a room model: auto-updating available rooms/devices list, locked-room indicators, optional password prompt on join, simple host/join flows, window/screen selection, audio window/source selection, and clearer in-session state.
5. Add better user-facing diagnostics for remaining common setup mistakes and sync/network states once reports show the need.

## Live-Streaming Hardening

Only pick these up when reports show the need; normal audio and no-audio fallback are currently working in user testing.

- Include receiver A/V sync/playout fields in receiver feedback summaries so sender reports can diagnose title values like `av -600ms` without separately collecting the receiver log.
- Investigate small-drift audio time-stretching only if A/V catch-up drops become audible or too frequent; keep hard drops for large real-time recovery.
- Improve the title text if users confuse diagnostic A/V estimates with true audible drift.
- Make `--stream-encoder auto` smarter by detecting hardware encoder input drops and falling back to software or marking that hardware encoder as unhealthy.
- Measure and report max encoder input queue age in milliseconds.
- Prefer dropping stale queued encoder input before it becomes visible latency.
- Revisit UDP pacing if future reports show backlog with fixed 125% pacing headroom; make headroom adaptive instead of fixed if needed.
- Avoid arbitrary UDP queue media drops inside a GOP unless paired with a keyframe strategy.
- Consider keyframe request/force after sender skips frames.

## NAT Traversal Follow-Ups

- Keep media relay/TURN out of the main path unless direct STUN/signaling/manual invite/hole punching proves insufficient.
- Consider optional UPnP/NAT-PMP port mapping after the two-sided invite flow if users still hit blocked paths.

## Conditional / Deferred

- Revisit synced audio rejoin after automatic video-only fallback only if a real report shows audio appears late and needs to become synced automatically.
- Consider making CLI plaintext mode require `--allow-plaintext` after the encrypted/invite UX is smooth enough to avoid surprising users.
- Add a richer debug overlay if title telemetry becomes too dense.
- Detect likely silent system-audio capture, such as Opus packets staying near 3 bytes/packet, and warn that the sender may be capturing the wrong output device even when transport is healthy.
