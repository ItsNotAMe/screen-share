# Checkpoint C evidence

## Compiled native clients against workerd — 2026-09-15

RoomServiceTests now exercises the production Qt RoomAdmission and RoomSocket
against the actual isolated Worker/SQLite Durable Objects. The one-command Node
harness starts a numeric-loopback service, launches the compiled native process
with a 60-second watchdog, disposes the runtime and saves unique hashed/timed
artifacts. CMake registers the test when Node/Miniflare are installed, independently
of Widgets; missing prerequisites are reported during configuration.

The combined path covers directory subscription/resync, create/join with names and
passwords, capacity rejection, member updates, profile edits and stale-revision
conflicts, directed offer/answer/candidate/restart-request relay, host disconnect and
reattachment, blocked joins during disconnection, public/unlisted visibility, kick
and host closure. Tests require no mouse/keyboard or physical capture/audio device.
Integration exposed that snapshot events lacked the accepted revision: it now
travels with the immutable payload/generation for safe expectedRevision edits.

Application suites passed 13/13 each in Debug and Release, and CLI-only Release
passed 8/8. After extending reconnect/resync coverage, the final native-service
target was rebuilt and passed again in all three configurations. Worker typecheck
and 196/196 tests also passed. Evidence:

- `build/webrtc/native-worker-app-{debug,release}.log`
- `build/webrtc/native-worker-cli-release.log`
- `build/webrtc/native-worker-final-sdk-{app-debug,app-release,room-cli-release}.log`
- `build/webrtc/native-worker-worker-tests.log`
- Each build's `room-service-evidence/native-service-*/result.json` and `native.log`

The harness uses the native clients' explicit plaintext loopback diagnostic mode
and a test-only entry supplying the HTTPS URL/trusted edge IP normally supplied by
Cloudflare. Production transport policy is unchanged. This proves client/service
protocol integration, not remote TLS, deployment or real media negotiation: the
SDP/ICE payloads are synthetic. The shared UI/CLI media facade, native connection-ID
mapping, hibernation/load/queue acceptance, gaming latency and normal cutover remain
open. The old backend still handles normal sessions; nothing was deployed.

## Background directory and capacity delivery — 2026-09-15

Directory publication, capacity renewal and capacity release now run outside the
room input gate and state serializer. Room commands persist their changes/pending
publication and return without awaiting those services. One background delivery
loop per room coalesces wakeups, runs at most four jobs per drain, and leaves failed
or remaining work for the existing persisted alarm or a later mutation. No outgoing
event or publication history queue was added; only the latest pending summary is
retained. A five-second fetch abort bounds each client-side network wait.

State phases use an explicit serializer shared by input handling and background
acknowledgements. Network awaits never hold that serializer. Each acknowledgement
reloads current state and clears only the matching publication version, so a late
reply cannot restore an old roster/policy or erase a newer removal. Closure remains
persisted until directory removal and capacity release acknowledge. A confirmed
missing capacity reservation closes the room; transient failures preserve retry
state. This supersedes the earlier directory-control blocking limitation below.

Actual workerd tests hold a publication open while admission, socket attachment,
resync, policy edits and offer/answer relay complete within a 1.5-second test
watchdog. They then release the older request and verify the latest summary and
roster survive. Additional coverage holds an upsert across host closure, exercises
the real five-second abort and subsequent retry, and verifies bounded drain/single
client-flight behavior plus state-serializer rejection recovery. Test-only expiry
injection now uses the same state serializer; tests await eventual cleanup rather
than assuming remote acknowledgement is synchronous.

Typecheck and 196/196 Worker tests passed; evidence:
`build/webrtc/v2-outbox-tests.log`. These are control-path concurrency/timeout tests,
not gaming-latency or remote service availability measurements. An aborted request
can still execute remotely; versions and tombstones provide convergence after that
ambiguous outcome. Real hibernation reconstruction, queue pressure, free-tier load,
native-to-workerd/media integration and normal application cutover remain open.

## Directory publication and live subscriptions — 2026-09-15

The isolated v2 service now implements all six HTTP/WebSocket endpoints. A separate
SQLite V2Directory object owns directory revisions, explicit HTTP snapshots, initial
WebSocket snapshots, pushed upsert/remove deltas and bounded resync. Listing reads
only that object; there are no per-room verification calls. Public directory routes
reject membership Authorization headers. Event routes require an actual upgrade.
The default v1 service/config and normal UI/CLI remain unchanged; nothing deployed.

