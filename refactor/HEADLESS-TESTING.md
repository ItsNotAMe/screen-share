# Headless media checks

`room-v2-qt-ui` exercises actual Qt host/viewer widgets offscreen against the local
Worker, including coalesced edits, invalid settings, frame delivery and responsive
asynchronous stop/close. It uses silent synthetic audio and programmatic widget
actions, without physical input. [ROOM-UI.md](ROOM-UI.md) includes the explicit WGC
and real-renderer variant; offscreen rendering does not substitute for that proof.

The application suite includes `room-v2-cli-entry` (actual executable dispatch,
HTTPS enforcement and secret-free errors) and `room-v2-cli-media` (shared CLI
controller, local Worker, silent synthetic media, live changes, cancellation,
admission failure and bounded preview conversion). Use
`ctest --test-dir build/sdk-app-release -R "^room-v2-cli-" --output-on-failure`
after building. [ROOM-CLI.md](ROOM-CLI.md) documents reproducible finite runs and
the explicit generated-window Windows capture/preview variant.
`room-v2-cli-deployment` also stages the CLI alone in a fresh directory, verifies
its Qt networking/TLS dependencies and executes entry validation there.

Routine runs are silent: omit `-AudioDevice`. The runner explicitly resets the
cached device-test option OFF, while retaining synthetic PCM/Opus checks that
never play to speakers. Audible WASAPI tests require both `-AudioDevice` and
`-AllowAudibleTests`; only use them when the user requests audible testing.
Older commands below containing `-AudioDevice` describe historical device runs.

The public session proof also updates all four live viewers to fixed 320x180,
20 FPS and a manual 1 Mbps limit, verifies applied/source-observed revisions and
decoded reduced frames, restarts a peer, rejoins with the updated preferences,
then restores 640x360/30 FPS/Auto bitrate. It rejects invalid preferences, viewer
settings commands and commands after Stop. This checks correctness, not remote
latency or achieved bitrate/FPS. Source-observed is not a remote presentation ack.

The public-session scenario now uses NativeRoomRuntime itself; only source/audio
dependencies and evidence collection are diagnostic. It includes an authenticated
restart request, fresh rejoin and a single-viewer delivery failure while healthy
viewers continue. Capture startup failure must publish a Media error and drain.

For the Windows binding, build WindowsRoomSessionProof and run:

```powershell
node signaling-worker/tests/run-native-service.mjs build/sdk-proof-release/WindowsRoomSessionProof.exe build/webrtc/windows-runtime-check windows-media
```

This creates a test-owned animated window and captures it through WGC. Audio is
synthetic; no mouse/keyboard input occurs. It requires a Windows graphical session.
In the Codex sandbox WGC reports a missing capture service; the same test passes
outside that sandbox. With `-LiveCapture`, CTest also registers
`windows-room-session-media`. The regular CPU-only headless path remains available.
The Windows test permits codec fallback and does not assert hardware-only encoding.

`public-room-session-media` exercises the public v2 RoomSession API against an
isolated real Worker. It creates a host and four independent viewers, verifies
decoded H.264 and audible Opus separately for each viewer, leaves cleanly, cancels
admission, coalesces stop and holds a media-drain barrier to prove resources remain
alive while stopping. It also checks production rejects plaintext loopback and
unlisted room admission works. No Qt/event/SDP pump is exposed to the caller.
Build PublicRoomSessionProof and run the CTest entry; the usual native-service
harness records executable/Worker hashes and enforces a 60-second watchdog.

`room-media-session` tests role-based membership, stale socket generations,
peer-failure isolation without retry storms, asynchronous retirement/rejoin gates,
cancelled pending peers, startup errors and event-count/byte-pressure termination.
The authenticated four-viewer scenario now reports `shared_room_session: true`:
its snapshots and SDP/ICE use the same session routing as the backend target.
The coordinator advances it automatically; the diagnostic only observes progress.

`media-engine-lifecycle` exercises ten native engine lifetimes, wrong-thread
rejection, dependency validation, preserved ICE policy, partial-track rollback
and reliable control/unreliable transient-channel policy. It also verifies native
peer channel rejection, repeated close and rejection of late ICE revival. The authenticated
four-viewer scenario also uses this same backend factory/thread implementation.
Run it with CTest alongside `room-backed-four-peer-media`; neither requires
physical input. These are correctness checks, not latency or resource acceptance.

