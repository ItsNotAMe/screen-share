# Checkpoint B evidence

## Managed room peers and asynchronous capture shutdown — 2026-09-16

RoomManagedPeer connects authenticated room negotiation to HostPeerOwner's
scheduled lifecycle, restart budget and capture-aware retirement. The initial
offer waits for successful asynchronous capture attachment; failed attachment
is handled as a peer operation failure. Restart IDs include the peer incarnation
and restart revision. Room membership removal initiates capture cleanup without
waiting on signaling, and native peer resources remain retained until cleanup
completes.

HostPeerOwner.BeginStop returns a shared completion future. The registry starts
capture shutdown, rejects new peers/restarts/removals while stopping, and polls
completion without blocking signaling. It closes/releases remaining peers only
after capture callbacks have joined. Repeated stop requests share completion;
capture errors are returned rather than leaving the future pending. Synchronous
Stop remains a final-teardown fallback; runtime coordinators must await BeginStop
before destroying the owner.

RoomMediaProof now uses the shared managed adapter for four real room-backed
peers, scheduled ICE restart, socket loss/reconnect and kick/rejoin. Capture
startup waits outside signaling; per-viewer attachment/removal no longer call
future.get on signaling. Normal shutdown also awaits the asynchronous owner.
HostPeerOwnerTest deliberately holds capture delivery open and verifies that
signaling commands still run and peer resources remain retained until release.

Normal UI/CLI facade adoption and automatic cross-executor event dispatch remain
unfinished. This does not establish external latency or capture resource gates.

Validation: full media suites 29/29 in Debug and Release; application 13/13 and
CLI-only 8/8 in Release. Final capture-startup placement is verified by the
room-backed media and scheduled-owner tests in both configurations. Evidence logs:
build/webrtc/managed-room-{debug,release,app,cli}.log and
build/webrtc/managed-room-verified-{debug,release}.log.

## Owned room networking and roster integration — 2026-09-16

RoomNetwork owns a dedicated Qt event loop shared by admission and up to 64 room
sockets. Commands return futures; no caller Qt event pump is required. Normal
commands are bounded to 128 / 512 KiB; events to 256 / 512 KiB. Event overflow
retires all sockets and cancels admission, exposing a terminal backpressure marker
instead of silently dropping SDP/ICE. Stop requests coalesce outside normal queue
capacity; StopAll cancels admission and closes sockets asynchronously. Destructor
cancellation resolves pending admission before joining the thread. Socket handles
increase across attempts/rejoins, preventing old queued events from being applied
to replacement sockets. A terminal overflow owner must be replaced, not restarted.

RoomPeerRoster consumes complete authenticated snapshots on the control/signaling
executor. It validates the full peer set before mutation, ignores old revisions/
generations, isolates failed additions without retrying on unrelated revisions,
and retires peers on membership removal or transport loss. A lost transport
generation cannot revive peers; a newer authoritative snapshot is required.
Lifecycle hooks retain responsibility for native ownership and cleanup barriers.

The real workerd media scenario uses these shared components: room sockets run
off the main thread; host roster changes attach/retire native media, kick/rejoin
is automatic, and room closure removes subscriptions/peers. Socket disconnect
and reconnect exercise replacement incarnations with fresh signaling IDs.
RoomNetworkTest covers admission cancellation through StopAll and destruction,
20 owner lifecycles, coalesced stops, oversized commands, event overflow, and
roster stale-state/failed-peer behavior without pumping a caller event loop.

This does not complete the normal UI/CLI facade. The diagnostic still waits on
command futures and capture teardown; production facade commands must instead
observe completion asynchronously and preserve cleanup/generation barriers.
Latency, resource, remote-network and service-cost gates remain open.

Validation: full Debug/Release media suites passed 29/29; Release application
13/13 and CLI-only 8/8. Final affected tests are `room-network-ownership` and
`room-backed-four-peer-media`; logs live under build/webrtc/network-verified-*.log.
The latter records `owned_network_loop`, `roster_driven_peers` and
`socket_reconnect` alongside existing media/recovery metrics. Test-owned waits
remain distinct from a production asynchronous coordinator.

## Source ownership and autonomous negotiation — 2026-09-16

Native sources now live in backend/ and frontend/; CMake and proof consumers use
their respective include roots. The room Worker stays independently deployable.
This is an ownership cleanup, not a claim that legacy UI/CLI routing has changed.

