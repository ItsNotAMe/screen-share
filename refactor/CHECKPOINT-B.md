# Checkpoint B evidence

## Host capture/membership coordinator — 2026-09-15

`src/media/HostMediaSession` now owns CaptureSession and CaptureDistributor on
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
