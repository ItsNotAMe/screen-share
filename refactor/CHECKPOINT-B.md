# Checkpoint B evidence

## Opt-in Qt session adoption — 2026-09-16

ScreenShareUi accepts `--room-v2 CONFIG.json` and opens RoomSessionWindow using
QtRoomSession, the shared v2 backend and existing video widget/style. CLI parsing
was extracted into frontend/shared/RoomSessionConfig and a common frontend target.
Both entry paths enforce the same configuration/security rules. Default startup
and the normal room browser remain unchanged.

The Qt owner keeps one settings operation in flight and one latest pending edit,
checks local status without service requests, and consumes bounded latest frames.
Stop/close drain asynchronously while Qt continues processing events; the window
closes only after completion. Finished owners can start a new session incarnation.
Pending media/settings references are released on drain; cancellation preserves
admission uncertainty. Host controls expose presets, canvas, FPS, bitrate and Auto
limits with pending/applied/rejected state. Remote input is deliberately unavailable
until its authenticated consent/control path is integrated.

The offscreen widget scenario hosts/joins the isolated Worker, observes original
and reduced frames, rejects odd dimensions, coalesces 101 valid edits into one
revision, closes the viewer after drain, stops the host and reuses an owner. A
held media-stop future proves Qt heartbeat timers keep dispatching and finished
fires once per incarnation. No physical input is injected; audio is synthetic.

Final application suites pass 18/18 in Debug and Release; CLI-only Release passes
12/12. Logs: `build/webrtc/room-ui-app-debug.log`, `room-ui-app-release.log`,
`room-ui-cli-release.log`. Final Windows UI proof passes outside the capture-
restricted sandbox with WGC, the real renderer, 20 original + 20 reduced frames
and silent synthetic audio in 2.72 s. Artifact:
`build/webrtc/room-ui-windows-final/native-service-14cc707a-61ed-400d-8c42-69da41cbaed3`.

[ROOM-UI.md](ROOM-UI.md) documents invocation, lifecycle and test commands. These
results do not establish physical display/input latency, zero-copy presentation,
NAT/TLS, resource or service-cost acceptance. Normal create/join, directory/profile,
remote controls, source changes and aggregate allocation remain in milestone 2.

## Opt-in CLI host/viewer and bounded presentation — 2026-09-16

`ScreenShare --room-v2 CONFIG.json` now routes to a separate frontend using the
shared RoomSession/WindowsRoomRuntime. It supports host/join, display or window
capture, audio source/device selection, stream preferences, ordered timed live
changes, JSON status and responsive timed/console/window-close shutdown. Its
configuration parser rejects unknown fields, wrong types, invalid preferences,
oversized files and non-HTTPS production origins. Status/error output excludes
passwords and membership credentials; cancellation preserves admission uncertainty.

LatestRoomVideoFrame retains at most one decoded frame, replacing stale pending
work. The preview owner converts the consumed frame to NV12 and renders through
the existing D3D preview; decoder callbacks never render or queue unbounded frames.
CPU conversion is intentional for this first integration; zero-copy presentation
and measured gaming latency remain outstanding. Settings/status checks use local
snapshots and do not introduce service polling.

The same parser/controller is exercised against the isolated Worker with synthetic
H.264/Opus, a mid-session 320x180 -> 160x90 update, source/sender revision reports,
decoded pixel/NV12 validation, 100-frame presentation backlog replacement,
cancellation and failed admission. Real executable entry tests verify dispatch,
argument validation, production plaintext rejection and secret-free errors.
The deployment check starts with only the CLI executable in a fresh directory,
deploys and verifies Core/Network/WebSockets plus Windows TLS, and runs entry
checks. CLI runtime deployment no longer relies on tests or the Qt UI target.

Final application suites pass 17/17 in Release and Debug; CLI-only Release passes
12/12. Logs: `build/webrtc/room-cli-app-release.log`, `room-cli-app-debug.log` and
`room-cli-only-release.log`. All routine audio coverage uses silent endpoints.

The explicit Windows variant passes with generated-window WGC capture and actual
D3D preview presentation, silent synthetic audio and no physical input. Final
Release evidence: 53 original-resolution and 63 reduced-resolution frames,
6.78 s, at `build/webrtc/room-cli-windows-final/native-service-82755916-3ee6-4343-b16f-179c8866454f`.
This ran outside the capture-restricted sandbox. It does not measure physical
display latency or prove hardware-only codec use.

Usage, configuration and reproducible commands: [ROOM-CLI.md](ROOM-CLI.md).
Default UI/other CLI sessions remain legacy. This is opt-in frontend adoption,
not milestone 2 closeout: UI/profile/directory, remote input, source switching,
aggregate allocation, ICE-server configuration and all acceptance gates remain.

## Public live stream settings and silent tests — 2026-09-16