RoomPeerNegotiation now schedules completion/deadline processing on its signaling
thread while negotiation is active. Ready/idle peers have no recurring timer.
Close, destruction and a new negotiation generation invalidate delayed callbacks.
The real-room proof no longer calls Poll. It additionally destroys a peer before
its first tick and withholds an answer from another peer, checking that its 20s
deadline closes it automatically while established media continues.

Validation after relocating all 161 sources: full Debug and Release media suites
28/28 each; full Debug and Release application suites 13/13 each; Release CLI-only
8/8. Logs are build/webrtc/layout-{proof,app}-{debug,release}.log and
build/webrtc/layout-cli-release.log. All moved file contents match the previous
commit except the two intentional RoomPeerNegotiation implementation/header edits.
UI assets compile and deploy through their unchanged repository-relative paths.

## Authenticated four-viewer media — 2026-09-16

RoomPeerNegotiation now connects native asynchronous SDP operations and bounded
trickle ICE to authenticated room messages. It runs on the signaling executor,
uses fresh wire connection IDs, gates candidates behind descriptions, ignores
retired candidates/answers, and closes failed/timed-out peers. Local credentials
are cleared before creating a new description so retired ICE callbacks cannot be
assigned to the next generation. The transport callback must enqueue without
blocking; higher-level ownership must propagate later socket failures and enforce
the restart budget.

RoomMediaProof creates a real local workerd room through RoomAdmission and runs
one host plus four RoomSockets. Actual H.264 and synthetic Opus travel through
four native PeerConnections; SDP/ICE travel through the Worker. Coverage includes
a slow capture subscriber, twelve encrypted data channels, fresh-ID ICE restart,
stale candidate rejection, duplicate ID rejection, host kick, fresh admission and
media rejoin, and room shutdown. The proof records frames, mixed audible PCM,
candidate count and outbound signaling queue peaks in hashed evidence artifacts.

Debug and Release full media suites passed 28/28; Release application passed
13/13 and CLI-only passed 8/8. See HEADLESS-TESTING.md for the standalone command.
These are synthetic/local correctness results, not desktop capture, independent
per-viewer audio, NAT, input latency, resource acceptance or free-tier cost evidence.

The adapter and room library are shared production components. Diagnostic outer
orchestration still pumps operations and waits; do not copy those waits into UI
commands. The scheduled shared facade, automatic roster/recovery ownership and
normal UI/CLI adoption remain open. Matching service handlers now exist and are
exercised here. Work is grouped by deliverable in TODO.md; detailed original gates
remain in DETAIL-CHECKS.md.

## Scheduled peer ownership and concurrent negotiation - 2026-09-15

HostPeerOwner now binds the capture-aware registry to SignalingExecutor in the
shared application/proof library. A single weak delayed callback runs every
20 ms on signaling, advancing deadlines, restart actions, ready peer operations
and capture cleanup. No external Tick call is required. This is local lifecycle
scheduling, not a network poll or a timer on image/input delivery.

IMediaPeer::Poll consumes ready asynchronous work only. False/throw closes the
affected peer and records operationFailed; other peers keep running. Terminal
lifecycle failure takes precedence over pending work. Closed rows retain their
failure/cleanup status until removed. The registry remains the owner of peers.

The real four-viewer scenario now admits connections in generation order before
waiting for any answers. Its adapter advances local SDP creation, remote apply,
candidate readiness and answer creation through the owner's completion hook.
The same path handles restart and replacement. Restart no longer uses a pending
flag consumed by synchronous test-side SDP exchange, and no peer callback waits
or pumps messages. DescriptionTransfer is a diagnostic local-delivery adapter;
authenticated room delivery still needs a production adapter. Outer diagnostic
waits still pump while observing the scheduled owner.

Embedding requirements:

- Construct, invoke and destroy the owner on its supplied executor. UI/CLI calls
  must enqueue through the executor. It checks thread affinity at its boundary.
- Capture and executor outlive the owner. Stop joins capture before closing peers;
  remove remains asynchronous. Complete owner teardown before executor shutdown.
- Delayed callbacks hold only weak state and are inert after Stop/destruction.
  Do not destroy/reenter the owner from a peer's Poll/Close/restart method.
