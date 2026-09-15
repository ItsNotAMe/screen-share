# Room v2 wire and ownership reference

Status: Checkpoint A contract foundation. `PLAN.md` remains authoritative. The
command validators and subscription ordering tests exist; the running application
and deployed Worker still use v1. Server event validation and native atomic state caches are also implemented.
The native Qt WebSocket transport now has real local I/O tests; see
[CHECKPOINT-C.md](CHECKPOINT-C.md). Server authenticated dispatch, HTTP handlers,
application/media integration remain open. An isolated v2 Worker now implements
admission, room attachment/resync and membership alarms, with local Durable Object
runtime tests; directory and authenticated command dispatch remain unfinished.

## Ownership and threading

The session coordinator owns session/viewer generations and cancellation on one
control executor. It consumes immutable room snapshots and directs one WebRTC
PeerConnection per viewer. It does not parse JSON or run codec loops.

The room transport owns Qt Network/WebSockets on a dedicated networking event
loop. Wire decoding and revision tracking run there; validated immutable results
are queued to the coordinator. Stop disconnects callbacks and invalidates the
subscription generation before closing sockets. An already queued callback carries
its original generation and cannot update the replacement subscription.

Windows capture/audio, codecs and presentation remain separate from room transport.
Input goes over encrypted WebRTC data channels, not the Worker or status timer.
Video uses bounded latest-frame queues and WebRTC congestion control; manual
resolution is fixed and manual bitrate is a ceiling. This separation is essential
to the gaming latency targets, which still require external measurement.

## Client commands (implemented validation)

All messages are UTF-8 JSON objects with exact keys `v:2`, `type`, and `payload`.
Room sockets also require `roomId`. Directory sockets permit only `state.resync`
without `roomId`. Unknown fields and types are rejected, including client-supplied
`fromPeerId`, `revision`, tokens and role fields. Identifiers use 1–128 ASCII
letters, digits, underscores or hyphens. Revisions are integers 0–9007199254740991.

| Type | Additional envelope keys | Exact payload |
|---|---|---|
| profile.update | requestId | nickname, expectedRevision |
| room.update | requestId | expectedRevision and at least one of name, visibility, viewerLimit |
| peer.disconnect | requestId | peerId |
| peer.leave | requestId | empty object |
| signal.offer / signal.answer | connectionId, toPeerId | sdp |
| signal.candidate | connectionId, toPeerId | candidate, sdpMid, sdpMLineIndex; optional usernameFragment |
| signal.restart_request | connectionId, toPeerId | empty object |
| state.resync | none | empty object |

Room visibility is `public` or `unlisted`; viewerLimit is 1–63. Lowering the
limit does not evict existing members. Password rotation is not a room update.

Names are NFC-normalized and trimmed using the explicit Unicode White_Space set
in both implementations. After trimming, reject C0/C1 controls, Arabic letter
mark, left/right direction marks, bidi overrides/embeddings/isolates and BOM.
Unpaired UTF-16 surrogates are invalid. Nicknames require 1–32 code points and
at most 128 UTF-8 bytes; room names use 1–64 and 256 respectively. Raw names
are bounded at 1024 bytes. Surrounding whitespace, including tabs/newlines,
is removed before control rejection. Emoji/ZWJ, duplicate names and markup-like
text are allowed; the UI must render names as plain text. Names never authorize.

Reject frames over 64 KiB before parsing; non-signaling commands also have a
16 KiB raw-frame limit. SDP is nonempty, at most 60 KiB, without NUL. Candidate
strings are at most 4096 bytes; empty means end-of-candidates. sdpMid is null
or 1–64 bytes; sdpMLineIndex is null or 0–31; usernameFragment is absent, null
or 1–256 bytes. All candidate text fields reject NUL. Schema validation does not establish valid SDP/ICE semantics.
Errors are `too_large`, `invalid_json`, `invalid_envelope`, `invalid_payload`;
never echo submitted secrets or SDP in errors/logs. Duplicate JSON keys follow
the parsers' last-value behavior; this boundary does not claim to reject them.

## Server output (implemented validation)

Server event validation is a separate entry point; it never permits sender identity
in a client command. All events require exact `v:2`, `type`, `payload` keys. Room
scope adds `roomId`; directory scope forbids it. Only state snapshot/delta events
carry an envelope revision. Names arriving from the server must already be
canonical (NFC, trimmed and validated); the client rejects rather than repairs them.