Build the proof targets using [BUILD.md](BUILD.md), then run from the repository
root. No mouse, keyboard, window, audio device or GPU is required for these two
commands; the Windows software Media Foundation codec must be available.

Quick smoke (includes a real ICE restart; allow up to the per-process watchdog):

```powershell
python scripts/test-headless-media.py build/sdk-proof-release build/webrtc/headless-smoke
```

Longer regression (100 capture restarts, 100 coordinator restarts, 20 one-viewer runs and three four-viewer runs):

```powershell
python scripts/test-headless-media.py build/sdk-proof-release build/webrtc/headless-regression --regression
```

Use a fresh output directory each time. Both commands return nonzero on failure.
To include native room/directory WebSocket checks, also build application tests:

```powershell
python scripts/test-headless-media.py build/sdk-proof-release build/webrtc/media-and-room-smoke --room-build-directory build/sdk-app-release
```

The optional room checks include native HTTP admission plus a real socket server and a separate networking thread,
including the actual 30-second heartbeat and 10-second missing-pong timeout. It
has a 75-second watchdog. No deployed service or credentials are needed.

Each child has a 45-second watchdog, or 60 seconds for a four-viewer scenario;
a timed-out child is killed and reaped. These
executables do not spawn descendants. Results include executable SHA-256, exit
code, elapsed time, timeout status, logs and capture timing percentiles in
`result.json`. The runner stops on the first failure and preserves its evidence.
WebRTC transport logging remains disabled to avoid recording signaling secrets.

Current coverage:

- Optional `RoomSocketTests`: authorization headers, snapshot readiness, pushed
  state/signals, room/self/role checks, one-shot resync, heartbeat/reconnect,
  directory lifecycle and saturated writes. This tests native transport, not
  server authorization, media connection-ID routing or Cloudflare cost.
- Optional `RoomAdmissionTests`: create/join bodies, typed rejection, identity/token
  validation, redirect/cookie suppression, bounded responses, cancellation and
  the real ten-second timeout. Socket tests also verify admission role binding.

- `HostPeerOwnerTest` exercises automatic deadlines/restart, failed-completion
  isolation, capture detachment, healthy media progress, stale requests and 25
  owner restarts with weak timers still queued. It never pumps WebRTC messages
  or explicitly ticks the registry. Four real peers now negotiate concurrently
  through that scheduled owner; restart and rejoin advance ready SDP futures
  without blocking peer callbacks. Signaling delivery is still in-process.

- The shared `SignalingExecutor` owns the WebRTC event loop for all media
  scenarios. Its headless test covers FIFO execution, native event callbacks,
  task failure, 64-command pressure, cancellation, closure destruction on the
  owner thread and repeated/self-requested shutdown. `--negotiation-only` also
  completes actual offer/answer operations using external future waits, without
  manually pumping messages. Other media diagnostics retain nested waits on
  the owned thread; those waits are not the application integration pattern.

- `PeerNegotiation` is built as the same library in the application and proof.
  Real offer/answer/restart paths now use its asynchronous operations. A dedicated
  `WebRTCProof --negotiation-only` scenario cancels 25 pending offers, destroys
  their owners, pumps late callbacks, verifies a fresh negotiation succeeds,
  and checks busy/stale requests, malformed/oversized SDP and native failures.
  It runs once in smoke/regression and also before the four-peer scenarios.

- A capture-bound peer registry automatically removes subscriptions on terminal
  failure and explicit removal. Cleanup futures are polled without waiting on
  signaling; queue-pressure/cancellation errors retry on later ticks. A blocked
  delivery/filled-command-queue test checks pending status, retry, healthy frame
  progress and exactly-once close. The four-peer scenario injects a terminal
  peer failure and verifies capture detachment before rejoin. This is a lifecycle
  fault injection, not an actual network failure.

- Capture subscription commands carry both host-session and viewer-connection
  generations. The coordinator test rejects retired attachment and cleanup
  commands after viewer replacement; the real four-peer rejoin scenario sends
  stale capture removal and verifies that the replacement keeps decoding.

