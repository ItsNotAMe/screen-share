# Measured backend comparison

These are local measurements, not universal performance or
Internet-latency guarantees. Backend comparisons controlled hardware/software,
viewer counts and run order. Retain the modular backend; outstanding qualification is listed
in [known limitations](known-limitations.md).

## Idle capture demand (2026-11-07)

The host now requests full capture cadence only when at least one connected,
configured viewer can receive video. Empty rooms, connecting-only peers and video
pause use at most five health samples per second (respecting lower source caps).
Bootstrap runs at the requested cadence until valid native pixels are owned.
Renewed viewer demand wakes capture without waiting for the 200 ms idle slot.
Audio, input identity and source monitoring remain independent; stop still checks
cancellation on the existing capture loop. No new worker, queue or encoder policy.

Six alternating active/idle phase pairs on one production CaptureSession owner,
with generated changing 1920×1080 luma, measured:

| Measure | Active | Idle |
| --- | --- | --- |
| Median process CPU ms per second | 120.92 | 15.61 |
| Median delivered samples in about two seconds | 120 | 10 |

Median CPU work falls **87.1%** in this generated capture workload; individual
pairs improve 74.9–91.7%. CPU includes the capture worker and measurement thread,
with 500 ms settling followed by 2000 ms measurement per phase. Windows process
CPU timing is coarse at this low idle load. This measures luma generation,
capture polling and callback handoff, excluding WGC, codecs, transport and
rendering. It is not a measured native-game or whole-application CPU reduction.
[Raw phase runs and source/executable hashes](benchmarks/idle-capture-2026-11-07.json).

The real-peer/synthetic-source regression records six capture samples in each
1.2-second empty/paused interval, preserves audio during video pause, restores
normal capture for a connected viewer and idles again after the last viewer
leaves. A merely connecting peer cannot keep capture at full rate. Supplied-clock
checks cover source caps, exact active/idle boundaries, immediate demand wake,
missed-slot skipping and rebuild reset. Queued WGC selection is bounded to two
newer-frame polls and explicitly closes discarded frames before conversion.
Portable tests check newest-frame selection, ownership and bounded/error cases.

The native window proof (`NativeCaptureTests --demand-check` or `--demand`) also
checks the first resumed pixels, input identity, minimize/restore and closure.
It compiles, but could not open its generated test window here: Windows Graphics
Capture reports that its service does not exist. These native assertions and
native capture CPU savings remain unverified; no desktop capture was substituted.

Reproduce the generated measurement with
`python scripts/measure-idle-capture.py --program build/release/CaptureDemandBenchmark.exe
--output RESULTS.json`. Active source cadence and automatic encoder threading
remain unchanged. There is no claim of lower active-sharing latency.

## Software encoder slice-worker experiment (2026-11-07)

**Default decision:** normal sharing uses automatic software threading. The
two-worker limit introduced in `89b3522` is removed from that default because it
increases encoding completion time and reduces frame-time headroom. Lower CPU
work does not mean lower stream delay. An efficiency tradeoff should be an
explicit choice; there is no user-facing efficiency mode yet. The optional
low-level worker setting and the evidence below remain available.