- A failed lifecycle snapshot can precede scheduled cleanup. peerClosed explicitly
  distinguishes completed native close from a newly observed failure. Wait for
  peerClosed with captureCleanupPending false, or removal's row disappearance,
  before treating cleanup as complete. The first full run caught this assumption
  in the proof; completion now has an explicit snapshot field.
- The 20 ms cadence is an initial control scheduling default, not a gaming
  latency claim. Native/driver hangs still require the process watchdog.

HostPeerOwnerTest uses the actual executor without manual message pumping. It
checks an expired initial connection, scheduled restart, failed-completion
isolation, healthy capture progress, stale requests, joined stop, owner-thread
destruction and 25 recreated owners while weak timers remain queued.

Final evidence: `build/webrtc/owner-verified-sdk-proof-{debug,release}.log`,
`owner-verified-sdk-app-{debug,release}.log`, and
`owner-verified-headless-{debug,release}/result.json`. Earlier runs before the
explicit peerClosed field are retained under `owner-concurrent-*` and
`owner-headless-*` for comparison.
Final Debug/Release media suites passed 27/27 each; both application suites
passed 10/10. Final Debug headless smoke passed 12/12 and Release regression
passed 33/33, including 20 single-viewer and three concurrent four-viewer runs.
The normal UI/CLI backend, authenticated room transport, external latency and
known capture handle-growth acceptance remain open.

## Owned signaling event loop - 2026-09-15

SignalingExecutor is now part of the shared ScreenShareNegotiation library.
Its portable header exposes commands/futures and typed operation results; the
implementation owns a native WebRTC thread. All single-peer, hardware/audio and
four-peer media scenarios run on that executor instead of wrapping main in an
AutoThread. The proof's registry and native peer lifetimes remain inside the
executor task. The normal application facade has not switched backends.

At most 64 commands may wait behind the running command. Capacity, invalid,
closed, cancelled and task-failure results are explicit. Exceptions are contained
without exposing arbitrary exception text. Only one application command is
posted to the native queue at a time, allowing native callbacks between commands
and avoiding an unbounded native queue of application work.

Shutdown/embedding contract:

- Post initiates work; production commands return without waiting on later
  executor commands or native completion. Complete pending negotiation through
  asynchronous owner dispatch, not the diagnostic's nested message waits.
- RequestStop is nonblocking, rejects new commands and cancels pending commands
  after the active command returns. Accepted closures, including cancelled ones,
  are destroyed on signaling before their future completes. Rejected closures
  are destroyed on the submitting thread and must be safe there.
- Stop joins from an external owner and may be repeated/concurrently called.
  Self-join rejects; a task may call RequestStop but may not destroy the executor.
- Close/destroy native peers, negotiation adapters and factories on signaling
  before stopping the executor. Keep SSL/Winsock alive until it joins. Stop does
  not preempt an active native call or implicitly close independently owned peers;
  retain a process watchdog for a hung native/driver call.

The dedicated executor test verifies ordering, current-thread identity, ordinary
native callback progress, exception containment, full-queue rejection, 64 pending
cancellations, owner-thread closure release and shutdown behavior. The
negotiation-only scenario additionally creates two native peers, exchanges and
applies offer/answer SDP across separate commands, waits only from the external
caller, then destroys all native state on signaling. No manual message pumping
is used in that scenario. Existing media diagnostics still use nested waits.

Validation evidence is recorded in `build/webrtc/executor-final-{debug,release}.log`,
`executor-app-{debug,release}.log` and `executor-headless-{debug,release}/result.json`.
Debug and Release media suites passed 26/26 each; application suites passed
10/10 each. Debug headless smoke passed 11/11 and Release regression passed
32/32, including 20 single-viewer and three four-viewer runs. The only later
source edit reformatted the diagnostic dispatch indentation without changing code.
Application ownership/completion dispatch, authenticated room messages, actual
network impairment and UI/CLI cutover remain open. This change makes no new
latency/resource-growth acceptance claim.

## Shared asynchronous SDP negotiation - 2026-09-15

Added PeerNegotiation in the private WebRTC implementation module and the shared
ScreenShareNegotiation CMake target. Application and proof builds compile the
same implementation. The proof's CreatedDescription/AppliedDescription helpers
are removed; single-peer, hardware/Opus and four-peer ICE-restart paths now use
the shared adapter for local offer/answer creation/application and remote SDP.

