# Checkpoint C evidence

## Authorized room mutations — 2026-09-15

Room sockets now execute profile.update, room.update, peer.disconnect and peer.leave
through a separate pure authorization/decision module. Identity comes only from
the active socket generation. Viewers can edit their own nickname or leave; only
hosts edit policy or kick viewers. A host leaving closes the room without election.
Lowering viewerLimit preserves existing viewers and blocks further admission.
Removed members immediately lose their stored token hash and cannot reattach.

Profile/policy changes check expectedRevision. Mutation state and its result cache
commit before acknowledgement/delta delivery. The last 32 request IDs, canonical
command digests and results are persisted per socket generation: identical retries
return the original result without another mutation; conflicting reuse rejects.
Replacement starts a fresh cache. This is bounded deduplication, not indefinite
exactly-once delivery. A lost acknowledgement after removal still requires the
client's existing unconfirmed-outcome handling.

Socket attachments enforce 120 application messages per fixed minute; overflow
closes that socket, without a cross-object rate check. Automatic ping/pong bypasses
application dispatch. This preliminary metadata budget will need a separate
signaling budget when signaling is enabled. Save now preserves an earlier alarm
deadline so frequent mutations cannot postpone expiry/capacity renewal. Closure
tombstones retain their reason for retry.

Typecheck and the 192-test Worker suite pass, including expanded actual workerd
coverage for unauthorized edits/kicks, normalized profile updates, duplicate and
conflicting request IDs, stale revisions, live policy edits, lower-limit behavior,
kick/leave token invalidation, host closure and a resync flood. Evidence:
`build/webrtc/v2-mutations-tests.log`. Directory publication, targeted signaling,
native/media integration, hibernation reconstruction and service cost/latency
acceptance remain unfinished. No deployment or application cutover occurred.

## Isolated Worker admission and socket membership — 2026-09-15

`signaling-worker/wrangler.v2.toml` now selects a separate v2 entry point and
SQLite room/control objects. The default v1 entry/config and normal application
remain unchanged; nothing was deployed. This is a service prerequisite, not a
completed v2 service or Checkpoint C.

Implemented HTTPS create/join, health and authenticated room-event attachment.
Admission consumes at most 16 KiB before acquiring the room mutation gate, checks
exact fields, normalizes names and bounds exact password UTF-8 to 128 bytes.
Tokens are 32 random bytes; storage contains only SHA-256 token hashes. Passwords
use independent random salts and versioned PBKDF2-SHA256 at 100,000 iterations.
Provisional membership lasts 30 seconds and counts against capacity. Concurrent
password verification/admission is serialized, so joins cannot overbook a room.

Room sockets use hibernation attachments with peer identity and increasing socket
generation. Replacement closes old sockets; their later close cannot mutate the
replacement. The first event is an identity-bound snapshot. Resync is supported.
The exact automatic ping/pong pair is configured, and alarms consult automatic
response timestamps for 90-second membership expiry. Host disconnect broadcasts
reconnecting and blocks joins; reconnect preserves membership. Host expiry closes
the room and releases capacity, retaining a closure tombstone if release fails.
No SDP or candidate history is stored. Other commands currently fail explicitly;
signaling is not forwarded without its future authorization/generation dispatcher.

The required control binding enforces 240 recognized HTTP requests/minute/IP,
10 creates/minute/IP, 30 joins/password attempts/minute/IP, and an authoritative
500-room reservation cap. IP keys are hashed. Reservations have 180-second leases,
currently renewed by each 30-second room alarm. Closed-by-default Origin checks,
no-store JSON responses and rejection of URL queries apply. Directory publication,
60-second summary renewal, configurable caps and full CORS/preflight support remain
unfinished. This development configuration is not ready for production cutover.

`npm run typecheck` and `npm test` in signaling-worker validate the service.
Final typecheck passed and tests passed 192/192; evidence is
`build/webrtc/v2-service-tests.log`. Wrangler's v2-config dry-run bundle also passed
without deployment (`build/webrtc/v2-worker-bundle`, `v2-worker-dry-run.log`).
The test bundles the production entry and runs actual workerd/SQLite Durable
Objects through Miniflare, with real HTTP and WebSocket traffic. Test-only storage
inspection and expiry injection are added by an in-memory test entry, never the
production bundle. Coverage includes hashed-only credential storage, normalization,
wrong passwords/tokens, eight simultaneous joins for one slot, provisional expiry
and readmission, abandoned-host cleanup, authenticated replacement, first snapshot,
auto pong, resync, reconnecting/recovery, closed Origin policy, query-secret rejection,
admission throttling, and 501 concurrent reservations against the 500-room cap.

Expiry tests inject persisted deadlines and call the real alarm handler; they do
not prove real alarm scheduling, hibernation eviction/reconstruction, cross-colo
behavior or free-tier cost. Directory updates, mutations/deduplication, targeted
signaling, per-socket rate/queue limits, native-to-Worker end-to-end tests and media
adoption remain open. Existing resource and gaming latency acceptance is unchanged.

## Native create/join admission - 2026-09-15