Room state now includes a versioned pending directory publication committed with
the room change. Only public rooms with an attached host publish. Counts include
provisional admissions because they reserve viewer capacity; cleanup updates the
summary. Host disconnect/reconnect, name/limit changes, visibility changes and
membership removal update the summary as needed. Nickname-only changes do not
change the directory. No roster, credentials, SDP or peer addresses are published.

Publication retries are idempotent by summary version; same-version conflicting
content rejects and older versions are ignored. Directory rows and their visible
revision commit in one storage transaction before notification. A removal keeps a
short-lived version fence so delayed older updates cannot undo it. Public summaries
renew every 60 seconds with 180-second leases. Lease-only renewal has no visible
revision or delta. Cleanup uses a 60-second alarm and also sweeps before directory
reads. Capacity renewals now occur every 60 seconds instead of every room alarm.

Failed publication remains pending for the room's next alarm. Closure retains a
tombstone until directory removal and capacity release succeed; expired directory
leases are the fallback for an unavailable room. Publication requests have a
five-second abort deadline. They currently execute within room operation handling;
slow/failing directory calls can delay subsequent control traffic, so decoupled
outbox delivery is still required before production responsiveness acceptance.
Media bytes never go through this path. No failure-injection pass establishes
remote deadline enforcement or service availability.

Directory sockets use the automatic heartbeat pair and timestamp-based idle cleanup,
six resyncs per minute, and a 512-subscriber development cap. Runtime outbound queue
pressure and free-tier sizing remain unvalidated. The server keeps no retry queue
of outgoing events; reconnect/resync obtains current state. Room lifecycle deltas
and closure notifications now also follow persistence, fixing the earlier ordering.

Typecheck and 194/194 Worker tests passed. Actual workerd tests cover publication
failure before commit, lost acknowledgement after commit, retry without duplicate
revision, provisional hiding/full/open cleanup, reconnecting/recovery, visibility,
stale resurrection rejection, renewal without revision changes, expired leases,
closure tombstones/retry, no listing fanout, unchanged directory rows after automatic
pong, contiguous pushed revisions, resync/flood handling and 500 maximum-length
summaries within the 256 KiB protocol limit. Expiry/renewal deadlines are injected
through test-only entry points; real scheduling and eviction are not claimed.
Evidence: `build/webrtc/v2-directory-tests.log`.
Wrangler's isolated configuration also passed a dry-run build with all three
Durable Object bindings; artifacts: `build/webrtc/v2-directory-bundle` and
`build/webrtc/v2-directory-dry-run.log`. This did not deploy the service.

Next: decouple directory I/O from room control dispatch, validate native clients
against workerd, then connect the production media/session facade. Real hibernation,
outbound pressure, remote TLS/NAT, cost, comparative latency and resource acceptance
remain open. This is not completion of Checkpoint B/C or the backend cutover.

## Authenticated signaling relay — 2026-09-15

Added a separate signaling authorization policy and actual room-socket dispatch.
The service supplies fromPeerId from the current authenticated socket, restricts
messages to host/viewer pairs, binds exchanges to both socket generations, and
requires fresh connection IDs for offers/restarts. It rejects stale IDs, duplicate
answers, wrong directions and candidates beyond 64 per sender/generation. Recovery
offers have a rolling budget; used-ID digests have a documented lifetime bound.
See ROOM-PROTOCOL.md for exact limits and the native adapter's required mapping.

Signal records use separate per-viewer storage keys and contain only IDs, socket
generations, counters and digests. No SDP/candidate history or retry queue is stored.
Removal/expiry deletes the viewer record, closure deletes all records. Signals do
not alter room revision. Independent metadata/signaling count and byte budgets
live in socket attachments, without cross-object limiter requests per message.

Typecheck and 193/193 Worker tests pass. Expanded actual workerd coverage relays
offers, answers, candidates and restart requests, verifies authoritative sender and
unchanged room revision, rejects a stale candidate after restart, and rejects
viewer-to-viewer offers without forwarding. Pure policy coverage exercises socket
replacement generations, candidate limits, replay, duplicate answers, recovery
budgets and bounded history. Evidence: build/webrtc/v2-signaling-tests.log.

This does not complete Checkpoint C: directory publication/subscriptions, native
media dispatch, real hibernation reconstruction, outbound queue-pressure behavior,
load/free-tier costs, remote NAT and gaming latency remain open. No deployment or
normal application cutover occurred.

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