- `HostPeerRegistry` owns peers behind `IMediaPeer`, dispatches lifecycle restart
  and close actions, retains failure snapshots and rejects retired-generation
  requests. The real four-peer scenario uses this owner for restart, removal,
  replacement and stop. A deterministic test covers dispatch failure isolation,
  timed-out close, stale/reused generations and exactly-once close/destruction.
  The proof adapter queues restart work for its local SDP driver; authenticated
  room delivery and the application event loop remain integration work.

- `PeerConnectionLifecycle` enforces the initial 20-second connection deadline,
  stale connection-event rejection and a rolling three-restarts-per-minute
  budget. Deterministic tests cover deadlines, duplicate disconnects, transient
  recovery, backoff, budget expiry and close without sleeping. Actual peer ICE
  state callbacks feed the policy; the four-peer scenario requests a real
  host-offered ICE restart with fresh credentials, preserves viewer settings
  and checks continued media on that viewer and healthy peers. JSON includes
  restart negotiation and media-check elapsed time; these are local scenario
  timings, not display latency or network-outage recovery acceptance.

- ICE candidates travel separately from SDP through the reusable bounded
  `IceCandidateHandoff`. Each direction waits for successful local and remote
  description application, rejects stale generations and closes its callback
  on teardown. SDP is asserted to contain no candidates. The dedicated test
  covers ordering, stale/closed delivery, overflow, invalid fields and delivery
  failure; real single/four-peer media tests exercise the handoff. This remains
  in-process signaling, without STUN/NAT or room-service transport coverage.

- Production `HostMediaSession` serializes capture/membership commands, assigns
  operation/session IDs and joins workers on stop. Its headless test covers
  100 restarts, stale commands, isolated callback failure, startup failure and
  cancellation despite a full command queue. The four-peer proof now uses this
  coordinator for capture and subscriber lifetime. Peer creation/signaling and
  the application facade remain outside it. Smoke runs ten child processes;
  regression runs 31. See [CHECKPOINT-B.md](CHECKPOINT-B.md).
- [COMPARISON.md](COMPARISON.md) defines the matched before/after scorecard.
  These checks establish regression coverage, not superior end-to-end latency.

- Production `CaptureSession` owns source construction, acquisition, recovery,
  callback delivery and source destruction on one worker. Both synthetic video
  and WGC adapters use this lifecycle.
- Headless owner scenarios exercise 100 fresh session IDs, initially disabled
  delivery, slow consumers, three device rebuilds, terminal exhaustion, source
  closure, startup timeout, callback failure and cancellation during backoff.
- Production `CaptureDistributor` gives each viewer its own delivery worker and
  one replaceable pending frame in addition to any frame already being consumed.
  Headless scenarios run four consumers with slow and failing consumers, remove
  a slow viewer during capture, and verify stale session/device/sequence rejection
  plus replacement-subscription isolation. Its JSON includes delivered/replaced
  counts and maximum capture-to-handoff age.
- The software media proof uses the same capture session with paced owned CPU
  pixels, actual H.264/Opus PeerConnections, offscreen decoding and three DTLS
  data channels. The longer command repeats complete media-process teardown.
  Both synthetic and WGC media proofs now deliver through the distributor.
- The four-viewer scenario creates four real host-to-viewer PeerConnection
  pairs with independent source wrappers, video senders, H.264 and three data
  channels per connection. It slows the fourth source handoff, checks continued
  healthy decoding, applies a 200 kbps WebRTC sender limit only to that viewer,
  resumes its delivery and replaces its complete connection while the others
  continue. Results include per-viewer decoded counts, pending replacements and
  successful rejoin. Opus is shared and checked at the aggregate playout sink.
- The production `CaptureVideoSource` wrapper preserves owned input dimensions
  and acquisition timestamps across handoff/conversion, and declares screen
  content with denoising disabled. The scenario includes a small-frame and
  delayed-timestamp check of that bridge.
- `StreamSettingsTest` checks the production Auto/Manual mapping, conservative
  initial-rate calculation, invalid/replayed revisions, fixed letterboxing and
  independent WebRTC pixel/FPS requests. It verifies downward adaptation and
  recovery without source upscaling. The four-peer scenario now applies its
  200 kbps setting through `ViewerStreamSettings`, checks no minimum bitrate was
  introduced, and waits for the source to observe that settings revision.

Capture-to-callback percentiles use one local monotonic clock and include pixel
generation. They do not measure encode, network, decode, display or gaming input
latency. Source frame pacing skips missed opportunities rather than producing a
catch-up backlog. Normal stop preserves frames already owned by consumers;
device loss invalidates resources from the failed device generation.