RoomSession now accepts validated host settings through a bounded asynchronous
command (one queued update; Busy tells callers to retry their latest choice).
Requests after stop/before Active are Unavailable; viewer runtimes return
Unsupported. Successful acceptance returns a monotonic runtime revision. Status
publishes desired preferences and each peer's sender-applied/source-observed
revision, current source dimensions and rejection. Acceptance does not claim
remote decode/presentation. Stop clears peer observations after media drains.

NativeRoomRuntime applies each revision once per ready sender. Existing peers keep
their prior working sender on live rejection, without a 5 ms retry loop; the next
explicit revision can retry. Initial failure still retires the affected peer.
New joins inherit the latest preferences. No renegotiation or room rebuild is
required, and the existing manual bitrate policy never installs a bitrate floor.
This changes per-viewer adaptation, not capture-device configuration or aggregate
bandwidth allocation. Increasing output FPS cannot exceed available capture FPS.

The public four-viewer proof changes to 320x180/20 FPS/manual 1 Mbps, checks all
sender/source revisions and reduced decoded frames, restarts, rejoins at the new
resolution, restores 640x360/30 FPS/Auto, and checks invalid/viewer/stopped commands.
Release and Debug media suites pass 31/31 with audio-device tests OFF; application
Release passes 14/14. Final Release public-session rerun after clearing stopped
peer status passes in 7.28 s. Logs: `build/webrtc/live-settings-{release,debug,app}.log`.
The Windows WGC variant passes outside the sandbox with synthetic audio in 9.96 s:
`build/webrtc/live-settings-windows-release/native-service-6ca5748f-03e3-452a-b069-7ba2de39dcbd`.

The runner rejects `-AudioDevice` unless `-AllowAudibleTests` is also supplied;
the rejection was checked before configuration/device startup. Routine runs omit
both and retain synthetic audio decoding coverage without speaker output.
No latency, achieved bitrate/FPS, resource, NAT/TLS or service-cost gate is closed
by these tests. Normal UI/CLI still use the legacy session pending adoption.

## Production runtime composition and Windows binding — 2026-09-16

NativeRoomRuntime replaces PublicRoomSessionProof's diagnostic peer/capture owner.
It composes MediaEngine/MediaPeer, HostMediaSession, HostPeerRegistry and
RoomManagedPeer. Host offers wait for attachment; restart requests pass through
the existing per-peer recovery budget. Viewer recovery emits an authenticated
restart request to the host. All native references survive capture retirement;
normal shutdown advances asynchronously until the registry/capture barrier drains.
Failures are reported to RoomMediaSession so status and authoritative membership
agree; failed delivery subscriptions cannot silently leave a connected blank peer.

Initial StreamPreferences are validated and applied to each source before delivery;
RTP settings are applied once negotiation is ready. Sources/senders are independent
per viewer. Live preference changes, aggregate allocation and UI settings remain
milestone 2 work. Source-start failure now reaches the public facade as Media,
rather than being misclassified as a room transport error.

WindowsRoomRuntimeFactory binds WindowsCaptureSource and WasapiPcmEndpoints,
with injectable audio for tests and a shared presentation sink/channel callback.
The host waits for its first capture device before building MF codec factories.
Device retirement retains the existing software fallback; automatic hardware
reenablement and actual driver-removal acceptance remain open. Source/sink objects
and the application WindowsMediaRuntime/SSL lease must outlive joined teardown.
ScreenShareMediaAdapters is now compiled by both application and proof targets;
NativeRoomRuntimeTests links the Windows factory through ScreenShareCore and checks
idle cancellation without opening capture/audio devices.

Full suites pass **33/33 media Debug/Release**, **14/14 application Release**,
**9/9 CLI-only Release**. Logs: `build/webrtc/native-runtime-debug.log`,
`native-runtime-integrated-release.log`, `native-runtime-app-final.log`,
`native-runtime-cli-final.log`. Final focused public-runtime checks with restart,
rejoin and healthy-peer progress after injected delivery failure pass in Release
(5.86 s) and Debug (5.95 s).

The generated-window Windows public-runtime proof passes outside the sandbox in
Release (5.76 s) and Debug (5.95 s), including four independent H.264/Opus viewers,
restart/rejoin and shutdown. Artifact roots:
`build/webrtc/native-runtime-windows-release-recovery` and
`build/webrtc/native-runtime-windows-debug-recovery`. Inside the sandbox WGC
CreateForWindow fails with “The specified service does not exist as an installed
service”; this is an environment limitation, not a passed test. No physical input
or desktop audio is used. These runs permit software fallback; hardware-only
encoding, external latency, two-hour soak and service-cost acceptance are not proven.

Milestone 1's local runtime integration deliverable is complete. Next is milestone
2: normal UI/CLI adoption, live settings, capture selection/audio configuration and
presentation through this factory. Legacy default removal still depends on the
original acceptance gates; normal application sessions have not been switched yet.