| Type | Additional envelope keys | Exact payload |
|---|---|---|
| state.snapshot (room) | revision | selfPeerId, policy, status, members |
| state.snapshot (directory) | revision | rooms |
| state.delta (room) | revision | One room change below |
| state.delta (directory) | revision | op: upsert + room, or op: remove + roomId |
| command.result | requestId | status: ok; or status: conflict + currentRevision; or status: error + code |
| room.closed | none | reason |
| signal.offer/answer/candidate/restart_request | connectionId, fromPeerId, toPeerId | Same payload as matching client command |

Nested objects have exact keys:

- Policy: `name`, `visibility` (public/unlisted), `viewerLimit` (1–63),
  `passwordProtected` (boolean).
- Member: `peerId`, `nickname`, `role` (host/viewer), `status`
  (connected/reconnecting). A room snapshot has 1–64 unique members, exactly one
  host and a selfPeerId present in the roster. Room status is open/reconnecting;
  it must agree with the host's connected/reconnecting status. Occupancy may exceed
  a lowered viewer limit without evicting existing members.
- Directory room summary: `roomId`, `name`, `viewerCount` (0–63), `viewerLimit`
  (1–63), `passwordProtected`, `status` (open/full/reconnecting), `summaryVersion`,
  `leaseExpiresAt` (Unix epoch milliseconds). Both version and expiry are safe
  nonnegative integers. Unless reconnecting, full means count >= limit and open
  means count < limit. A snapshot has at most 500 unique rooms.

Room deltas each carry one atomic operation:

- `{op: "policy", policy}` replaces the complete policy.
- `{op: "member.upsert", member}` adds/replaces one member, preserving existing role.
- `{op: "member.remove", peerId}` removes a viewer other than the receiver itself.
- `{op: "host.status", status}` changes room status and host member status together.

A host departure closes subscriptions; it never elects another host. Send
`room.closed` to the departing/kicked viewer rather than removing its own identity
from its roster. Closed reasons are host_left, host_expired, kicked, server_shutdown.
Result error codes are forbidden, not_found, invalid_state, rate_limited,
invalid_command. Results and signals never consume state revisions. Signals must
have different sender and target IDs; authenticated target/role/generation checks
still belong to the dispatcher.

Directory snapshots permit 256 KiB; other metadata events permit 16 KiB; signals
permit 64 KiB. Enforce serialized output bounds before publication/admission, not
only when decoding. Use 128-bit random base64url room/peer IDs (22 characters) for
server-generated identities so maximum rosters fit the metadata budget. The wire
identifier ceiling remains 128 characters for compatibility with request and
connection IDs. Do not broadcast credentials, addresses or rosters in summaries.

## Native state application (implemented pure cache)

`StateSubscription` binds to one subscription generation and, for rooms, expected
room/self identity. It rejects old callbacks before parsing. A validated snapshot
replaces state only after ordering and identity checks. Each delta is applied to
an isolated copy; the resulting snapshot must pass all schema, roster and size
invariants before both payload and revision commit. Consumers receive a Qt value
copy, not a mutable reference to accepted state.

Missing removals, role changes, invalid host/self removal, capacity overflow and
non-increasing summary versions produce one resync and preserve the last accepted
state. Later deltas are suppressed until a valid snapshot arrives. Host status is
updated atomically with room status. Close/stop clears state and invalidates old
callbacks. A malformed current event returns Invalid without changing state; the
future transport must close/recover that socket instead of repeatedly processing
bad events. Signals/results return Ignore from this state-only cache and must be
routed separately after target/connection-generation validation.

## Admission (native client implemented; service pending)

Native `RoomAdmission` executes one request at a time on networking and returns
a typed future. The contract for the new service is:

- Create: `POST /v2/rooms`, exact body `{v:2,nickname,password,policy}`;
  policy has `name`, `visibility`, `viewerLimit`.
- Join: `POST /v2/rooms/:id/join`, exact body `{v:2,nickname,password}`.
- Names use the same canonicalization as socket commands. Password is an exact
  unnormalized UTF-8 string, empty for no password, maximum 128 bytes, without
  ASCII control characters/DEL or invalid Unicode. This preserves the existing
  native byte ceiling; the v2 service must use the same UTF-8 bound.
- Success is HTTP 201 for create or 200 for join, JSON with exact keys
  `{v:2,roomId,peerId,role,token}`. Role is host for create/viewer for join.
  Token is canonical unpadded base64url of 32 cryptographically random bytes
  (43 characters). The client validates shape, not entropy. Join binds roomId.