The WGC checks remain separate and require an interactive Windows desktop/GPU:
`CaptureRecoveryTest --live` and `WebRTCProof --live-capture`. They create their
own windows and need no manual input. Run through a process watchdog, as a
native driver/RPC hang cannot be preempted by the capture session stop token.

## Remaining integration

This is the first shared production component, not the complete session runner.
The normal UI/CLI still uses legacy media pending Gate A. Local proof peers share
one process. Separate host/viewer processes, production session-facade routing,
complete settings behavior, adaptive multi-viewer isolation, scripted authorized gaming input,
network impairment and resource-growth reports remain to be implemented.
Synthetic success does not satisfy WGC, Internet or external latency acceptance.
The four-viewer delay acts at the source handoff, not the decoder or network.
The sender-limit check verifies parameter independence and continued fixed-size
decoding; it does not prove congestion recovery, actual wire-rate limits or
Auto/Manual product semantics. Four software encoders require sufficient CPU.

The settings core now implements the source/RTP mapping, but complete product
semantics still require the coordinator/UI and impairment acceptance. Fixed
resolution keeps its canvas; manual FPS targets its rate while encoded-frame
drops remain allowed; manual bitrate is an upper operating limit subject to
WebRTC congestion control. Auto source requests use WebRTC's `VideoAdapter`.
No measured-bandwidth controller was added. Accepted sender settings queue the
source revision; `observedRevision` reports when capture delivery sees it, not
when a remote display has rendered it.

Resizing a GPU frame currently uses a counted CPU readback/scaling fallback.
Matching dimensions preserve the native GPU frame. The optional
`StreamSettingsTest --gpu` check verifies both paths; it requires a D3D11 GPU
and should run with a process watchdog. GPU scaling performance and end-to-end
timing remain acceptance work. CPU/synthetic settings checks need no GPU.

Coordinator contract: assign a fresh nonzero session ID, serialize owner calls,
keep frame callbacks short, reject obsolete IDs downstream, and join before
releasing callback state. Callbacks may request cancellation but must not join
or destroy their own session. Hold `WindowsMediaRuntime` across joined Windows
capture sessions, after UI STA initialization. The capture factory must create
its source on the owner thread; it must not return an object constructed on the
UI thread. There is no unbounded frame handoff queue in this component.

Stop the capture owner before stopping its distributor. Removing a subscriber
joins its delivery worker before returning. Serialized membership calls may
reuse a viewer ID only after removal completes; no old callback then survives
into the replacement. A device generation change replaces pending frames but
does not preempt an already-running callback: downstream consumers must check
the sample generation and honor retired GPU resources. Slow callbacks cannot
block other delivery workers, but removal/shutdown must wait for an in-flight
callback, so native-call watchdogs remain necessary. The current admission
ceiling is 63 subscriptions, matching the planned service abuse ceiling.
# Local v2 service runtime tests

## Compiled native client against workerd

After installing the signaling-worker npm dependencies and building application
tests (UI optional), run from the repository root:

```powershell
node signaling-worker/tests/run-native-service.mjs build/sdk-app-release/RoomServiceTests.exe build/webrtc/native-worker-evidence
```

CMake also registers `room-v2-native-worker` when Node and Miniflare are available;
it prints an explicit status message if prerequisites are missing. The executable
is built independently of that registration. This test launches an isolated local
workerd/SQLite service and exercises the production Qt admission/socket clients.
It covers directory push/resync, create/join, passwords/capacity, revision-bound
edits/conflicts, signaling, host reconnect, visibility, kick and closure. It does
not instantiate a media PeerConnection; SDP/ICE payloads are synthetic.

The harness binds only numeric loopback and uses the clients' explicit plaintext
diagnostic opt-in. A test-only entry supplies the HTTPS URL and trusted edge IP
normally provided by Cloudflare; production HTTPS checks are unchanged. Remote TLS
and actual edge deployment are not covered. The native process has a 60-second
watchdog; sockets/runtime are disposed after it exits. Each run creates a unique
artifact directory with executable/Worker-bundle hashes, timing, exit status,
limitations, result JSON and a bounded native log. No physical input is required.