RoomAdmission now shares the networking library with RoomSocket. It returns typed
futures, permits one in-flight request, canonicalizes names through the existing
validators and validates returned identity, role and 256-bit token encoding.
Successful results directly provide a socket configuration on the same origin.
RoomSocket now requires expectedRole for room sockets and rejects conflicting snapshots;
admission always supplies this binding.

Create/join requests use HTTPS bodies. No redirects, cookie reuse/storage or HTTP
caching are permitted. Responses are limited to 16 KiB and a ten-second deadline.
Known structured rejections return typed errors. Cancellation, malformed or
mismatched responses, unknown errors and timeouts are explicitly unconfirmed;
application code never retries them automatically. A cancelled request may have
committed server-side. Provisional expiry remains a service responsibility.

The exact request/response contract is now recorded in ROOM-PROTOCOL.md. Native
tests use a real local HTTP server and exercise create/join normalization, identity
and role binding, invalid tokens, exact-key validation, typed rejection, redirects,
oversized responses, cookie suppression, busy admission, cancellation/replacement,
and the actual timeout. Live socket tests also reject a role-conflicting snapshot.
Submitted passwords and raw server error bodies are not emitted in errors/logs;
successful membership credentials are explicit in-memory values for attachment.

Final evidence: `build/webrtc/admission-verified-sdk-app-{debug,release}.log`,
`admission-verified-sdk-room-cli-release.log`, and
`admission-verified-headless-release/result.json`. Earlier optional-role runs
are retained under `admission-final-*` and `admission-headless-release`.
Final application suites passed 12/12 each in Debug/Release, CLI-only Release
passed 7/7, and the combined Release admission/socket/media headless run passed
14/14. All test processes completed within their watchdogs.
The headless runner's room option now runs admission plus socket tests. Service
handlers, membership expiry, permission enforcement and media connection-generation
dispatch remain open, along with normal application adoption and remote TLS tests.

## Native WebSocket transport - 2026-09-15

ScreenShareRoom shares the existing validators/cache and the new private Qt
RoomSocket across backend builds, independently of Widgets. The normal app still
uses v1. This is a transport prerequisite for B integration, not completion of B
or C. Admission, the v2 Worker and media connection-ID dispatch remain open.

RoomSocket lives on the networking event loop; tests run it on a separate QThread.
It accepts externally obtained membership data and attaches the token only through
Authorization. Remote origins require wss, with Qt certificate/hostname validation
retained. Userinfo, queries, fragments, arbitrary origin paths and token control
characters are rejected. Plaintext requires explicit diagnostic opt-in and numeric
loopback. Actual remote TLS/backend-plugin integration still needs validation.

Behavior and embedding contract:

- Handshake is not readiness: require an identity-bound authoritative snapshot.
  Independent room/directory revisions use the existing atomic cache. One gap
  sends one resync; later deltas are suppressed until the snapshot.
- Check schemas, room/self target, known membership and host/viewer direction.
  Viewer offers and unknown targets are rejected. These client checks do not
  replace server token, role, socket and target authorization.
- Callbacks carry increasing local transport generations; old socket callbacks
  are rejected. Queue value copies to consumers and discard older generations.
  These are not media connection IDs. The media adapter still needs connection/
  negotiation generation and candidate-budget checks. Never log signal values.
- Limit incoming frames/messages to 64 KiB for rooms and 256 KiB for directory,
  then apply stricter schema-specific limits. Cap outgoing Qt buffered bytes at
  256 KiB. Pressure drops/reconnects rather than adding a retry queue. Send returns
  typed results; native error strings and submitted credentials are not echoed.
- Exact automatic heartbeat pair: `v2:ping` / `v2:pong`. Send after 29–31 seconds;
  missing pong expires after 10 seconds. Connection/snapshot/resync deadlines are
  also 10 seconds. Resync cancels heartbeat waiting so a late pong cannot cancel
  its deadline. No periodic HTTP polling exists in this component.
- Reconnect bases are 1/2/4/8/16/30 seconds, jittered to 80–100%, capped at 30.
  An accepted snapshot resets backoff. Stop/room.closed cancel timers and clear
  membership configuration. Credentials are held in memory, not persisted.
- Callbacks must not throw, reenter or destroy RoomSocket. Stop/destroy it on
  networking before joining that thread. Application queue ownership remains an
  integration responsibility; this component does not create an invocation queue.

The live local QWebSocket server test covers headers, snapshot gating, normalized
profile writes, direction/target rejection, pushed signals/state, one-shot resync,
actual heartbeat timeout and authenticated reconnect, directory lifecycle, stop/
stale delivery and saturated writes. It requires no input, Widgets or deployed
service. CTest/headless runner enforce a 75-second process watchdog.

Final evidence: `build/webrtc/room-socket-final-sdk-app-{debug,release}.log`,
`room-socket-final-sdk-room-cli-release.log`, and
`room-socket-final-headless-release/result.json`. Initial runs without the write
saturation test remain under `room-socket-*` without `final`.
Final Debug and Release application suites passed 11/11 each; the fresh Release
CLI-only build passed 6/6. Combined Release headless media/room smoke passed
13/13, recording executable hashes, timing, watchdog results and JSON metrics.

Next: admission/membership lifecycle, local Cloudflare runtime tests, and mapping
validated room values into the media owner. These tests do not establish service
authorization, free-tier headroom, NAT success or gaming latency.