- Defined rejections have exact `{v:2,error}`: 400 invalid_request, 403 forbidden,
  404 not_found, 409 full/closed, 429 rate_limited. Do not echo secrets.
- Responses require application/json, are capped at 16 KiB, and expire after
  10 seconds. Redirects are never followed. Cookie load/save, authentication
  reuse and HTTP caching are disabled. Remote origins require HTTPS.
- Invalid/ambiguous responses, unknown failures, 5xx and timeouts are Unconfirmed.
  Cancellation is also marked outcomeUnconfirmed. Neither is automatically
  retried by application code; aborting does not undo a server transaction.
- Success produces a RoomSocket config on the same origin with HTTPS→WSS,
  bound room/peer/token and expectedRole. Socket snapshots cannot silently change
  that role. Callers must discard stale admission results and attach promptly.

See [CHECKPOINT-C.md](CHECKPOINT-C.md) for native HTTP test evidence. This is not
yet wired to a v2 Durable Object implementation or the normal UI/CLI workflow.

POST create/join returns server-generated peer ID and 256-bit membership token
over HTTPS. Store only the token hash server-side, retain the token only in client
memory and use authorization headers for socket attachment. Provisional membership
expires after 30 seconds if unattached; publish only after host attachment. Never
automatically retry ambiguous create/join requests. Validate membership, role,
current socket generation, target and connection generation before every action.
Deduplicate mutation request IDs in a bounded cache per active socket. Expected
revision conflicts fail rather than overwriting newer state. Apply per-socket
command limits and at most 64 candidates per generation, plus bounded candidate
queues; byte validators alone do not prevent floods.

## Subscription ordering (implemented pure policy)

Use independent `RevisionTracker` instances for room and directory. Every start
or stop advances the local callback generation; start clears state while awaiting
the server's authoritative snapshot. Reject old generations even if their revision
is newer. Apply only consecutive deltas. Ignore old/duplicate deltas. A gap (or
delta before snapshot) emits one resync decision, then suppresses deltas until
an authoritative snapshot arrives. Reject a snapshot below the accepted revision
within the same generation; an equal snapshot clears a pending gap. A reconnect
may accept any authoritative snapshot revision in its new generation.

The transport must serialize send and tracking decisions and reconnect if a resync
cannot be sent or the connection times out. Do not retry resync on every delta.
No tracker operation performs I/O. Close the directory subscription when hidden;
explicit refresh may GET a snapshot, but there is no periodic HTTP fallback.

## Low-request service design

The native transport's exact auto-response pair is `v2:ping` / `v2:pong`.
Configure room and directory objects with this pair. Pings occur at 29–31 seconds;
handshake/snapshot/resync deadlines are 10 seconds. Outgoing Qt buffered bytes are
capped at 256 KiB; pressure causes reconnect. These are implementation defaults,
not service-cost or latency claims.

Use hibernating sockets with fixed automatic ping/pong responses (30-second ping,
10-second timeout), 1/2/4/8/16/30-second reconnect backoff with jitter, and a room
alarm every 30 seconds checking 90-second membership expiry. Old socket closure
must not remove a replacement. Unexpected host disconnect blocks new joins while
reconnecting; expiry closes the room without host election.

Directory summaries contain room ID/name, occupancy/limit, password-required flag,
joinability/status, summary version and lease expiry. Never include member rosters,
tokens, SDP or addresses. Publish only public rooms; renew a 180-second lease every
60 seconds without broadcasting lease-only changes. Listing reads the directory
only, with zero per-room verification. Persist pending summary/removal versions
transactionally in the room and retry cross-object failure from alarms. Keep
closure tombstone work until removal succeeds; lease expiry is the last fallback.
Admission/reservation failure must fail closed. Measure actual free-tier usage in
the deployed runtime before claiming the plan's request/storage headroom.

## Validation

`tests/fixtures/room-v2/commands.json` and `events.json` are executed by the Qt
C++ test and Node test. Native `state-cache.json` traces assert complete payload
and accepted revision after every event, including failed transitions.
Fixtures may supply a message, exact raw string, hex bytes, padding, or a compact
payload-field repeat descriptor for byte-boundary cases. `revisions.json` is an
ordered trace: event, callback generation, incoming revision, expected decision,
expected accepted revision. Start/stop entries omit the last two fields.

Run `npm run typecheck` and `npm test` in signaling-worker (Node 24 supports the
tests' native TypeScript stripping). Native application CTest includes
`room-v2-protocol` when Qt/UI tests are enabled. These are pure boundary tests,
not Cloudflare runtime, authorization, networking or end-to-end latency tests.
