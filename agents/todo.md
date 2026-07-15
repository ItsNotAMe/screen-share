# TODO

## Security (pre-public-release audit, 2026-07-15)

Findings from a full security audit ahead of a possible public release. The crypto primitive, TLS validation, packet-parser bounds checks, backend password hashing, and the host-authoritative remote-control model are all sound; the items below are the gaps. Fix the "Must fix" block before shipping publicly. Highest leverage is the crypto key derivation — a per-session random key also removes the cross-session replay leverage behind several other items.

Status: branch `fix/security-hardening` now covers **every** audit finding (all CRITICAL/HIGH plus all MEDIUM/LOW), build-verified (CLI + Qt UI) and, where possible, test-verified: a localhost loopback harness validated the per-session UDP crypto, and openssl-generated vectors validated the update-signature verifier. The remaining MEDIUM/LOW batch (signaling resource limits, NAT-retarget key-proof, injected-input rate limit, O(n²) fragment scan, CORS lockdown, and the remote-control consent surface + keyboard scoping) all landed and build clean. Two items still need the release owner / live environment: (1) update signing — generate/hold the ECDSA-P256 private key, paste the public key into `kUpdatePublicKeyXy`, add the manifest-signing step (see `docs/update-signing.md`); (2) deploy the signaling worker with the new `RATE_LIMITER` DO binding + migration `v3` and validate the cross-colo limiter / room-count cap / directory sweep against a live Cloudflare account. The NAT-retarget hardening is correct-by-construction (retarget only to a key-proven endpoint) but real-NAT rebind testing is still worthwhile before relying on it in the field.

### Must fix before public release

- [x] **CRITICAL — deterministic key + 32-bit GCM nonce → reachable nonce reuse.** _Done: per-session key via HKDF(master, random 16-byte salt) carried in every packet header (PacketVersion 4); the nonce encodes the sub-stream role and a monotonic counter (no random prefix). Key is now unique per session/endpoint, so counter nonces can't collide. Loopback-validated (encrypted video both directions, feedback channel, wrong-key rejection, plaintext)._
- [x] **HIGH — no replay protection on the remote-control channel.** _Done (`fix/security-hardening`): `UdpSender::ProcessControlPacket` tracks the highest accepted control `sequence` per peer, scoped by session fingerprint, and drops non-increasing values. Cross-session replay is fully closed once the per-session key lands._
- [x] **HIGH — plaintext sessions authenticate control by source ip:port only.** _Done: the host control handler and grant path now require an encrypted session (`options.accessCodeKey`); remote control is refused on plaintext sessions._
- [x] **HIGH — access-code fingerprint is a fast unsalted offline oracle.** _Done: access codes are now generated-only (>= 16 bytes enforced; the app generates ~100-bit codes), so the fast fingerprint is no longer a viable offline oracle. The room password is a Worker-side gate only and is no longer mixed into the key. (A UI change to remove any free-text access-code entry is the remaining polish.)_
- [x] **HIGH — signaling backend has no peer authentication.** _Done: server-issued per-peer token on join, required for peers/heartbeat/leave/events and to re-announce/host; candidates + room key returned only to token-holding members; token never leaked in peer lists; legacy unauthenticated KV per-room path retired (503). C++ client threads the token through the live loop; CLI has `--peer-token`._
- [x] **HIGH — memory-exhaustion DoS in UDP reassembly.** _Done: aggregate byte budgets (`maxPendingFrameBytes`/`maxPendingAudioBytes`) enforced in the eviction paths, lower per-frame cap, and the first-fragment allocation is now exception-safe (also closes the latent OOB below)._
- [x] **HIGH — auto-updater has no signature / pinned-key verification.** _Done: ECDSA-P256 manifest signature verified against a pinned public key before an update is offered/installed (Windows CNG has no native Ed25519; same property). Fails closed with no key or a bad signature. Verifier validated with openssl vectors. Release owner must generate the keypair, pin the public key, and sign manifests — see `docs/update-signing.md`._

### Should fix before release