Operations return futures immediately, carry operation/connection identities
and allow only one in-flight request per adapter. Overlap returns Busy instead
of accumulating a queue. Wrong generations reject before native mutation.
SDP is bounded to 60 KiB and rejects empty/NUL-containing data, matching the
room protocol's payload bound; the room transport must still validate the full
serialized envelope. Native error descriptions are not echoed with credentials.

Local SDP is serialized before gathering, but a successful future is delivered
only after SetLocalDescription succeeds. Local ICE credentials become available
before native candidate callbacks, preserving the existing retired-credential
filter. Creation/application observers hold weak state and operation identities.
Close resolves an outstanding future as Cancelled and rejects further work;
late callbacks cannot apply a cancelled created description or complete a newer
operation. Cancellation does not roll back an already-issued native SDP apply;
the peer owner closes the native connection during lifetime cancellation.

All adapter methods/destruction belong to the peer's signaling executor. There
is no message pumping, blocking wait or internal thread in this component. The
local diagnostic driver still pumps its event loop to observe futures; wiring
the application executor and room transport remains open. This milestone builds
the library into the application dependency graph without switching UI/CLI media.

The dedicated negotiation scenario uses actual PeerConnections: 25 pending
offer cancellations and owner destructions, bounded overlapping requests, stale
remote requests, malformed/oversized/NUL SDP, failed answer creation, successful
replacement negotiation after late callbacks, and remote application on a closed
native connection. Existing encrypted media, restart, settings, failure cleanup
and rejoin checks now run through the same adapter. No private signaling is logged.

Debug/Release media suites pass **25/25** each and application builds/suites
pass **10/10** each. Evidence logs:
`build/webrtc/async-negotiation-sdk-proof-debug.log`,
`build/webrtc/async-negotiation-sdk-proof-release.log`,
`build/webrtc/async-negotiation-sdk-app-debug.log` and
`build/webrtc/async-negotiation-sdk-app-release.log`.
Use the existing run-webrtc-proof helper when CMake regenerates so the pinned
MSVC/Windows SDK environment is initialized. Existing AutoThread warnings remain.

Headless Debug smoke passes **10/10** runs and Release regression **31/31**,
including the dedicated cancellation test, 20 single-peer lifecycles and three
four-peer restart/failure-cleanup/rejoin scenarios. Reports with executable
hashes and watchdog outcomes:
`build/webrtc/async-negotiation-headless-debug/result.json` and
`build/webrtc/async-negotiation-headless-release/result.json`.

## Automatic peer-failure capture cleanup - 2026-09-15

HostPeerRegistry can now bind to HostMediaSession and its host generation.
Terminal peer failure and explicit removal enqueue connection-scoped capture
removal. Tick polls the future without waiting; Capacity/Cancelled results stay
pending and retry on subsequent ticks. None/StaleGeneration completes cleanup,
because a stale host/connection must never remove a newer subscription.

Snapshots expose cleanup-pending and the last cleanup error while preserving
the original peer failure. Remove returning true means accepted; with capture
bound, callers tick until the row disappears before reusing that viewer slot.
Failed rows remain available for diagnosis until explicitly removed. Retained
cleanup futures stay bounded by the registry's 63-peer admission cap.

Full Stop joins bound capture through its priority stop command before closing
and releasing remaining peers. The capture owner must outlive the registry;
declare it first. Stop may report an unexpected capture-stop operation failure;
the owning executor must serialize shutdown and must not race competing stop
commands. Existing process watchdogs remain necessary for native calls. Peer
Close cancels its network callbacks while retained capture source references
remain valid until asynchronous delivery cleanup joins them.

The deterministic owner test blocks one viewer callback, verifies healthy frame
progress, fills the capture command queue, observes Capacity on another removal
and verifies eventual cleanup after release. The real four-peer proof injects
a terminal peer failure, waits for automatic detachment, retains the failure
reason and rejoins with a fresh connection generation.

Application executor scheduling, asynchronous room/WebRTC adapter integration,
UI/CLI replacement and field/network validation remain open. No legacy path is
kept as a permanent alternate backend. Native handle-growth and measured gaming
latency acceptance are unchanged.

Debug and Release media suites pass **24/24** each; logs:
`build/webrtc/peer-cleanup-debug.log` and `build/webrtc/peer-cleanup-release.log`.
Application binaries were not rebuilt: the changed registry header currently
enters the proof path, and normal UI/CLI behavior is unchanged.

