# TODO

## Ordered Roadmap

1. Add a KV-backed active room directory for browseable room summaries while keeping live signaling state in Durable Objects. Include a safe `GET /rooms` endpoint for future UI room-list viewing.
2. Validate NAT multi-viewer rooms with real computers using the signaling flow. Follow-ups after testing: per-viewer connection/health display, optional per-viewer bandwidth policy, and better fallback UX for watchers whose NAT still needs direct invite/manual help.
3. Build the stage-2 native UI after the core share/watch/session flow settles. Keep it modern, simple, dark-mode friendly, and integrated into the program itself rather than just a launcher shell. Favor a room model: auto-updating available rooms/devices list, locked-room indicators, optional password prompt on join, simple host/join flows, window/screen selection, audio window/source selection, and clearer in-session state.
4. Add better user-facing diagnostics for remaining common setup mistakes and sync/network states once reports show the need.

## Live-Streaming Hardening

Only pick these up when reports show the need; normal audio and no-audio fallback are currently working in user testing.

- Include receiver A/V sync/playout fields in receiver feedback summaries so sender reports can diagnose title values like `av -600ms` without separately collecting the receiver log.
- Investigate small-drift audio time-stretching only if A/V catch-up drops become audible or too frequent; keep hard drops for large real-time recovery.
- Improve the title text if users confuse diagnostic A/V estimates with true audible drift.
- Make sender CLI/report NAT status freshness-aware, so `nat_status=connected` does not stay sticky forever after the last receiver feedback packet.
- Make `--stream-encoder auto` smarter by detecting hardware encoder input drops and falling back to software or marking that hardware encoder as unhealthy.
- Measure and report max encoder input queue age in milliseconds.
- Prefer dropping stale queued encoder input before it becomes visible latency.
- Revisit UDP pacing if future reports show backlog with fixed 125% pacing headroom; make headroom adaptive instead of fixed if needed.
- Avoid arbitrary UDP queue media drops inside a GOP unless paired with a keyframe strategy.
- Consider keyframe request/force after sender skips frames.

## NAT Traversal Follow-Ups

- Keep media relay/TURN out of the main path unless direct STUN/signaling/manual invite/hole punching proves insufficient.
- Re-test cross-LAN Worker rooms after the static-candidate fanout fix; if `direct_udp_blocked` still appears, compare whether sender is transmitting to the peer public endpoint before deciding on fallback UX.
- Re-test opening a second fresh Worker room after deploying the Durable Object signaling state change; the 2026-05-22 report showed Watch saw Share while Share stayed at zero targets, consistent with KV eventual-consistency races.
- If repeated reports still show true `direct_udp_blocked`, decide on the fallback UX: recommend Tailscale/mesh VPN clearly, or add an optional relay/TURN path for networks that refuse UDP hole punching.
- Consider optional UPnP/NAT-PMP port mapping after the two-sided invite flow if users still hit blocked paths.

## Conditional / Deferred

- Revisit synced audio rejoin after automatic video-only fallback only if a real report shows audio appears late and needs to become synced automatically.
- Consider making CLI plaintext mode require `--allow-plaintext` after the encrypted/invite UX is smooth enough to avoid surprising users.
- Add a richer debug overlay if title telemetry becomes too dense.
- Detect likely silent system-audio capture, such as Opus packets staying near 3 bytes/packet, and warn that the sender may be capturing the wrong output device even when transport is healthy.