- [x] **Signaling UDP-reflection/DDoS:** _Done: on join, srflx candidates must match `CF-Connecting-IP` (same family) and host candidates must be private-range; offending candidates are filtered out, so a join can't aim other peers' UDP at a victim._
- [x] **Signaling resource exhaustion:** _Done (`fix/security-hardening`): authoritative cross-colo rate limiter via a per-IP `RateLimiterObject` DO (layered behind the existing per-isolate limiter, fails open on limiter errors); global room-count cap (`MAX_ACTIVE_ROOMS`) enforced at room creation through a `/directory/reserve` on the directory DO; alarm-based directory sweep (`DIRECTORY_SWEEP_INTERVAL_MS`) so stale entries are reclaimed without a read; `GET /rooms` served from a short-lived edge cache (`ROOM_LIST_CACHE_TTL_SECONDS`) to collapse the per-room live-summary fan-out. New DO binding + migration `v3` in `wrangler.toml`. (Per-room peer cap was already done.)_
- [x] **NAT retarget trusts cleartext fingerprints:** _Done: on an encrypted session `UdpSender::MaybeRetargetFromNatProbe` now retargets only to an endpoint that has already delivered a validly-decrypting feedback/control packet (`IsVerifiedEndpointLocked`, backed by the post-decrypt `controlPeers_` set), so an unauthenticated spoofed probe can no longer redirect the media stream. A genuine NAT rebind re-registers via the receiver's decrypting feedback first, so it self-heals. Plaintext sessions (no key to prove) keep the fingerprint-only behavior._
- [x] **Keyboard injection is system-wide even when sharing one window.** _Done: `RemoteInputInjector::InjectKey` now early-returns unless target bounds are set, i.e. only injects during a full-display share (the same gate the mouse path uses; bounds are cleared for a window share). Keystrokes can no longer leak to whatever window has host focus during a window share. Window-share remote input still awaits client-rect mapping (window-capture follow-up)._
- [x] **Updater local-tamper windows:** _Done: the interpreter runs from the full System32 `powershell.exe` path, and the helper re-verifies the package SHA-256 (`--sha256`, signature-bound) immediately before extraction, closing the TOCTOU window._
- [x] **Saved reports leak peer ip:port:** _Done: `logs/console.log` is scrubbed of IPv4/bracketed-IPv6 addresses before bundling (ports kept for debugging); the verbatim `--log` file is untouched. Access codes/passwords already did not leak._

### Hardening / lower priority

- [x] **Room password in URL query string** — _Done: the client sends the password only via the `X-ScreenShare-Room-Password` header, and the Worker no longer reads the query-string fallback._
- [x] **Signaling client accepts `http://`** — _Done: `RequireSecureForRoomPassword` rejects a non-HTTPS `--signal-server` whenever a room password is set._
- [x] **Reassembly inconsistent-state latent OOB** — _Done: the first-fragment buffers are built under try/catch and the entry is erased if an allocation throws (both video and audio paths)._
- [x] **Minor DoS/robustness:** _Done: the control `button` field is range-validated before the enum cast; the O(n²) fragment-overlap scan is now O(log n) per fragment (`InsertDisjointFragmentRange` keeps `receivedRanges` sorted and checks only the two neighbors — video + audio paths), so per-frame reassembly is O(n log n); injected input is token-bucket rate-limited (1000/s sustained, 500 burst) at the runtime control handler with throttled `remote_control_input_throttled` telemetry._
- [x] **Signaling misc:** _Done: `metadata.name`/`platform` reject control chars, malformed `decodeURIComponent(roomId)` returns 400, the locked-room existence oracle is closed, and CORS is now allowlist-based and closed by default — no `Access-Control-Allow-Origin` is emitted unless `ALLOWED_ORIGINS` names the origin (native clients send no Origin and are unaffected; `*` must be opted into explicitly)._
- [x] **Remote-control consent surface:** _Done: first-grant-per-session consent/warning dialog before the host grants any control (revert on cancel), a persistent red "<name> is controlling your <mouse/keyboard>" indicator in the active-share top bar, and a global **Ctrl+Alt+Shift+F12** panic-revoke hotkey (registered on the shell window; drops control even under an input flood since the OS delivers WM_HOTKEY on the UI thread). The dialog text states that granting control is effectively full machine access._

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
