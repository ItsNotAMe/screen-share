# Hardware encoder investigation — 2026-09-18

The user reported that legacy hardware encoding caused delay and felt slow.
The controlled workload reproduces that symptom. Hardware is not automatically
the better choice: scheduling, buffering, driver behavior and competing GPU work
matter. [COMPARISON.md](COMPARISON.md) records the complete 16-run control matrix.

## Reproduced legacy delay before portable fixes

At 1080p with a 60 FPS / 12 Mbps ceiling, tuned legacy hardware measures
241–244 ms image-age p95, versus 60–65 ms for tuned legacy software. Both honor
requested timer precision and disable the legacy UDP pacing queue. Hardware
still delivers about 53 distinct source images per second: the problem is old
images arriving late, not simply an inability to emit frames.

All four pre-fix hardware-control logs report ten queued encoder inputs at shutdown.
The old `H264StreamEncoder::Start` set the asynchronous queue cap to
`clamp((fps + 5) / 6, 4, 12)`, which is ten at 60 FPS. `queuedInputCount()`
counts that application-side queue. Ten source frames represent about 167 ms
at 60 FPS before further encoding/network/receive work. This is consistent with
the measured excess delay; it is not a measurement of every driver queue or a
claim that this queue alone explains every millisecond. Legacy's sub-millisecond
`stream_encode_avg_ms` measures asynchronous submission, not completed encoding.

## What differs in v2

The v2 adapter reuses the encoder implementation with external hardware scheduling.
It allows one active submission plus one replaceable pending frame; newer input
replaces waiting work instead of building the legacy ten-frame queue. It checks
output timestamp association, probes actual configured dimensions/rate changes
and keyframes, and uses precise short waits for asynchronous output.

A missing output triggers a 500 ms deadline, session-scoped hardware quarantine
and software recovery with a keyframe. Cancellation and retired-device handling
are separate: a cancelled healthy encoder is not quarantined, and stale GPU
textures are never read back after device retirement.

At the user's request, the portable scheduling fix is also applied to
legacy: `HardwareFrameWait` lives in the shared codec module and legacy completes
one hardware frame before taking another. The old raw-frame queue is removed,
rather than reducing its limit only in the benchmark. Legacy frame-clock and WGC
poll waits also use `ShortWait`; asynchronous startup/drain waits use it too.
The preceding numbers remain the pre-fix baseline and must not be presented as
the final tuned-legacy comparison.

Portability audit for the repeat comparison:

| Fix/control | Legacy treatment | V2 treatment |
|---|---|---|
| Precise short waits | Shared WGC polling, frame-clock/input slices, hardware readiness/output/drain | Capture owner, GPU completion, hardware output and silent PCM pacing |
| Encoder queue delay | Same shared one-submission completion/deadline; old ten-frame queue removed | Same helper; one replaceable pending frame outside it |
| Sender pacing latency | Existing low-latency mode explicitly selected; no UDP pacing backlog | Gaming WebRTC pacing adjustment retained; bitrate/congestion controls active |
| Timer-resolution policy | Test process honors requested precision | Identical test-process policy |
| Silent-sink fixture error | No audio track/output, so no unpaced sink | Shared paced silent sink; never the superseded no-op fixture |

WebRTC field trials are not applicable to the legacy UDP transport. Decoder
implementations and per-viewer encoding architecture still differ and remain
reported limitations. Removing these differences would replace the baseline
rather than isolate the portable fixes; no intrinsic architecture speedup is
inferred from the resulting total CPU difference.

These are failure-recovery mechanisms, **not a 500 ms gaming latency target**.
A driver that continually returns frames slowly but within the deadline need
not trigger fallback. Blocking third-party Media Foundation/driver calls cannot
be interrupted by this polling deadline. Startup probes do not certify sustained
gaming performance under GPU contention.

## Controlled result and policy

With the same v2 pipeline and decoder preference, final hardware controls give
72–79 ms image-age p95 and about 52 fresh frames/s; software gives 109–121 ms and
about 37 fresh frames/s. Hardware also uses less measured process CPU. All eight
final v2 control runs confirm the requested encoder and zero unexpected fallbacks.
Keep hardware preferred based on this measured desktop result; do not claim
it is universally faster or disable software recovery.