## Public owned session lifecycle — 2026-09-16

api/RoomSession.h now exposes asynchronous Start/Stop and thread-safe Status with
portable options/results. It owns RoomNetwork, SignalingExecutor, admission,
RoomSessionCoordinator, RoomMediaSession and an injected RoomRuntime. Native
factory construction and destruction happen on signaling. Start resolves on the
first authenticated snapshot, not the first decoded media frame. Repeated Start
is Busy; Stop is coalesced and also cancels pending admission. Cancelled in-flight
admission conservatively reports an unconfirmed outcome and is never retried.
Membership credentials remain internal and are cleared after shutdown.

Normal Stop sends peer.leave, waits up to one second for acknowledgement/closure,
then closes sockets. Media BeginStop completion gates runtime destruction while
Advance continues servicing cleanup. Stop/Status stay responsive during a held
media barrier. External destruction joins only after this barrier; never destroy
the facade inside its runtime callbacks. Runtime Ready/Remove are nonthrowing,
RoomSend is signaling-only and reports queue acceptance, and BeginStop must return
a valid completion future. A new admission uses a new RoomSession object.

Transport recovery retains the existing socket implementation and adds a ceiling
of three reconnect attempts per rolling minute and 35 seconds without a fresh
snapshot. Protocol/backpressure failures are terminal. The lifecycle timer stops
while healthy/idle; the existing coordinator continues media/event dispatch.
Recovery policy exists but injected real transport impairment remains unverified.

PublicRoomSessionProof uses only public Start/Stop/Status from the caller. Its
injected diagnostic runtime combines shared native engine/peers/negotiation with
HostMediaSession and synthetic source/audio. Four independent viewer engines each
validate decoded H.264 and audible Opus. Coverage also includes duplicate Start,
cancelled admission, unlisted create, graceful viewer departure, host closure,
production TLS enforcement, coalesced stop and a deliberately held drain barrier.
No physical input or caller Qt/WebRTC event pumping is involved.

Full media suites pass **33/33 Debug and Release**. Logs:
`build/webrtc/public-session-debug.log` and `public-session-release.log`.
Application Release **13/13** and CLI-only Release **8/8** also pass; logs
`build/webrtc/public-session-app.log` and `public-session-cli.log`.
After final shutdown/cancellation/security cases, focused public-session checks
pass again (Release 2.81 s, Debug 3.01 s; Testing/Temporary/LastTest.log).
This is correctness evidence, not latency, NAT, resource or service-cost acceptance.

Remaining: standard Windows RoomRuntimeFactory with production capture settings,
audio and presentation, and normal UI/CLI adoption. The synthetic diagnostic
composition is deliberately not installed as the default application backend.

## Shared room/media session composition — 2026-09-16

RoomMediaSession, in ScreenShareRoomSession, now consumes authenticated room
snapshots and signals on signaling. Host sessions reconcile connected viewers;
viewer sessions reconcile the connected host. The event queue is bounded to
256 entries / 512 KiB. Socket generation and snapshot revision barriers reject
stale updates; transport loss retires peers and requires a newer generation to
resume. Closed/stopped or overflow-failed sessions cannot revive from later input.
An invalid/rejected negotiation retires only its sender, and unrelated profile
revisions do not create retry storms. Unknown/nonmember signals are ignored.

RoomPeerRoster now distinguishes pending additions from failed ones. A readiness
hook gates slot reuse until asynchronous capture cleanup and native retirement
finish. Only pending additions retry on coordinator ticks. Authoritative removal
or shutdown cancels pending additions; failed construction waits for leave/rejoin.
Ready/Remove callbacks must not throw or reenter; Add owns cleanup on failure.

The real four-viewer proof uses a host and four viewer RoomMediaSessions. Its
diagnostic packet queue, role filtering and roster revision dispatcher have been
removed. The coordinator services these sessions after capture retirement. Native
construction hooks and media evidence remain in the proof. Viewer shutdown may
precede host membership delivery; health checks now respect that suspended state.
Production facades should create the session before opening its socket, route all
socket events through OnEvent, and call Advance from the existing coordinator.

Full media suites pass **32/32 Debug and Release**, application Release **13/13**,
CLI-only Release **8/8**. Logs: `build/webrtc/room-session-debug.log`,
`room-session-release-final.log`, `room-session-app.log`, `room-session-cli.log`.
Dedicated tests cover pending rejoin, cancellation, failure isolation, stale events,
terminal queue pressure and startup errors. These results establish correctness,
not latency/resource acceptance or a deployed-service free-tier cost improvement.
After final startup/terminal-close and pending-cancellation assertions, focused
room-network/session/authenticated-media checks pass **3/3** in both configurations
(Release 35.00 s, Debug 35.03 s; build Testing/Temporary/LastTest.log files).