Snapshot events now include their accepted revision alongside the copied payload,
so a consumer can issue expectedRevision edits without reading live network state.

Background-delivery coverage now deliberately holds a directory request while
admission, attachment, resync, edits and signaling execute. It tests newer updates
and host closure racing with the old acknowledgement, plus the actual five-second
fetch abort and eventual retry. The 1.5-second control-operation watchdog is a
regression bound, not a gaming-latency measurement. Expiry injection and observations
respect the production state serializer and asynchronous cleanup semantics.

Directory coverage now includes real pushed snapshots/deltas, publication failures
before and after commit, closure retry, version fences, lease expiry/renewal,
provisional capacity and host reconnecting, no per-room listing calls, resync floods,
and a 500-room maximum-name snapshot. `tests/directory.test.mjs` injects faults and
deadlines only in its in-memory test entry. See CHECKPOINT-C.md for remaining
real-time scheduling, hibernation, queue-pressure and native integration limits.

From `signaling-worker`, run `npm run typecheck` then `npm test`. The suite bundles
the production v2 Worker into an isolated Miniflare/workerd instance with SQLite
Durable Objects and real HTTP/WebSocket traffic. It needs no account, deployment,
physical input or desktop capture. The runtime test has a 60-second watchdog and
disposes sockets/runtime on completion. It tests concurrent admission, credentials,
socket replacement/reconnect, automatic pong and global capacity. Expiry is injected
through a test-only entry point and exercises the production alarm handler; actual
hibernation/alarm timing and native-client/media integration remain separate work.
# Authenticated four-viewer media (2026-09-16)

RoomSessionCoordinator now owns automatic room/media event dispatch. The scenario
wait loop only observes state; it does not drain room events or forward SDP/ICE.
A five-second observation pause during ICE restart verifies independent progress,
reported as `autonomous_dispatch`. This is not a five-second latency target.
`room-session-coordinator` tests bounds, late send failures and cancellation.
Pending-signaling metrics now come from the shared coordinator. Commands below
are unchanged; expect roughly 35 seconds for the real-room scenario.

Managed room peers now exercise HostPeerOwner's scheduled restart budget and
asynchronous capture retirement. The initial offer waits for capture attachment;
capture startup and final stop are awaited outside signaling. The
`scheduled-peer-owner` test holds a delivery callback open during BeginStop and
checks that signaling remains responsive until shutdown completes.

RoomNetwork now owns the Qt networking loop; the scenario never calls
QCoreApplication::processEvents. RoomPeerRoster drives peer creation/removal from
authenticated host snapshots, including socket disconnect/reconnect, kick/rejoin
and room-close cleanup. `room-network-ownership` separately tests bounded queues,
overflow, coalesced stops, cancellation and stale roster isolation headlessly.
The outer diagnostic still waits on command futures; it is not the normal UI/CLI
session coordinator. Commands for running the scenario remain unchanged.

The backend now schedules negotiation itself; the diagnostic does not poll native
negotiation futures. Additional coverage cancels before the first scheduled tick
and lets an unanswered peer hit its automatic 20-second deadline while healthy
media continues. Expect roughly 30 seconds for this scenario. Source relocation
to backend/ and frontend/ does not change the command below.

After building the hardware/audio-enabled WebRTC proof preset (requires the pinned
Qt 6.10.3 Core, Network and WebSockets installation), run from the repository root:

```powershell
node signaling-worker/tests/run-native-service.mjs build/sdk-proof-release/RoomMediaProof.exe build/webrtc/room-media-evidence media
```

This one command starts isolated workerd, creates/joins using native admission and
room sockets, negotiates real four-viewer H.264/Opus through authenticated signaling,
checks slow-viewer isolation and twelve encrypted data channels, restarts ICE,
kicks/rejoins a viewer, closes the room and cleans up under a watchdog. No physical
mouse/keyboard input is used. CTest includes it as `room-backed-four-peer-media`.
Each evidence directory includes JSON verdict/metrics, executable and Worker hashes,
and a native log. Signaling queue peaks and sent candidate counts are reported.

Capture/audio are synthetic; audible PCM is mixed receiver evidence. This test uses
shared production negotiation/room components with diagnostic orchestration, not
the unfinished normal UI/CLI facade. It does not establish desktop/GPU capture,
remote TLS/NAT, gaming input/image latency, leak bounds or service-cost acceptance.