Headless Debug smoke passes **9/9** and Release regression passes **30/30**,
including three four-peer terminal-failure cleanup/rejoin runs. Reports with
binary hashes, watchdog outcomes and timing:
`build/webrtc/peer-cleanup-headless-debug/result.json` and
`build/webrtc/peer-cleanup-headless-release/result.json`.

## Connection-scoped capture cleanup - 2026-09-15

Capture subscriptions previously validated only the host-session generation.
A delayed cleanup for a departed connection could therefore remove a replacement
subscription with the same viewer ID. HostMediaSession now requires a connection
generation for AddViewer and RemoveViewer, retains that identity in subscription
snapshots and reports it with subscriber failures. Admission uses strictly
increasing generations within the host session, matching the peer registry;
retired attachments cannot resurrect a subscription after removal. Removing an
absent subscriber remains idempotent, while a mismatched live connection is
rejected before changing delivery.

The coordinator test replaces a subscriber, rejects an old attachment/removal
and waits for new frames. The real four-peer rejoin proof uses actual connection
generations and rejects delayed capture cleanup before checking continued
decoding. This fixes a prerequisite for automatic peer-failure cleanup; the
application executor/room adapter integration remains open.

Validation: Debug and Release media suites pass **24/24** each, and application
builds/suites pass **10/10** each. Logs are
`build/webrtc/connection-guard-sdk-proof-debug.log`,
`build/webrtc/connection-guard-sdk-proof-release.log`,
`build/webrtc/connection-guard-sdk-app-debug.log` and
`build/webrtc/connection-guard-sdk-app-release.log`.

Headless Debug smoke passes **9/9** runs and Release regression passes **30/30**.
Reports (including hashes and watchdog outcomes):
`build/webrtc/connection-guard-headless-debug/result.json` and
`build/webrtc/connection-guard-headless-release/result.json`.

## Host capture/membership coordinator — 2026-09-15

`backend/media/HostMediaSession` now owns CaptureSession and CaptureDistributor on
a serialized control worker. Public futures return operation IDs, session
generations and typed errors; snapshots expose capture state, device generation,
active operation and per-viewer delivery counters. Capture and delivery stay
off the command queue. The portable public interface has no Qt/WebRTC types.

Start accepts an asynchronous source startup; successful command completion
does not guarantee capture is already running. Running means capture with
subscribers, not a connected remote peer. Check snapshots for terminal source
failure. Factory construction and destruction stay on the capture owner thread.

Normal commands have a 64-entry queue limit. A current-generation Stop cancels
queued commands and is accepted despite queue pressure; a stale Stop rejects
without cancelling newer work. Stop joins capture and delivery workers before
returning. Failed subscribers are removed independently. A callback must not
wait synchronously for a command that joins it or destroy its owning session.
Native calls still need the process watchdog; cancellation cannot preempt a
blocked driver call or arbitrary callback.

The four-peer proof now uses this coordinator for capture and subscriber
start/remove/rejoin/stop. Peer construction/signaling, shared audio and settings
application remain in the proof layer. The normal application remains legacy.
This milestone does not complete the production facade or all of Checkpoint B.

### Validation

- Debug and Release native media suites: **21/21 each**, including hardware and
  audio-device targets. Logs: `build/webrtc/coordinator-debug.log` and
  `build/webrtc/coordinator-release.log`.
- Debug and Release application builds and suites: **10/10 each**. Logs:
  `build/webrtc/coordinator-app-debug.log` and
  `build/webrtc/coordinator-app-release.log`.
- Headless Debug smoke: **6/6 child runs**. Artifact:
  `build/webrtc/coordinator-headless-debug/result.json`.
- Headless Release regression: **27/27 child runs**, including 20 single-peer
  process lifecycles and three four-peer scenarios. Artifact:
  `build/webrtc/coordinator-headless-release/result.json`.
- Coordinator test covers 100 session restarts, source owner-thread destruction,
  stale generations, repeated stop, startup failure, isolated failed viewer and
  stop under queue pressure (64 cancelled commands and 16 capacity rejections).

Commands use `scripts/run-webrtc-proof.ps1` with the existing Debug/Release SDK
paths documented in BUILD.md, `-Hardware -AudioDevice` for media and
`-Application` for application builds. Headless commands and limitations are in
[HEADLESS-TESTING.md](HEADLESS-TESTING.md).

