# Room v2 wire and ownership reference

Status: Checkpoint A contract foundation. `PLAN.md` remains authoritative. The
command validators and subscription ordering tests exist; the running application
and deployed Worker still use v1. Server event decoding, authenticated dispatch,
HTTP admission and Durable Object runtime tests remain Checkpoint C work.

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

## Server output and admission (implementation contract; not implemented)

Keep server decoders distinct from client-command validation. Server-only sender
identity must never become a permitted client key. Add shared fixtures before
wiring these output types into the client:

- `state.snapshot`: v, type, revision, payload; add roomId for room state.
  Room payload contains current policy and authoritative member roster, including
  own peer identity and roles. Directory payload contains safe summaries only.
- `state.delta`: same envelope, a typed change payload. Publish only actual state
  changes; room and directory each have their own revision sequence. Specify
  exact change payload schemas with snapshot schemas during server implementation.
- Command acknowledgements carry requestId and typed success/conflict/error;
  they must not advance the state revision independently of a state mutation.
- Relayed signals carry authenticated `fromPeerId`, roomId, connectionId and
  target. They carry no state revision. Only host/viewer pairs may exchange them;
  the host offers and the viewer answers/requests restart.

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

`tests/fixtures/room-v2/commands.json` is executed by the Qt C++ test and Node test.
Fixtures may supply a message, exact raw string, hex bytes, padding, or a compact
payload-field repeat descriptor for byte-boundary cases. `revisions.json` is an
ordered trace: event, callback generation, incoming revision, expected decision,
expected accepted revision. Start/stop entries omit the last two fields.

Run `npm run typecheck` and `npm test` in signaling-worker (Node 24 supports the
tests' native TypeScript stripping). Native application CTest includes
`room-v2-protocol` when Qt/UI tests are enabled. These are pure boundary tests,
not Cloudflare runtime, authorization, networking or end-to-end latency tests.
