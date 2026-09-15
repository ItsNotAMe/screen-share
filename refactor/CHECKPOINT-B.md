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