### Remaining work

Next integrate peer/signaling ownership, connection generations and asynchronous
settings/events, then expose the complete engine through the shared UI/CLI
facade. Keep the realistic headless path using these same production APIs.
Separate processes, authorized test-owned input and network impairment remain
unimplemented. Existing current-binary native handle-growth failures (+76/+10)
remain open; these synthetic lifecycle tests do not resolve them. See
[CLOSEOUT-A.md](CLOSEOUT-A.md).

No comparative performance improvement is claimed. Matched workloads, external
gaming image/input latency, quality and room-service cost checks are specified
in [COMPARISON.md](COMPARISON.md).

## Trickle ICE candidate handoff — 2026-09-15

Added portable `IceCandidateHandoff`, used by the actual WebRTC media proofs.
Candidates are buffered until local and remote description application succeeds,
then delivered in order without waiting for ICE gathering to complete. The proof
serializes SDP before gathering and asserts it contains no candidate lines.
Thus successful H.264/Opus/data connectivity now depends on separate candidate
handoff, rather than the previous bundled-SDP shortcut.

Each directional handoff has a fresh negotiation identity; stale pushes and
readiness notifications cannot mutate it. Limit each handoff to 64 accepted
candidates over its entire lifetime, with 4096-byte candidate and 64-byte MID
bounds and NUL/index validation. Overflow, malformed data and application failure
close delivery and release queued candidates/callback ownership. Teardown closes
both directions before closing the PeerConnection. All calls stay on the proof
signaling executor; the class is not a thread-safe room transport. The room
adapter must map authenticated connection identities and validate wire messages
before using it. Negotiation failure requires a fresh handoff.

Debug and Release media suites pass **22/22** (`build/webrtc/trickle-debug.log`
and `build/webrtc/trickle-release.log`). Dedicated coverage includes ordering,
readiness barriers, stale/closed callbacks, the 64-candidate bound, invalid
fields and throwing delivery callbacks. Application binaries were not rebuilt:
this milestone changes the reusable header and proof integration, not the
current application path.

This is still local in-process signaling without STUN configuration. Production
peer ownership, authenticated room routing, end-of-candidates wire mapping,
connection deadlines, bounded ICE restart/backoff and actual NAT/network tests
remain open. Native handle-growth failures and comparative latency acceptance
are unchanged. Do not mark the full PeerConnections checklist complete.

Headless Debug smoke passes **7/7** child runs; Release regression passes
**28/28**, including 20 single-peer processes and three four-peer/rejoin runs.
Artifacts: `build/webrtc/trickle-headless-debug/result.json` and
`build/webrtc/trickle-headless-release/result.json`. Both include executable
hashes, watchdog outcomes and component metrics. No manual input was needed.

## Connection lifecycle and real ICE restart - 2026-09-15

Added portable `PeerConnectionLifecycle` with a 20-second initial deadline,
typed direct-connect/remote-close/restart-limit failures and stale connection
event rejection. Disconnections use 500 ms/1 s/2 s backoff and at most three
restart attempts per rolling minute. Repeated notifications do not postpone
backoff; successful reconnection does not reset the rate budget. Transient
recovery before backoff expires avoids a restart. Restart attempts each have a
20-second deadline; expiry reschedules within the rolling budget. This is a
rate bound, not a maximum lifetime attempt count for a long-lived connection.

The proof feeds standards-compliant ICE state callbacks into the policy, checks
initial deadlines throughout description negotiation, and closes the policy
before peer teardown. One four-peer viewer receives an explicit restart request;
the host creates an ICE-restart offer and the viewer answers using fresh
credentials and fresh candidate handoffs. Candidates from retired local
credentials are filtered. The scenario verifies old handoff rejection, unchanged
200 kbps viewer settings, resumed frame delivery and continued healthy viewers,
then performs the existing full leave/rejoin. Credentials are never logged.

The deterministic policy test covers deadline boundaries, late/stale success,
duplicate disconnection, backoff, rolling-budget exhaustion/expiry, attempt
timeout, transient recovery, remote close and isolation from a healthy peer.

