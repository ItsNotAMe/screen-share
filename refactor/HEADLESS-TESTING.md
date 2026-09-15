# Headless media checks

Build the proof targets using [BUILD.md](BUILD.md), then run from the repository
root. No mouse, keyboard, window, audio device or GPU is required for these two
commands; the Windows software Media Foundation codec must be available.

Quick smoke (about eight seconds on the reference machine):

```powershell
python scripts/test-headless-media.py build/sdk-proof-release build/webrtc/headless-smoke
```

Longer regression (100 capture restarts plus 20 complete local media runs):

```powershell
python scripts/test-headless-media.py build/sdk-proof-release build/webrtc/headless-regression --regression
```

Use a fresh output directory each time. Both commands return nonzero on failure.
Each child has a 45-second watchdog; a timed-out child is killed and reaped. These
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
settings changes, multi-viewer isolation, scripted authorized gaming input,
network impairment and resource-growth reports remain to be implemented.
Synthetic success does not satisfy WGC, Internet or external latency acceptance.

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