`WindowsRoomRuntimeOptions::preferHardwareEncoding` now allows an embedding or
diagnostic to force software without changing the room/capture/transport path.
The normal default remains hardware-preferred. This is not a new user-facing
encoder selector. The software throughput deficit needs attention before
calling fallback equivalent to normal 1080p60 operation.

After the shared fixes, legacy hardware improves from 241–244 ms to **60–64 ms**,
with about 52–53 fresh frames/s and zero queued raw inputs in all four final logs.
Legacy hardware and software both beat v2 hardware's local image age; the
hardware-to-hardware gap is about **13–15 ms**.
Consequently neither this hardware result nor the earlier default-configuration
comparison establishes that every backend performance goal is complete.

## Recovery validation

Release and Debug each pass six hardware tests: output deadline/cancellation,
decoder recovery, fallback/GPU ownership, direct hardware H264 transport,
encoder lifecycle and owned-GPU encoder lifecycle. The fallback test drains real
Media Foundation events while withholding output to force timeout/quarantine and
checks timestamp-preserving software IDR recovery. Other checks cover zero-rate
suspension/resume, keyframes, latest-frame burst coalescing, reset/release and
retired-device recovery. These are injected failure tests, not physical driver
crash or unplug certification.

Raw controls: `build/comparison-codec-controls/result.json`, with per-run reports
and logs; updated controls: `build/comparison-portable-final/result.json`.
Both matrices validate 16/16 runs. [Compact evidence](evidence/codec-portable-controls-2026-09-18.json)
retains their measurements, hashes and verification. All runs use generated window content, silent/discarded audio and no
physical input. No game, user desktop content or controller input is captured.

The additional legacy regression encodes sixteen retained GPU frames over two
start/drain/stop cycles. Every call must return exactly its current timestamp,
nonempty data and sender-clock metadata, with no pending raw input or CPU
readback. Bitrate/keyframe changes must produce an IDR, and drain must not expose
old retained output. This would fail the former asynchronous queue behavior.
Release/Debug each also pass native runtime, CLI media, offscreen Qt UI and the
ten evidence-validator cases after the portability change.

## Ten-minute hardware load

The pre-portability v2 executable completed 600 measured seconds with four
1080p viewers, 129,512 total hardware-encoded frames and zero software fallbacks.
All four final senders/receivers report hardware H264. Worst-viewer whole-run
image-age p95 is 72.0 ms and p99 is 115.0 ms; fresh delivery is 51.4–51.9 FPS,
with zero invalid markers. Mean completed encode time is 4.1–4.3 ms at the final
sample. Teardown completes in 0.63 seconds. Raw evidence is in
`build/comparison-hardware-sustained`.

This did not reproduce legacy's persistent 240 ms lag. It is one ten-minute
generated-scene run, not a two-hour hardware certification or a test under game
GPU load. Percentiles cover the whole interval; there are no per-minute latency
percentiles, so brief stalls or a short late-session regression are not excluded.
The application was not rebuilt during the run.

Private process memory rose from 572.6 to 606.2 MiB (maximum 626.8 MiB); handles
went from 2,060 to 2,065 with a 2,290 maximum. The diagnostic deliberately retains
marker IDs and age samples throughout measurement. Those totals include fixture
growth, runtime/driver allocation and all local peers; they do not certify a
production leak bound. Existing native resource acceptance remains open.

## Remaining boundaries

The tested desktop has an RTX 5070 Ti and AMD integrated graphics; inventory
alone does not prove which adapter every codec selected. The controls confirm
hardware/software implementation selection, not vendor attribution. Repeat the
real two-machine GPU-presentation check with the selected adapter recorded and
representative game GPU load before a broad hardware-performance claim.

Equal configured bitrate ceilings are not equal measured wire rate. Legacy
decodes in software and shares one encoded stream; v2 uses independent encoders
and hardware-preferred decoding. Local CPU image consumption adds readback and
does not measure display/input latency, audio quality, A/V skew, GPU utilization
or Internet behavior. Keep those acceptance gates open.
