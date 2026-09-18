# Receiver buffering: capture cadence correction

## Cause and production change — 2026-09-18

The growing receive delay was caused upstream by `SwitchablePcmCapture`. Its old
read waited up to 10 ms for a block and emitted silence on timeout. A real block
arriving shortly afterward could then be returned immediately. Those two outputs
each advanced the audio timeline by 480 samples despite less than 20 ms elapsing.
Repeated boundary misses overfed WebRTC's audio jitter buffer. A/V synchronization
then delayed otherwise healthy video to follow the buffered audio.

A temporary separate-audio-stream experiment kept video buffering around 10 ms.
With normal synchronization restored, filtered WebRTC traces showed audio delay
around 0.9 seconds at the first trace, growing to roughly 3.6 seconds, while video
was progressively delayed. Those experiments isolated the cause; their temporary
stream grouping and logging changes were removed from the final code.

The wrapper now owns one monotonic 10 ms output cadence. At each slot it consumes
the latest fresh captured block or supplies silence. A late block cannot create
an extra output slot; a delayed consumer skips missed slots without a catch-up
burst. The producer still owns acquisition and retains only one block, with the
existing 30 ms stale-block rejection. Source changes use the same output cadence.
Failed endpoints supply silence on that cadence without a second silence wait.
Cancellation is checked before and after the at-most-10-ms scheduled wait.

No new media queue, thread, codec implementation, bitrate floor or synchronization
bypass is introduced. Native device teardown limitations remain unchanged.

## Regression coverage and evidence

- `LateCaptureCadence` uses a source that returns every 13 ms. Two hundred output
  blocks must take at least 1.95 seconds, with both real PCM and silence observed.
  It also checks no catch-up burst after a 50 ms consumer stall and cancellation.
- Release and Debug `pcm-adm-lifecycle` pass, including source-switch failure,
  timeout, microphone processing, None mode, endpoint recovery and shutdown.
- Both builds' public-room input/media regression passes with the production fix.
  Release/Debug UI and CLI applications rebuild successfully with that same backend.
- The lifecycle evidence validator now rejects missing or over-100-ms reported
  mean video jitter-buffer delay in this synthetic loopback scenario, even when
  frame-rate recovery passes. Fifteen Python evidence tests pass. The 100 ms guard
  detects this buffering regression; it is not an external gaming-latency target.
  Re-evaluating the original `receiver-sync-trace` artifact with the new guard
  rejects its excessive buffering despite its earlier frame-rate-only pass.
- The corrected 180-second Release trace run preserves A/V synchronization, passes
  slow-viewer isolation and recovers to about 29.8 fps on all four viewers. Maximum
  reported mean receive buffering is 21 ms versus roughly 1,400 ms before the fix.
  At most one receiver drop was reported, compared with hundreds before the fix.
- Debug's 180-second run passes the buffering guard: maximum reported mean 13 ms,
  zero receiver drops, healthy viewers around 30 fps during impairment and all
  viewers around 29.9–30 fps after recovery.
- Final Release without tracing passes: maximum reported mean 12 ms, zero receiver
  drops, healthy viewers 29.8–30 fps during impairment and recovery around 30 fps.
  All 24 tracked dependencies release; the separate 30-second post-stop observation
  and read-only memory accounting complete successfully in both 180-second runs.

Artifacts under `build/webrtc/` retain their own binary/runner hashes:

| Artifact | Scope |
| --- | --- |
| `receiver-without-sync/result.json` | Diagnostic bypass only; not the production fix |
| `receiver-sync-trace/result.json` and `native/resources.log` | Original behavior with filtered synchronization traces |
| `receiver-cadence-fixed-release/result.json` | Corrected 180-second Release run with synchronization traces |
| `receiver-cadence-fixed-debug/result.json` | Corrected 180-second Debug run and buffering guard |
| `receiver-pacing-final-release/result.json` | Final Release code without diagnostic tracing, with buffering guard |

```powershell
python scripts/test-room-lifecycle.py build/sdk-app-release build/webrtc/receiver-NEW --cycles 1 --soak-seconds 180 --slow-viewer --idle-seconds 30 --memory-accounting
```

The short reproduction and regression are closed only for this measured software
path. Full two-hour soak, network impairment, physical audio quality/device-clock
behavior, external capture-to-display/input-response latency and the existing WGC
resource failure remain separate acceptance gates. `soakAcceptanceComplete` stays
false. The next group is the sustained soak and network impairment work; retain
normal A/V synchronization and both the rate and buffering checks.