Timing investigation: the first combined state/media assertion took about
16 seconds despite negotiation finishing in about 0.6 seconds. Splitting the
assertion showed the 20-frame media check passed at 0.66 seconds while ICE state
confirmation arrived around 16.3 seconds. These are local scenario checks, not
an outage duration or external input/display measurement. The final proof uses
`OnStandardizedIceConnectionChange` instead of the legacy combined ICE/DTLS
callback documented in the pinned header. Keep negotiation, media-check and
state-check timings separate in JSON; do not gate frame delivery on state UI.

Production peer ownership/action dispatch, authenticated restart requests,
remote wire-generation binding, STUN/NAT and real outage/interface-change tests
remain open. This proof explicitly requests recovery; it does not simulate a
network outage or implement automatic application reconnection. Native handle
growth and external latency acceptance are unchanged. Application binaries were
not rebuilt because the new portable header is currently integrated in the proof
path; normal UI/CLI behavior remains legacy.

Final Debug and Release native media suites pass **23/23** each. Logs:
`build/webrtc/peer-lifecycle-final-debug.log` and
`build/webrtc/peer-lifecycle-final-release.log`. The final standardized-callback
Debug smoke measured 578 ms negotiation, 648 ms for the frame-count check and
16,283 ms for state confirmation. Switching callback did not remove the state
confirmation delay; its cause remains open, and it must not be described as a
16-second video outage. Existing AutoThread deprecation warnings are unchanged.

Final headless Debug smoke passes **8/8** runs; Release regression passes
**29/29**, including three complete four-peer restart/rejoin scenarios. Artifacts:
`build/webrtc/peer-lifecycle-final-headless-debug/result.json` and
`build/webrtc/peer-lifecycle-final-headless-release/result.json`. Executable
hashes and watchdog results are retained. Earlier timing runs remain historical
artifacts; use these final paths for the committed implementation.

## Owning peer registry and recovery dispatch - 2026-09-15

Added portable `IMediaPeer` and `HostPeerRegistry`. The registry owns at most
63 peer objects on one signaling/control executor, drives lifecycle actions,
queues asynchronous ICE restart requests and closes timed-out or failed peers.
A failed dispatch closes only that peer and records a distinct dispatch-failure
flag. Terminal snapshots retain their failure reason after native Close changes
the lifecycle state. Remove/Stop/destruction close each accepted peer once and
release ownership; rejected admission closes the supplied peer too.

Admission requires fresh, strictly increasing connection generations. Admit
peers in generation order before starting asynchronous negotiation in the
production adapter; do not order admission by asynchronous completion. This
keeps retired-generation rejection bounded without an ever-growing tombstone
history. Stop is terminal for that registry; a new host session needs a new
registry. Failed rows count toward capacity until removed. Implementations must
keep their lifecycle object alive through Close, cancel queued work on Close,
and never synchronously reenter registry methods from restart/close callbacks.

The four-peer proof now transfers actual peer-pair ownership into this registry.
Its adapter queues a restart; the existing local SDP driver consumes that work
outside Tick, preserving the nonblocking registry contract. The scenario removes
and replaces a peer through the owner, rejects a restart targeting the retired
incarnation and stops capture before releasing all peers. Peer shutdown is
idempotent to support explicit Close followed by object destruction.

Dedicated deterministic coverage checks one-shot restart dispatch, stale
requests/removal, rejected reused generations, throwing dispatch isolation,
initial timeout, repeated stop and close/destruction counts. The fake adapter
in that unit test is separate from the real media scenario.

Remaining: application signaling-executor scheduling, full asynchronous WebRTC
adapter/room delivery, automatic capture subscription cleanup on peer failure,
and UI/CLI facade integration. This does not establish remote network recovery,
resolve the native handle-growth defect or prove better external latency.

Debug and Release media builds/suites pass **24/24** each. Evidence:
`build/webrtc/peer-owner-debug.log` and `build/webrtc/peer-owner-release.log`.
The new owner currently enters the proof build through its portable header;
application binaries were not rebuilt and normal application behavior is
unchanged. Existing AutoThread deprecation warnings remain.

Headless Debug smoke passes **9/9** runs; Release regression passes **30/30**,
including 20 single-peer processes and three four-peer restart/rejoin scenarios.
Artifacts: `build/webrtc/peer-owner-headless-debug/result.json` and
`build/webrtc/peer-owner-headless-release/result.json`. Reports retain executable
hashes, watchdog outcomes and restart timing diagnostics. No manual input was
required. The existing delayed ICE-state confirmation remains unchanged.