Still open: a public facade owning admission, start/join/stop, all native resources,
recovery status and UI/CLI adoption. Normal application media remains legacy.

## Shared native engine and peer ownership — 2026-09-16

MediaEngine and MediaPeer are production components in ScreenShareNegotiation,
compiled by both application and diagnostic builds. The engine owns network and
worker threads, factory/Opus setup, caller-supplied audio/video implementations,
independent peer creation, host track attachment and data-channel creation.
Caller ICE configuration is preserved while Unified Plan/MaxBundle are enforced.
Failed audio attachment rolls back the video attachment; rollback failure closes
the peer. There is no synthetic source or diagnostic observer inside the engine.

MediaPeer owns ICE-state lifecycle updates, native negotiation, the incoming
video sink and a maximum of three channels. Unknown/duplicate labels or incorrect
reliability policies are closed before reaching application callbacks. Control
is ordered/reliable; input-state and telemetry are unordered/no-retransmit.
Close disconnects callbacks, removes the video sink, closes channels and native
connection, and cannot be undone by late ICE callbacks. A replaced incoming video
track first removes the old sink. Construction/use/destruction belong to signaling;
frame delivery belongs to WebRTC's delivery thread. Derived evidence sinks must
call Close before destroying their members. All peers/tracks must be released
before engine destruction, then the signaling executor may stop. SSL remains
application-owned.

RoomMediaProof no longer imports the old diagnostic Peer/MediaLink implementation.
Its remaining observers only validate decoded pixels/messages. Actual room-backed
four-viewer media, socket reconnect, budgeted restart, kick/rejoin, autonomous
timeout, cancellation and shutdown now exercise these shared native objects.

Media suites pass **31/31 Debug and Release**; application Release **13/13**, CLI-only
Release **8/8**. Logs: `build/webrtc/media-peer-debug.log`,
`media-peer-release.log`, `media-peer-app.log`, `media-peer-cli.log`.
The new lifecycle test covers ten engine lifetimes, rejected dependencies/thread
access, ICE-policy preservation, partial-track rollback, channel constraints and
late-event rejection. These are correctness checks, not handle/latency acceptance.
After final observer/header cleanup and the additional channel-policy assertions,
focused engine + authenticated-room tests pass **2/2** again in each configuration
(Release 35.19 s, Debug 35.33 s; each build's Testing/Temporary/LastTest.log).

Remaining milestone 1 work: move the outer room roster/session composition and
recovery policy into the shared facade, expose asynchronous commands/status, and
adopt it in UI/CLI. Normal application sessions still run the legacy backend.
Do not reopen completed factory, peer-lifecycle or dispatch extraction work.

## Autonomous room/media dispatch — 2026-09-16

ScreenShareRoomSession is a shared application/proof build target containing
RoomSessionCoordinator and RoomSignalCodec. The coordinator runs on signaling,
consumes the RoomNetwork mailbox automatically, resolves socket-open futures,
delivers generation-filtered events and advances media lifecycle hooks. Sends
return queue acceptance immediately; later network failures become typed error
events. Pending sends are bounded to 128 operations / 512 KiB, and a late failure
from a retired connection generation cannot fail its replacement. Close removes
callbacks before asynchronous transport shutdown; Stop cancels unresolved opens
and invalidates delayed work. Callbacks may send responses but must not mutate
socket lifetimes or destroy the coordinator.

RoomPeerSignal is now a portable media type. Shared wire conversion validates
incoming event structure and explicitly rejects non-signal events; authentication
and role authorization remain RoomSocket's responsibility.

RoomMediaProof no longer drains network events or forwards signaling from its
outer wait loop. Scenario commands and observations cross the executor boundary;
backend scheduling drives roster reconciliation, SDP/ICE and send completions.
The test pauses all observation for five seconds after requesting ICE restart,
then verifies negotiation already completed. The pause demonstrates autonomous
dispatch, not a latency acceptance measurement. Queue metrics now describe the
coordinator's pending sends rather than the retired diagnostic packet queue.

RoomSessionCoordinatorTest covers thread affinity, late send failure, operation/
byte bounds, retired callback suppression, cancelling an unresolved open and
no callbacks after stop/destruction. The normal ScreenShareSession facade remains
legacy: peer-factory/session composition and public commands/status still need
production adoption, followed by settings/presentation/input integration.

Validation: full media suites passed 30/30 in Debug and Release; Release application
13/13 and CLI-only 8/8. Final focused dispatch/media checks use
build/webrtc/coordinator-verified-{debug,release}.log; full logs are
coordinator-final-release.log, coordinator-full-debug.log, coordinator-final-app.log
and coordinator-final-cli.log in the same directory. Callback-generated responses
are tested outside pending-send iteration to avoid iterator invalidation.

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