The experiment requested at most two slice workers through 1080p, respecting a
one-core assignment, instead of each encoder choosing an automatic pool. Larger
pictures, hardware and low-level callers' default were unchanged. Microsoft's
[worker-count property](https://learn.microsoft.com/en-us/windows/win32/medfound/codecapi-avencnumworkerthreads)
controls this setting. In the tested inbox codec it must be set before format
negotiation to size the pool, then reapplied afterward because negotiation resets
the reported property. Applying only afterward changed the query but retained
the original thread count and bitstream. Diagnostics report the final queried
setting, with zero meaning automatic/unavailable. Optional rejection remains
compatible, and rate/FPS restarts and software fallback retain the request.

This trades encoding parallelism for lower CPU consumption. Six alternating
AB/BA pairs for each pattern/input/encoder count, against unmodified `de0d90f`,
gave these four-encoder medians:

| Generated pattern | Input | CPU ms per encoded picture, before → after | CPU reduction | Mean elapsed ms per four-picture batch, before → after |
| --- | --- | --- | --- | --- |
| Smooth moving pattern | CPU NV12 | 15.059 → 11.523 | 23.5% | 6.132 → 7.435 |
| Smooth moving pattern | GPU NV12/readback | 14.962 → 11.511 | 23.1% | 6.241 → 7.930 |
| Detailed moving pattern | CPU NV12 | 14.392 → 11.344 | 21.2% | 5.687 → 7.256 |
| Detailed moving pattern | GPU NV12/readback | 14.677 → 11.833 | 19.4% | 6.622 → 8.023 |

Every paired CPU result improved; four-encoder paired reductions ranged from
12.4% to 28.2%. One-encoder median CPU reductions were 25.4–32.9%, with mean
elapsed time increasing by 2.38–2.64 ms per picture. Four-encoder live process
thread counts fell from 57 to 21 for CPU input and 80 to 44 for GPU input;
these counts include all benchmark/driver threads. Private-byte medians also
fell slightly, but this is not a full-application memory measurement.

Longer encode times matter. With four encoders and detailed GPU input, median
batch p95 rose from 12.04 to 16.89 ms and p99 from 13.50 to 18.52 ms. Some batches
exceed the 16.67 ms budget of 60 FPS. These unpaced tests do not establish
unchanged 60 FPS delivery under game load or lower input-to-display latency.

Bitstreams change with the slice layout. Decoding the first 16 measured pictures
from each encoder, including their warmup/reference chain, gives these PSNRs:

| Pattern | Luma dB, before → after | Combined chroma dB, before → after |
| --- | --- | --- |
| Smooth | 53.061 → 52.844 | 54.242 → 53.776 |
| Detailed | 45.092 → 45.839 | 45.446 → 45.890 |

Thus smooth-pattern quality decreases slightly and detailed-pattern quality
improves in this fixture; quality is not universally unchanged. Both use the same
12 Mbps assignment, but actual VBR output differs: smooth output is about
7.46 → 6.82 Mbps and detailed output 12.85 → 12.88 Mbps over the nominal eight
seconds of measured input. Output hashes/byte counts are
stable within each version. The runner permits these differences only with an
explicit decoded-quality bound (0.5 dB here); its default remains bit-exact.

All 96 runs use identical benchmark source and Release options on one
16-logical-processor Windows machine: generated changing 1920×1080 NV12, 60 FPS
settings, 30 warmup frames and 480 measured frames per encoder, with at most one
frame in flight per encoder. Timed work includes preparation/readback, encoding,
callback handoff and encoded-byte hashing. Process CPU includes all codec threads.
Fixture generation, startup, warmup, resource snapshots, post-run decoding/quality
checks and teardown are excluded. Capture, transport, rendering and physical game
performance are outside the comparison. The full-room scorecard is unchanged.
[Raw runs, quality/resource checks, source and executable hashes](benchmarks/encoder-thread-budget-2026-11-07.json).

Reproduce with `EncoderComputeBenchmark.exe cpu 4 480` or add `detailed` as its
fourth argument; `gpu` selects generated GPU input for software encoding. To
reproduce this experiment, use the benchmark source from `89b3522` on both
sides: production `de0d90f` for the automatic baseline and `89b3522` for the
limited-worker candidate. The current sharing default again uses automatic
threading. Build those benchmark executables, then run
`python scripts/compare-encoder-compute.py --before BEFORE.exe --after AFTER.exe
--output RESULTS.json --samples 6 --frames 480 --patterns both
--quality-max-loss-db 0.5 --baseline-commit de0d90f --before-source BASELINE_SOURCES`.
The baseline directory contains the three named encoder/factory sources recorded
in the raw report. Real-MF checks cover 1/2/16-core assignments, property readback,
FPS restart/resume, automatic larger pictures, invalid counts and the existing
keyframe, mapping, coalescing, late-output and cancellation behavior.

## Direct visible-frame copy in software decoding (2026-11-07)

Cropped software output now copies visible Y/UV rows straight from the locked MF
buffer into independently owned frame pixels. The former path allocated/copied
the entire coded picture first, then allocated/copied its visible crop. For a
1920×1088 coded picture cropped to 1920×1080, this removes an intermediate vector
of about 3 MiB and its full-frame copy. The visible output allocation/copy remains;
uncropped output and hardware texture ownership retain their paths. The buffer
unlocks on crop validation or allocation failure as well as on success.

Six alternating AB/BA pairs per configuration against `850fd02` gave these medians:

| Concurrent software decoders | Elapsed ms for 960 pictures each, before → after | Process CPU ms per decoded picture, before → after |
| --- | --- | --- |
| 1 | 3411.35 → 2852.87 (−16.4%) | 5.713 → 5.469 (−4.3%) |
| 4 | 4425.46 → 3708.36 (−16.2%) | 6.952 → 6.104 (−12.2%) |

Every four-decoder pair improved both CPU (5.9–19.0%) and elapsed time
(13.3–24.4%). Single-decoder elapsed reductions were 13.3–20.9%; CPU results
were noisy, ranging from a 5.2% regression to a 32.6% reduction. Do not infer a
uniform single-decoder CPU gain. All runs retained matching decoded byte counts,
full-frame checksums, visible dimensions and timestamp order.

The benchmark source and fixed 64-picture encoded 1080p fixture are identical in
both executables. Each decoder runs on its own thread, warms for 64 pictures,
then decodes 960 pictures without pacing. Timing covers decode, owned pixel
copies/cropping, a full-frame checksum and drain. Process CPU includes all codec
threads; fixture generation/loading, startup, warmup and teardown are excluded.
No other builds/tests ran during the comparison. Both versions already reuse MF
scratch buffers, whose allocation counter stays zero after warmup; that counter
does not count the intermediate pixel vector removed here. This is a decoder
workload measurement, excluding capture, networking, presentation and total
application memory/CPU or end-to-end latency. It does not qualify hardware handle
lifetime. [Raw runs, fixture/source identifiers and binary hashes](benchmarks/decoder-visible-copy-2026-11-07.json).

Reproduce with the existing `DecoderComputeBenchmark` and
`scripts/compare-decoder-compute.py` commands in the next section, using
`--baseline-commit 850fd02` and the same fixture for both executables. The baseline
is unmodified production decoder code; its allocation counters already existed.
Regression checks include changing cropped/uncropped output, resize, retained
immutable frames, Stop/restart and the generated-input GPU path.

## Reused software-decoder output storage (2026-11-06)

The software decoder now reuses one caller-owned Media Foundation output buffer
instead of allocating another large NV12 buffer on every output poll, including
polls that need more input. Samples remain fresh on each poll, and decoded frames
still own independent pixel vectors. A format change discards the buffer; Stop
releases it. The hardware decoder's supplied samples keep their existing path.
The caller-allocation branch follows the transform's output-stream flags and
buffer-size requirement described by Microsoft's
[ProcessOutput contract](https://learn.microsoft.com/en-us/windows/win32/api/mftransform/nf-mftransform-imftransform-processoutput).

Six alternating-order pairs per configuration, using the same pre-encoded
1920×1080 fixture, gave these medians:

| Concurrent decoders | Elapsed ms for 960 frames each, before → after | CPU ms per decoded frame, before → after | Measured large output-buffer allocations, before → after |
| --- | --- | --- | --- |
| 1 | 3595.84 → 3363.89 (−6.5%) | 5.916 → 5.420 (−8.4%) | 1921 → 0 |
| 4 | 5162.26 → 4623.26 (−10.4%) | 7.731 → 7.153 (−7.5%) | 7684 → 0 |

Every elapsed pair improved: 3.9–11.3% with one decoder and 5.3–15.2% with four.
CPU measurements varied: paired reductions ranged from −5.4% to 17.2% and
−1.3% to 11.2%, respectively. The median CPU improvements do not establish a
uniform CPU reduction. All runs retained matching decoded byte counts and
full-frame checksums, dimensions and timestamp order.

The baseline is `ff38fa3` with an allocation counter and the identical benchmark
source added. Both executables use the same Release toolchain and one fixed
64-picture fixture. Each decoder has its own thread and warms up for 64 pictures;
timing then covers 960 pictures, drain, owned output copies/cropping and a full-
frame checksum. Process CPU includes all decoder worker threads. Fixture generation,
loading, decoder startup, warmup and teardown are excluded. The four-decoder case
models simultaneous software receive workloads, not four host encoders. Capture,
networking and presentation are excluded; these are not total application CPU
or end-to-end latency measurements. No other builds/tests ran during the comparison.

Reuse retains one scratch buffer per running software decoder (about 3 MiB for
coded 1920×1088), bounded by the existing 128 MiB output limit. The two initial
allocations per decoder happen before measurement as the stream type settles.
Published-frame copies and codec-internal allocations remain. Regression checks
cover changing pixels, retained frames, 1080 cropping, resize in both directions
and Stop/restart cleanup. This does not qualify hardware decoder handle lifetime.
[Raw runs, source identifiers and binary hashes](benchmarks/decoder-compute-2026-11-06.json).

Build `DecoderComputeBenchmark`. Generate the fixture once with
`DecoderComputeBenchmark.exe --generate build/decoder-input.bin`, then run
`DecoderComputeBenchmark.exe build/decoder-input.bin 4 960`. For a comparison,
compile the identical benchmark against each revision with the allocation counter
available, save the baseline decoder sources, and run
`python scripts/compare-decoder-compute.py --before BEFORE.exe --after AFTER.exe
--fixture build/decoder-input.bin --before-source BASELINE_SOURCE_DIRECTORY
--baseline-commit BASELINE_REVISION --output build/decoder-results.json
--samples 6 --frames 960`.
The runner alternates order, saves each run, rejects differing byte counts or
checksums, and records fixture, benchmark, decoder-source and executable hashes.

## Matched encoder timing and direct plane input (2026-11-06)

The earlier NV12 change (`2c2a5fa` → `05582cc`) reduced elapsed encoding time
for CPU-backed input by 8–12% in four alternating-order pairs. GPU-backed input
showed no clear elapsed improvement, and process CPU measurements were noisy.
Removing conversion calls alone did not establish a general CPU reduction.

The next change removes the temporary per-frame NV12 vector and packing copy.
Software NV12 planes now copy directly into an owned Media Foundation sample;
padded strides and minimal last rows are supported. The source remains owned
through that synchronous copy. I420 and hardware submission retain their paths.

Six alternating-order pairs against `05582cc`, 480 measured input frames per
run after 30 warmup frames, gave these medians:

| Input | Encoders | Elapsed ms per input batch, before → after | CPU ms per encoded frame, before → after |
| --- | --- | --- | --- |
| CPU NV12 | 1 | 2.642 → 2.174 | 12.207 → 13.460 |
| CPU NV12 | 4 | 8.934 → 8.139 | 17.000 → 15.584 |
| GPU NV12 | 1 | 3.205 → 2.782 | 14.062 → 12.939 |
| GPU NV12 | 4 | 8.486 → 7.571 | 16.553 → 16.036 |

Elapsed reductions range from 8.9% to 17.7%; the four-encoder GPU case improves
10.8%, with all six pairs faster. CPU readings are mixed, including a higher
single-encoder CPU-backed median, and every case's paired CPU range includes a
regression. CPU savings remain inconclusive. Encoded byte counts and 64-bit
output hashes match across every paired case.

This is one 16-logical-processor Windows machine, generated changing 1920×1080
NV12, software H.264 at 60 FPS / 12 Mbps settings, unpaced with at most one frame
in flight per encoder. Process CPU includes user/kernel time on all threads;
elapsed time includes preparation, encoding, callbacks and output hashing.
Startup, input generation, capture, network, decoding and presentation are
excluded. These are encoder-workload timings; the full-session scorecard is
unchanged. [Raw runs, binary hashes and source identifiers](benchmarks/encoder-compute-2026-11-06.json).

Build `EncoderComputeBenchmark` and run `EncoderComputeBenchmark.exe cpu 4 480`
or `gpu 4 480`. For comparison, compile the identical benchmark source against
each revision in isolated builds with the same pinned SDK and Release options,
then run `scripts/compare-encoder-compute.py --before BEFORE.exe --after AFTER.exe
--output build/encoder-compute/results.json --samples 6 --frames 480`. The runner
alternates order, saves every run, rejects differing outputs and reports CPU
variability. It does not modify the source trees or change machine scheduling.

## Direct NV12 software encoding (2026-11-06)

The Media Foundation adapter now packs NV12 input directly instead of converting
it to I420 and immediately converting it back to NV12. GPU readback can retain
NV12 for software encoders and CPU preview materialization. I420-only callers
keep their direct readback conversion; a second CPU format is allocated only if
explicitly requested, using already-owned pixels without another GPU readback.

The new encoder test submits eight changing, padded NV12 frames to four real
software encoders and decodes all 32 outputs. The old adapter called ToI420 32
times; the updated adapter calls it zero times while preserving decoded luma,
chroma, dimensions, timestamps and frame mapping. The explicit GPU variant uses
eight shared GPU frames for the same 32 outputs: eight readbacks, zero I420
conversions. It also checks CPU preview pixels and both mapping request orders.

These are conversion/readback counts; matched encoder timings appear above.
The following direct-plane change removes temporary per-viewer packing. Media
Foundation's owned input copy remains. The full-session scorecard is unchanged.

## Shared scaling across viewers (2026-11-06)

Matching viewers now reuse an immutable frame's GPU-scaled pixels and lazy CPU
readback. Each captured frame retains at most four output geometries; different
frames never share cached output. Viewer settings, frame mapping, adaptation and
encoding remain independent. Device retirement and scaler quarantine bypass the
cache, and evicting an entry leaves retained consumer frames valid.

The explicit generated-texture test (`StreamSettingsTests.exe --gpu`) delivers
24 changing 1920×1080 frames concurrently to four 1280×720 viewer sources, using
the production adaptation/scaling path. Separate wrappers reproduce the former
work; the shared-input path uses the same settings and pixels. Before the change,
both paths submitted 96 scales and performed 96 readbacks. Afterward, the shared
path submits 24 scales and performs 24 readbacks, with one output texture per
frame instead of four. Pixel and preset checks pass in both paths.

This is a 75% reduction in these operations for matching viewers, not a measured
75% reduction in total application CPU, memory or end-to-end latency. Codec work,
transport and receiving are outside this comparison, and differing geometries
still require separate output. The historical full-session scorecard below has
not been rerun for this change.

## UI and preview follow-up (2026-11-05)

A local four-viewer status fixture counted 66 diagnostic snapshots in one second
before cadence separation and ten afterward; runtime/control advances stayed at
about 65–67 per second in both runs. The requested five-millisecond media/control tick is unchanged.
The GUI also copies its snapshot only at its existing 100ms status publication
cadence, while frame handoff continues independently. This measures avoided
snapshot work, not a whole-application CPU or latency improvement.

Small source previews now fit on the GPU before BGRA readback, preserving aspect
ratio and avoiding capture upscaling. Generated-window WGC measurements:

| Source | Preview | Readback bytes before / after |
| --- | --- | --- |
| 1266 × 713 | 639 × 360 | 3,650,560 / 921,600 |
| 386 × 793 | 175 × 360 | 1,319,552 / 276,480 |
| 286 × 193 | 286 × 193 | 222,336 / 222,336 |

The host monitoring preview is limited to five updates per second, matching the
creation preview rather than separately copying about 30 full-resolution frames
per second. Source-picker thumbnails use a 320 × 180 bound. Stream capture and
viewer video retain their configured rate and dimensions. These checks do not
establish a particular game's CPU/GPU savings or WAN performance.

## Audio-worker scheduling follow-up (2026-11-05)

The custom PCM workers did not use the MMCSS scheduling employed by the pinned
WebRTC Windows audio device. A silent, synthetic playback worker under a 750ms
higher-priority task on the same CPU had a maximum callback gap of 229ms before
the fix and 10ms after registering the audio worker with MMCSS. The test pins
the worker after multimedia task registration and checks that it stays on the
contended CPU. The output reserve is only 50ms, so the previous gap is enough
to underrun it even when the network is healthy.

Playout, the capture feeder and device capture producers now register the
thread-scoped Pro Audio task and revert registration on their owner thread.
Unavailable registration is diagnostic and nonfatal. This demonstrates and
addresses CPU-contention starvation; it does not prove what caused an earlier
remote session's first stall or eliminate kernel/driver stalls.

## Latest scorecard — congestion-window follow-up

All 16 uninstrumented 1080p60/12 Mbps controls pass again after reducing v2's
additional in-flight allowance to 50 ms with bitrate pushback. Forward/reverse repeats preserve the
earlier local image-age and hardware CPU improvements. Portable codec/timer fixes
remain in both backends; legacy has no WebRTC congestion controller to configure.
These runs check normal-load regressions, not matched impaired-network superiority.

| Viewers / encoder | Legacy age p95 / v2 (ms) | Legacy fresh FPS / v2 | Legacy CPU / v2 (one core = 100%) | Legacy private MiB / v2 |
| --- | --- | --- | --- | --- |
| 1 / software | 45.3 / 29.8 | 52.5 / 52.5 | 184 / 119 | 435 / 311 |
| 1 / hardware | 44.5 / 29.2 | 53.1 / 52.1 | 57 / 24 | 333 / 239 |
| 4 / software | 49.9 / 41.5 | 49.2 / 46.1 | 406 / 530 | 492 / 912 |
| 4 / hardware | 48.7 / 33.0 | 49.9 / 51.9 | 274 / 95 | 407 / 531 |

Numbers are means of two runs, same-process generated WGC capture to CPU image
consumption. Legacy still decodes in software; v2 prefers hardware decode.
Configured ceilings are equal, actual rate/quality are not: one-viewer software
luma PSNR is 26.7 dB legacy versus 23.1 dB v2. Four-viewer software CPU remains
higher and fresh FPS is about 6% lower than legacy in this matrix, though both pass
the unchanged workload gate. Do not call this a universal quality/resource win.
Relative memory optimization remains backlogged. The historical compact evidence
retains hashes, per-run metrics and limitations in Git at commit `376d59a`.


## Matched network impairment (2026-09-19)

**Retain v2.** All 32 selected runs validate, with forward/reverse order for each
configuration. V2 sustains motion under both impaired links while legacy mostly
freezes. These are medians of the two runs' affected-viewer held-image p95 and
fresh FPS during the 12-second impairment, with legacy / v2 shown in each cell:

| Link / viewers / encoder | Held-image p95 (ms) | Fresh FPS |
| --- | --- | --- |
| Collapse / 1 / hardware | 11,179.5 / 187.1 | 1.2 / 28.2 |
| Collapse / 1 / software | 11,207.9 / 260.8 | 1.1 / 19.5 |
| Collapse / 4 / hardware | 11,172.0 / 194.8 | 1.2 / 28.5 |
| Collapse / 4 / software | 11,205.0 / 226.1 | 1.1 / 24.6 |
| Loss / 1 / hardware | 6,211.5 / 157.5 | 0.5 / 51.2 |
| Loss / 1 / software | 6,207.7 / 185.5 | 0.5 / 49.0 |
| Loss / 4 / hardware | 7,642.6 / 166.0 | 0.2 / 51.0 |
| Loss / 4 / software | 5,110.0 / 192.7 | 0.3 / 47.4 |

The result is not an every-metric win: after collapse, software legacy has lower
recovery-phase held-image p95 (77.9 vs 121.6 ms for one viewer; 86.8 vs 108.7 ms
for four) and higher recovery FPS. V2's reduction in freezing during impairment
is the principal improvement. The configured link is not a claim of real WAN
coverage, and these transient numbers do not replace the separate 150 ms settled
age gate or external gaming-latency measurement. Four-viewer software v2 also
uses more CPU (522–544% of one core versus legacy 360–365%) and private memory
(904–931 MiB versus 495–501 MiB). The hardware v2 configurations use less CPU;
healthy viewers remain responsive in both paths. These resource differences are
included in the evidence rather than hidden by the improved freeze metric.


Historical reports and raw evidence indexes are retained in Git at commit `376d59a`.
Reproduction tools remain in `scripts/compare-backends.py` and
`scripts/compare-impaired-backends.py`.
