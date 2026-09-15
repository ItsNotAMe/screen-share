# Headless media checks

Build the proof targets using [BUILD.md](BUILD.md), then run from the repository
root. No mouse, keyboard, window, audio device or GPU is required for these two
commands; the Windows software Media Foundation codec must be available.

Quick smoke (roughly 15 seconds on the reference machine):

```powershell
python scripts/test-headless-media.py build/sdk-proof-release build/webrtc/headless-smoke
```

Longer regression (100 capture restarts, 20 one-viewer runs and three four-viewer runs):

```powershell
python scripts/test-headless-media.py build/sdk-proof-release build/webrtc/headless-regression --regression
```

Use a fresh output directory each time. Both commands return nonzero on failure.
Each child has a 45-second watchdog, or 60 seconds for a four-viewer scenario;
a timed-out child is killed and reaped. These
executables do not spawn descendants. Results include executable SHA-256, exit
code, elapsed time, timeout status, logs and capture timing percentiles in
`result.json`. The runner stops on the first failure and preserves its evidence.
WebRTC transport logging remains disabled to avoid recording signaling secrets.

Current coverage:

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
