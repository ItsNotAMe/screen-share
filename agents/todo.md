# TODO

## Your Validation

Only you can fully validate these because they need real machines/networks.

- Worker room validation already tested: open room across two LANs, and locked/password room locally.
- Remaining Worker room validation: locked/password room across two LANs, watcher joins an idle room before Share resumes, sharer reconnect, and two or more watchers with WebSocket peer notifications plus HTTP fallback.
- Mid-session stream settings validation: while sharing, switch Resolution between Auto and explicit tiers at or below the display size and confirm Watch keeps receiving after each restart.

## Build Work

1. Build the stage-2 native UI after the core share/watch/session flow settles. Keep it modern, simple, dark-mode friendly, and integrated into the program itself rather than just a launcher shell. Favor a room model: available rooms/devices list, simple host/join flows, window/screen selection, audio window/source selection, and clearer in-session state.
2. After the revamped UI, add application sharing: capture a selected application's video and matching audio instead of the whole display/system mix.
3. After the revamped UI, add a host-side mute control so the sharer can mute outgoing audio during a live session.
4. Add better user-facing diagnostics for remaining common setup mistakes and sync/network states once reports show the need.

## Completed Build Work

- Added per-viewer connection/health display for multi-viewer Share sessions.
- Added mid-session Share resolution changes. Resolution now has Auto plus explicit tiers at or below the selected display size; the UI sends runtime commands and the sender restarts capture/encoding without closing the room.
- Added sharper GPU capture resizing and light receiver-side upscale sharpening for desktop text.

## Report-Driven Follow-Ups

Only pick these up when real reports show the need; normal audio, no-audio fallback, freezes, and current Worker rooms are currently behaving well in user testing.

- Include receiver A/V sync/playout fields in receiver feedback summaries so sender reports can diagnose title values like `av -600ms` without separately collecting the receiver log.
- Make sender CLI/report NAT status freshness-aware, so `nat_status=connected` does not stay sticky forever after the last receiver feedback packet.
- Detect likely silent system-audio capture, such as Opus packets staying near 3 bytes/packet, and warn that the sender may be capturing the wrong output device even when transport is healthy.
- Investigate small-drift audio time-stretching only if A/V catch-up drops become audible or too frequent; keep hard drops for large real-time recovery.
- Improve the title text if users confuse diagnostic A/V estimates with true audible drift.
- Make `--stream-encoder auto` smarter by detecting hardware encoder input drops and falling back to software or marking that hardware encoder as unhealthy.
- Measure and report max encoder input queue age in milliseconds.
- Prefer dropping stale queued encoder input before it becomes visible latency.
- Revisit UDP pacing if future reports show backlog with fixed 125% pacing headroom; make headroom adaptive instead of fixed if needed.
- Avoid arbitrary UDP queue media drops inside a GOP unless paired with a keyframe strategy.
- Consider keyframe request/force after sender skips frames.

## Conditional / Deferred

- Revisit synced audio rejoin after automatic video-only fallback only if a real report shows audio appears late and needs to become synced automatically.
- Consider making CLI plaintext mode require `--allow-plaintext` after the encrypted/invite UX is smooth enough to avoid surprising users.
- Add a richer debug overlay if title telemetry becomes too dense.
- Consider optional UPnP/NAT-PMP port mapping only if repeated real reports show direct Worker/STUN rooms failing on otherwise important networks.
