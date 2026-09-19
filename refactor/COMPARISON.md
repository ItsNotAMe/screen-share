# Before/after validation scorecard

Status: **Retain modular capture. V2 now has lower local image age in all four
matched configuration groups, and hardware uses substantially less CPU.
Matched packet impairment also strongly favors v2 for continuity; see
[MATCHED-NETWORK.md](MATCHED-NETWORK.md). Laptop hardware resources and physical/
Internet acceptance remain open.** Legacy
receives the portable timer, sender-mode, hardware-queue, decoder and software
CABAC fixes too. These are measured workload results, not a universal
architectural win or permission to waive the remaining acceptance gates.
User requested comparative validation as part of B on 2026-09-15. Passing a
component test or Gate A is not a comparative performance result.

User priority update (2026-09-18): memory optimization relative to legacy is
deferred to [BACKLOG.md](BACKLOG.md). The measured 522/924 MiB hardware/software
private footprint covers the host plus four local receivers. Practical usability
and absence of unbounded growth remain required; beating legacy's memory number
does not. The congestion follow-up is documented in CONGESTION-WINDOW.md.

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
Relative memory optimization remains backlogged. Raw reports:
`build/congestion-rate-fair`; the compact
[congestion evidence](evidence/congestion-window-2026-09-19.json) retains hashes,
per-run metrics and limitations, including the earlier drop-only candidate.

## Previous scorecard — software efficiency and bounded readback allocation

All **16 new uninstrumented runs** pass workload and actual-codec validation.
Both backends receive shared software CABAC and unchanged-bitrate fixes. V2
additionally preserves codec history across small input-cadence changes and
reuses its GPU readback staging allocation. See
[SOFTWARE-THROUGHPUT.md](SOFTWARE-THROUGHPUT.md) for diagnosis and rejected experiments.

| Variant | Viewers | Image-age p95 ms | Fresh-marker FPS | CPU, one-core % | Private MiB |
|---|---:|---:|---:|---:|---:|
| Improved legacy software | 1 | 46.0 | 51.5 | 181.4 | 437.1 |
| Improved legacy hardware | 1 | 43.0 | 53.0 | 53.6 | 333.8 |
| V2 software | 1 | 29.6 | 52.9 | 141.6 | 306.5 |
| V2 hardware | 1 | 27.8 | 52.9 | 25.7 | 244.3 |
| Improved legacy software | 4 | 48.8 | 49.6 | 391.9 | 500.5 |
| Improved legacy hardware | 4 | 48.2 | 51.5 | 262.3 | 397.6 |
| V2 software | 4 | 40.7 | 48.2 | 559.3 | 923.7 |
| V2 hardware | 4 | 30.8 | 52.1 | 99.4 | 522.4 |

Same 1080p, 60 FPS / 12 Mbps ceilings, generated motion, five-second warmup,
twenty-second measurement, forward/reverse order and requested timer policy.
Each cell is the median of two runs; image age uses each run's worst viewer.
CPU covers host plus receivers, minus the scene thread, and is not GPU load.
Image age ends at CPU consumption, not physical display. Equal configured
ceilings are not equal measured bitrate or identical codec/decoder scheduling.

**Verdict:** hardware v2 is approximately 35–36% lower in local image age and
52–62% lower in measured CPU than improved hardware legacy, at similar fresh
delivery. Software v2 rises from 36.7/35.9 to 52.9/48.2 fresh FPS for one/four
viewers; local image age falls from 61.1/65.8 to 29.6/40.7 ms. Keep the modular
capture/ownership wrapper; these fixes do not require reverting it.

**Remaining tradeoff:** four software viewers use about 43% more CPU and 85%
more private memory than software legacy. Four hardware viewers use about 31%
more private memory than hardware legacy. V2 has independent per-peer encoders
and GPU decoders; legacy shares an encoder and decodes in software. Bounded
readback scratch reuse removes allocation churn, but does not close total-memory
acceptance. Do not present all modes as better on every metric.

Sampled luma PSNR is 26.1/26.3 dB for v2 software versus legacy's 26.7/26.8,
and 28.0/28.0 for v2 hardware versus 28.2/28.2. These samples do not establish
equal full-color quality at matched actual bitrate. Raw controls are preserved
in `build/software-memory-fair`; compact evidence is in
[software-throughput-2026-09-18.json](evidence/software-throughput-2026-09-18.json).
Normal GPU presentation, game load, sustained resources, congestion latency and
physical image/input acceptance remain open before default cutover.

## Earlier scorecard — complete-picture decoder on both backends

All **16 new uninstrumented runs** pass workload and codec validation. The
workload and alternating order below are unchanged. Both backends now use
complete-picture H264 decoder input and a verified low-latency setting, in
addition to the prior shared improvements. [CAPTURE-LATENCY.md](CAPTURE-LATENCY.md)
records the isolated capture measurements, stage traces and frame-retention bug.

| Variant | Viewers | Image-age p95 ms | Fresh-marker FPS | CPU, one-core % | Private MiB |
|---|---:|---:|---:|---:|---:|
| Improved legacy software | 1 | 43.3 | 53.2 | 155.5 | 438.6 |
| Improved legacy hardware | 1 | 44.1 | 53.3 | 55.5 | 338.6 |
| V2 software | 1 | 61.1 | 36.7 | 92.3 | 315.4 |
| V2 hardware | 1 | 50.8 | 52.6 | 26.6 | 255.1 |
| Improved legacy software | 4 | 47.8 | 52.8 | 348.7 | 489.5 |
| Improved legacy hardware | 4 | 47.7 | 53.5 | 269.2 | 399.8 |
| V2 software | 4 | 65.8 | 35.9 | 366.8 | 916.2 |
| V2 hardware | 4 | 44.7 | 52.5 | 100.3 | 540.4 |

Values are medians of two runs (worst-viewer p95 per run for image age), not
pooled percentiles. V2 hardware's individual p95 values were 47.8/53.8 ms for
one viewer and 39.2/50.2 ms for four, so the approximately 3 ms four-viewer
advantage is not a robust universal latency win. Legacy hardware's corresponding
runs were 43.7/44.5 and 47.8/47.6 ms.

**Decision:** retain the shared `DesktopCapturer` behind the modular owned-frame
wrapper. Capture-only p95 was approximately 25.2 ms for legacy configuration
and 24.6 ms for owned configuration; replacing safe ownership with the borrowed
legacy pool would not address the measured delay. Decoder p95 fell from
34–38 ms to below 0.5 ms in hardware stage traces. Legacy gets the same fix.

V2 hardware image age falls from 72.4/78.8 to 50.8/44.7 ms in the fair controls;
legacy hardware also falls from 59.6/63.9 to 44.1/47.7 ms. V2's one-viewer gap
is now about 6.7 ms. Its measured CPU is 52–63% lower, while four-viewer private
memory remains about 35% higher. Software image age improves substantially but
fresh delivery still trails legacy (about 36 versus 53 FPS); do not credit its
lower one-viewer CPU without that qualification.

Raw new controls: `build/comparison-decoder-fair`. The stage/capture diagnostics
are separate from these uninstrumented measurements. The
[compact evidence](evidence/capture-decoder-2026-09-18.json) retains the old/new
fair summaries, individual results, source/binary/report hashes and validation.
Normal GPU presentation, game load, equal measured bitrate/full-color quality,
physical latency, sustained resources and network acceptance remain open.

## Earlier scorecard — before the complete-picture decoder fix

All **16 updated runs** pass workload/codec validation, in addition to the sixteen
pre-fix controls below. Motion scene, 1080p, 60 FPS and 12 Mbps ceilings,
five-second warmup, twenty-second measurement, one/four viewers, forward/reverse
variant order and explicit timer policy are unchanged. Legacy hardware now uses
the exact same one-submission/output-deadline helper as v2; its old input queue
is removed in production code. Legacy frame-clock, WGC and hardware waits also
use the precise timer. Both legacy variants select the existing low-latency mode.

| Variant | Viewers | Image-age p95 ms | Fresh-marker FPS | CPU, one-core % | Private MiB |
|---|---:|---:|---:|---:|---:|
| Improved legacy software | 1 | 59.7 | 52.9 | 155.7 | 432.0 |
| Improved legacy hardware | 1 | 59.6 | 53.2 | 53.7 | 335.3 |
| V2 software | 1 | 109.1 | 36.7 | 84.7 | 314.5 |
| V2 hardware | 1 | 72.4 | 52.1 | 26.8 | 254.1 |
| Improved legacy software | 4 | 65.2 | 49.6 | 357.1 | 512.0 |
| Improved legacy hardware | 4 | 63.9 | 51.7 | 254.1 | 395.7 |
| V2 software | 4 | 120.9 | 36.9 | 379.6 | 933.3 |
| V2 hardware | 4 | 78.8 | 51.7 | 106.4 | 574.4 |

Cells are medians of two runs; latency uses each run's worst-viewer p95. CPU
covers host plus all local receivers, minus the scene thread, and excludes GPU
utilization. Image age ends at CPU consumption. These are not external latency
or full color/equal-measured-bitrate quality results.

**Conclusions:**

- The same portable fixes bring legacy hardware from 240.7/244.4 ms to
  59.6/63.9 ms, while preserving fresh-image delivery. All four final legacy
  hardware logs report zero queued encoder inputs, versus ten before the fix.
  The large legacy hardware delay was not an unavoidable cost of hardware encoding.
- Within v2, hardware remains the better tested choice: about 52 fresh FPS and
  72–79 ms versus software's 37 FPS and 109–121 ms. Software fallback is functional
  but does not meet equivalent 1080p60 delivery in this workload.
- V2 hardware still trails improved legacy hardware by about **13–15 ms**. It
  uses about 50–58% less measured CPU, but different decoder and per-viewer encoder
  architectures prevent attributing that solely to framework efficiency. Four-viewer
  private memory is about **45% higher** than improved legacy hardware.
- Performance acceptance remains **open**. Next work is the measured v2 latency
  gap, software-throughput deficit and four-viewer memory—not another claim that
  unchanged legacy is slower. Retain physical/game-load, GPU-presentation and
  matched-network acceptance separately.

[Compact before/after evidence](evidence/codec-portable-controls-2026-09-18.json)
preserves both matrices, hashes, actual codec observations, source snapshots'
hashes, queue counts, the ten-minute hardware summary and Release/Debug checks.
Raw final runs: `build/comparison-portable-final`. An earlier restricted attempt
in `build/comparison-portable-fixes` failed capture/admission and was stopped;
it remains preserved and is excluded from performance results. The identical
executable completed the final matrix with the required capture/network access.

## Matched production-path workload

`BackendComparison` now invokes the retained legacy `RunShareSession` /
`RunWatchSession` APIs and v2 `RoomSession` / `WindowsRoomRuntimeFactory` in an
optimized application build. It captures only a generated, owned WGC window;
there is no desktop capture, audible output or physical input injection.

## Codec controls before the additional legacy fixes — 2026-09-18

These controls precede the subsequent shared hardware-queue/frame-wait fixes.
Preserve them as the symptom baseline, not the final improved-legacy verdict.
The user's fairness objection was correct: timer precision and less buffering
are portable improvements. `scripts/compare-codec-controls.py` runs four variants
in forward/reverse order with one/four viewers, for **16 validated runs**. All
use the same motion scene, 1080p, 60 FPS ceiling, 12 Mbps ceiling, five-second
warmup and twenty-second measurement. Every test process explicitly honors its
requested timer resolution, including when its owned window is occluded. This
does not change the machine's timer/power policy.

Legacy controls enable its existing low-latency option (no UDP pacing queue).
The hardware control additionally overrides the typed preset's software choice
using the existing legacy runtime option. V2 changes only encoder preference;
its decoder remains hardware-preferred. Actual codec selection is checked, and
any v2 software fallback invalidates a codec-control run. Legacy still uses a
software decoder and shares one encoded stream among viewers; v2 has independent
per-viewer encoders. These are explicit differences, not an isolated architecture
or equal-wire-bitrate experiment.

| Variant | Viewers | Image-age p95 ms | Fresh-marker FPS | CPU, one-core % | Private MiB |
|---|---:|---:|---:|---:|---:|
| Tuned legacy software | 1 | 59.8 | 53.1 | 153.2 | 433.3 |
| Tuned legacy hardware | 1 | 240.7 | 53.3 | 54.5 | 336.1 |
| V2 software | 1 | 114.5 | 36.5 | 94.9 | 317.9 |
| V2 hardware | 1 | 74.8 | 51.8 | 27.2 | 252.4 |
| Tuned legacy software | 4 | 65.3 | 52.2 | 351.4 | 494.3 |
| Tuned legacy hardware | 4 | 244.4 | 53.3 | 262.8 | 405.9 |
| V2 software | 4 | 123.0 | 38.5 | 407.5 | 939.0 |
| V2 hardware | 4 | 73.1 | 52.1 | 100.0 | 565.5 |

Values are medians of two runs; latency uses each run's worst-viewer p95.
Fresh-marker FPS counts distinct captured source images, not duplicate delivery.
The measured age ends at CPU image consumption, not physical presentation.
Sparse grayscale PSNR is retained in the evidence but is not full image-quality
acceptance at equal measured bitrate.

**Verdict:** hardware is the better tested encoder choice within v2 on this
desktop, and its legacy lag is reproducible. However, tuned legacy software is
about 15 ms faster with one viewer and 8 ms faster with four. V2 software also
falls well below the requested 60 FPS workload. Lower CPU usage does not waive
these latency/delivery gaps; four-viewer private memory remains higher for v2
hardware than tuned legacy software. Do not mark the backend universally better
or close performance acceptance using the default-configuration table below.

The hardware queue diagnosis, failure checks and sustained-run boundaries are
in [HARDWARE-ENCODING.md](HARDWARE-ENCODING.md). Next performance work should target
the measured v2 latency and software-fallback throughput, followed by GPU-display
and real two-machine gaming load. No additional comparison harness is needed.

```powershell
python scripts/compare-codec-controls.py build/sdk-app-release/BackendComparison.exe build/codec-controls-NEW --origin https://screenshare-signaling-v2.bit-yeet.workers.dev
```

## Default-configuration matrix methodology

The first matrix uses 1920×1080, 60 FPS, a 12 Mbps per-viewer video ceiling,
encrypted loopback transport, five seconds of warmup and twenty measured seconds.
Static, scrolling and high-motion grayscale scenes each run with one and four
viewers, in legacy/v2/v2/legacy order. Every run starts a fresh process. Window
updates target 60 Hz; actual source updates are reported rather than assumed.

A complementary binary marker identifies each generated source image. A shared
monotonic clock measures scene-update-to-CPU-image-consumption age. Reports retain
frame counts, unique markers, invalid markers, p50/p95/p99 age, sampled grayscale
error, process CPU, separately measured scene-thread CPU, private memory, working
set, handles and teardown observations. Summary latency is the median of each
run's worst-viewer p95, not a pooled percentile. A completed run is not a pass of
the original physical-latency or stability thresholds.

V2 uses `SilentPcmCapture` and the shared **paced** `DiscardPcmPlayout`. A no-op
audio write is not equivalent: the ADM expects device pacing and otherwise spins
and advances playout too quickly. Corrected reports require `audioPlayoutMode`
to identify this contract. Optional sender fields locate encode, packet-send and
receiver-buffer contributions; none is a full queue-age bound. Surviving-thread
CPU attribution excludes exited/new threads and supplements total process CPU.

The native executable also accepts a final `retained-only` argument for resource
isolation. That mode consumes frame metadata without converting pixels and reports
only delivery FPS/resources. It does not render, validate image quality or measure
image age; the paired-image validator explicitly rejects it. Comparing it with
CPU consumption estimates readback/validation overhead, not GPU-display cost.

Important comparison boundaries:

- This compares the retained legacy path and v2 at the same current source
  revision, not an old released binary. Shared platform fixes apply to both.
- The legacy typed application API explicitly selects software H264. V2 uses
  its production hardware-preferred path. Codec paths are recorded; this is a
  comparison of actual application defaults, not matched encoder implementations.
- The CPU consumer reads decoded pixels for both backends. It forces readback
  of v2 GPU frames; results must not be extrapolated to zero-readback GPU display.
- Both video ceilings are 12 Mbps, but actual output/wire rates can differ.
  Sparse grayscale error does not establish full color/image quality at equal
  measured bitrate. No encoder quality win is inferred from it alone.
- Local image age excludes physical presentation and input. Audio is absent on
  legacy and a discarded silent track on v2. Four-viewer CPU covers host and all
  receivers in one process; scene-thread CPU is separated, GPU/DWM work is not.
- V2 admission uses the isolated HTTPS service; legacy uses direct local targets.
  Startup times are recorded but are not a matched signaling comparison.

```powershell
cmake --build build/sdk-app-release --target BackendComparison
python scripts/compare-backends.py build/sdk-app-release/BackendComparison.exe build/comparison-NEW --origin https://screenshare-signaling-v2.bit-yeet.workers.dev
```

The runner preserves failed runs, hashes the executable/source/reports, rejects
missing or mismatched measurements, and never produces an aggregate "better"
score. Its default matrix is 24 runs. Use `--scene scroll --viewers 1` for a
four-run focused comparison; keep at least two alternating rounds.

## Corrected reference scorecard — 2026-09-18

All **24 corrected runs** pass workload/evidence validation, with two alternating
repeats per backend in each group. The same source, output settings, durations and
image checks apply to both backends. Each cell is **legacy → v2**. CPU is percent
of one core after subtracting scene-thread work; memory is private process MiB,
not total CPU+GPU memory. Age ends at CPU image consumption, not physical display.

| Scene / viewers | Image-age p95, ms | Fresh-marker FPS | CPU, one-core % | Private MiB |
|---|---:|---:|---:|---:|
| Static / 1 | 80.9 → 67.3 | 51.1 → 49.2 | 105.6 → 20.2 | 435.0 → 269.6 |
| Static / 4 | 73.9 → 62.3 | 51.0 → 51.5 | 274.1 → 83.6 | 491.8 → 582.0 |
| Scrolling / 1 | 64.9 → 59.2 | 52.8 → 52.4 | 102.4 → 22.3 | 429.5 → 260.1 |
| Scrolling / 4 | 73.2 → 63.0 | 51.7 → 50.9 | 248.3 → 89.8 | 496.3 → 573.0 |
| High motion / 1 | 85.2 → 72.1 | 52.5 → 52.1 | 164.3 → 25.7 | 433.0 → 255.2 |
| High motion / 4 | 91.3 → 74.1 | 50.1 → 52.5 | 369.4 → 101.7 | 496.9 → 556.8 |

V2 CPU is 64–84% lower; one-viewer private memory is 38–41% lower. Four-viewer
private memory is still 12–18% higher in this combined host-plus-receivers process.
Fresh-frame differences range from about -3.7% to +4.8%; lower age was not obtained
by lowering configured resolution/FPS or accepting missing pixels. Source updates
remain about 59.7–59.9 Hz. Legacy's approximately 60 output FPS includes duplicates.
Sampled grayscale quality is essentially equal for static/scrolling content;
high-motion PSNR is 23.2→27.2 dB (one viewer) and 23.4→27.0 dB (four). Actual bitrates
and encoders differ, so this does not establish equal-bitrate full-color quality.

### Fixes and causality limits

- **Sender pacing:** pinned screen-content defaults use a 1x factor and 2875 ms
  drain horizon. The production engine now uses `2.5,200,80,40,-60,3` through its
  owned field-trial view. Congestion feedback and video/aggregate ceilings remain
  active. The 200 ms parameter accelerates draining; it does not expire packets.
  With the corrected sink but default pacing, a diagnostic motion run still had
  310.5 ms image-age p95 and 107.9 ms mean packet-send delay, with 2.8 ms encoding.
  This diagnostic preceded the final silent-audio timer change, so it is not an
  isolated estimate of pacing's exact contribution to the final scorecard.
- **Background waits:** encoder polling, capture GPU completion, capture backoff
  and device-free PCM pacing now use owned high-resolution waitable timers.
  Silent/occluded Windows processes can lose ordinary short-sleep precision;
  [Microsoft documents that policy](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessinformation).
  The private timer uses the [high-resolution timer API](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)
  (Windows 10 1803+), not a process-wide power-policy override or busy loop.
  Non-Windows portable owners retain a standard sleep fallback. Cancellation is
  checked between bounded waits; resources close with their worker/endpoint.
- **Benchmark correction:** the no-op audio sink was replaced by the existing
  paced `DiscardPcmPlayout`. This is a fixture correction, not a production CPU
  optimization. Earlier CPU and synchronized-video claims are superseded. The
  final legacy/v2 pairs, not the old-v2/new-v2 difference, support the wins above.

Release and Debug each pass seven targeted media/capture/settings/deadline tests
and all nine evidence-validator cases. Under forced timer throttling, median
short waits are 1.03/1.07 ms, twenty PCM blocks take 206.5/207.5 ms, and worker
timer handles are released. The final Release packet matrix passes 5/5; Debug
collapse also passes. First impaired internal response is 364 ms Release and
718 ms Debug; subsequent impaired samples are 57–63 / 84–94 ms. These remain
small synthetic samples, not physical input-latency or complete congestion acceptance.

Raw evidence: `build/comparison-corrected-final`,
`build/webrtc/reference-final-{release,debug}`. The earlier partial pacing-only
matrix under `build/comparison-pacing-fixed` was deliberately stopped after the
background-wait regression repeated; its failed performance conclusion is preserved.

[Compact corrected evidence](evidence/backend-comparison-corrected-2026-09-18.json)
retains run/report hashes, per-viewer values, source hashes, hardware inventory,
packet checks and outstanding acceptance flags.

Four extra high-motion/four-viewer runs alternate legacy/v2/v2/legacy with retained
metadata consumption only. V2 uses 76.0% of one CPU core versus legacy's 355.5%;
private memory is 537.9 versus 501.8 MiB. Compared with v2's CPU-pixel workload
(101.7%, 556.8 MiB), readback plus image checking accounts for about 25.7 CPU
percentage points and 18.9 MiB in these separate runs. It does not explain the
entire memory difference. These runs do not measure GPU presentation, and the
remaining 36.1 MiB is not attributed to a specific component without profiling.

The rebuilt native runtime, CLI media and headless Qt UI checks also pass 3/3 in
both Release and Debug. Normal GPU/two-PC resource, immediate capture-handle,
physical input/display/A/V, device and Internet/NAT gates remain open.

## 2026-09-18 initial scorecard — superseded fixture

**Do not use this initial matrix to judge production performance.** Its custom
silent playout endpoint returned immediately instead of pacing 10 ms PCM blocks.
Each viewer could therefore spin an audio worker and advance audio playout too
quickly. This contaminates CPU and synchronized-video measurements. Raw evidence
is retained below for audit, not deleted or reclassified as a passing comparison.
The corrected fixture uses the existing `DiscardPcmPlayout`; its validator now
rejects reports without the explicit paced-playout mode. Use the corrected
scorecard above.

All 24 workloads completed and passed evidence validation. The replacement is
**not yet demonstrated better overall**. Raw artifacts are under
`build/comparison-1080-release/`; the compact evidence file records hashes and
per-run values. Source/runner snapshots preserve the exact measured revision.
[Compact evidence](evidence/backend-comparison-2026-09-18.json) includes hardware
inventory, run/report hashes, per-viewer metrics and explicit unmeasured fields.
Both Release and Debug comparison targets build; seven validator cases pass in
each configuration. A final four-run short alternating smoke test also passes
after cleanup-lifetime hardening and extra codec metadata were added to the harness.
No production backend policy was changed in this comparison group.

Each cell is **legacy → v2**, using medians over two runs per backend. CPU is
percent of one core after subtracting the source drawing thread; private memory
does not include GPU memory. Image age ends at CPU consumption, not the display.

| Scene / viewers | Image-age p95, ms | Fresh-marker FPS | CPU, one-core % | Private MiB |
|---|---:|---:|---:|---:|
| Static / 1 | 59.4 → 62.3 | 53.3 → 52.9 | 93.6 → 121.4 | 428.9 → 259.6 |
| Static / 4 | 63.6 → 67.2 | 52.3 → 52.2 | 255.7 → 485.6 | 496.1 → 609.4 |
| Scrolling / 1 | 61.9 → 64.8 | 54.0 → 52.2 | 86.1 → 123.0 | 433.1 → 267.9 |
| Scrolling / 4 | 72.3 → 67.4 | 52.9 → 52.9 | 236.4 → 489.0 | 490.6 → 603.6 |
| High motion / 1 | 81.8 → 365.1 | 53.4 → 47.8 | 154.5 → 124.3 | 427.7 → 266.7 |
| High motion / 4 | 92.6 → 379.6 | 49.8 → 47.7 | 357.4 → 500.8 | 500.4 → 617.0 |

The source itself sustained roughly 59–60 updates/sec. Legacy's ~60 output FPS
includes duplicate content; fresh markers are reported separately. High-motion
sampled grayscale PSNR was 23.1→27.5 dB for one viewer and 23.4→26.8 dB for four.
That is not a free quality gain: v2 delivers fewer fresh frames, actual bitrates
are not matched, and the encoders differ. Static/scroll quality samples are
essentially equal between paths.

### Historical investigation prompted by the superseded results

1. **Investigate and fix high-motion v2 image age before cutover.** Both repeats
   reproduce it (one-viewer p95 342/388 ms). The workload is local and stable;
   passing synthetic 640×360 network tests did not expose this reference-load issue.
2. **Separate CPU-readback overhead from normal GPU-presentation cost**, then fix
   the measured four-viewer CPU/memory regressions that persist on the normal path.
   Do not assume this readback-heavy diagnostic represents zero-copy presentation.
3. Re-run the affected matched pairs after fixes. Preserve quality/FPS, source
   progress and ownership checks; do not claim success by reducing workload.
4. Keep the separate initial-congestion backlog, resource, physical input/audio,
   external-latency and network/service acceptance gates open. There is no reason
   to add unrelated backend features or restart completed soak/harness work.

## Current evidence

| Requirement | Legacy evidence | V2 evidence | Verdict |
|---|---|---|---|
| Gaming capture-to-display p95 <80 ms | No external measurement | No external measurement | Unmeasured |
| Gaming input-to-visible-response p95 <120 ms | No external measurement | No external measurement | Unmeasured |
| Quality capture-to-display p95 <250 ms | No external measurement | No external measurement | Unmeasured |
| Image quality at equal bitrate | Matched-ceiling grayscale samples above | Same sampled scene; different codec path and actual rate | Full color/equal-wire-rate quality remains unmeasured |
| Sustained delivery and stale queue age | Improved software age 43–48 ms; hardware 44–48 ms | Hardware 45–51 ms, software 61–66 ms with lower fresh FPS; earlier ten-minute hardware evidence available | Shared decoder delay fixed; one-viewer gap and software delivery remain; full-queue/physical gates open |
| CPU/GPU/memory efficiency | Tuned control CPU/private-memory samples above | Hardware uses less CPU; four-viewer private memory is higher; software four-viewer cost is worse | CPU-consumer result only; normal GPU presentation and GPU utilization remain unmeasured |
| Native resource lifetime | No matched old-backend soak | 100 full-room restarts pass the handle bound in both builds (+5); extended 500-cycle capture fails (+226). Reproduced retained ports trace to WGC/RPC; cleanup experiments still fail the immediate bound. See CAPTURE-HANDLES.md | Allocation origin and delayed cleanup are identified; immediate resource acceptance and room memory remain open. Relative comparison unavailable |
| Congestion recovery and viewer isolation | No matched impairment run | Packet impairment/recovery and four-peer rejoin pass; initial-collapse backlog remains | Matched network comparison unmeasured |
| Room responsiveness/free-tier cost | No matched workload report | Authenticated push, heartbeat/listing invariants and eight-hour model verified | Matched deployed usage/billing comparison unmeasured |

The v2 CPU presentation handoff now demonstrably avoids its previous NV12→I420→NV12
round trip and extra UI pixel-vector copy. Tests assert original pixel-pointer
identity and zero conversion/repack counts on actual decoded frames; Windows UI
and CLI tests measure DXGI maximum frame latency 1. CLI `presentation` counters
and Qt frame/presentation statistics expose those local operations. This is a
specific implementation improvement, not a matched legacy/v2 CPU/GPU or external
latency result. Software decode and CPU-to-GPU upload still occur.

UI and CLI now use one backend-owned native renderer and recovery session; the
duplicate CLI D3D pipeline is removed. Injected GPU-loss/resize tests demonstrate
bounded recovery and terminal handling in both frontends. This reduces duplicated
implementation and closes a CLI correctness gap, but does not establish a CPU/GPU
speedup, physical-driver recovery or end-to-end gaming latency improvement.

Historical baseline details are in CLOSEOUT-A.md. It used local plaintext,
mostly unchanged desktop content, no presentation/audio and a Debug binary.
Do not compare its averages to V2 hardware maxima, synthetic capture timing or
encrypted media counters as if conditions were equal. The current resource
failure remains an open acceptance failure, regardless of the old backend.

## Required paired runs

1. Record both commit/binary hashes, configuration, hardware/drivers, source
   scene, codec, output dimensions/FPS, bitrate limit, audio mode, viewer count,
   transport encryption, network path and impairment seed/schedule.
2. Run both implementations on the same machine pair and deterministic scene.
   Use optimized Release builds and the same warmup/sample duration. Alternate
   order and repeat runs to expose variance; keep other workloads consistent.
3. Cover unchanged desktop, moving text/scrolling and high-motion content at
   matched settings. Preserve reference and received frames for quality review;
   latency gains must not be credited to accidental frame-rate/quality reduction.
4. Measure one and four viewers, bandwidth collapse/recovery, loss/jitter,
   late join/rejoin, slow receiver, source closure and long-session teardown.
   Separate controlled source delay from actual decoder/network impairment.
5. Report sample counts, p50/p95/p99 and worst queue age alongside CPU/GPU,
   memory/handle trends, actual resolution/FPS, video/wire bitrate, drops and
   recovery time. An unavailable counter is null/unmeasured, not zero.
6. Measure actual display/input response on real machines with an external
   camera/clock method and a deterministic test-owned response scene. Internal
   monotonic timestamps are separate pipeline estimates; clocks on different
   machines are not assumed synchronized. Never inject into unrelated apps.
7. Keep raw artifacts and a per-requirement verdict: pass, regression,
   unmeasured or not comparable. Do not create a single aggregate score that
   hides a latency, quality, security or stability regression.

## Cutover rule

The application stays on legacy media until the replacement's integration and
acceptance evidence supports switching it. Preserve the existing latency,
quality, security and stability requirements. A smaller latency number alone
does not establish improvement. Known native handle growth must be resolved or
fully accounted for against the required teardown/soak criteria before cutover.

Full-room accounting now checks application-owned dependencies and separates busy
heap allocations, free heap space and later decommit. Slow consumption uses the
actual UI/CLI bounded presentation buffer. These are v2-only diagnostics; see
ROOM-STRESS.md for passes, retained failures and limitations. They establish neither
a legacy comparison nor the two-hour/network/latency acceptance gates.

The v2 receive-buffering regression now has a measured before/after correction:
audio timeout silence followed by immediate late PCM overfed the audio timeline;
one output cadence preserves normal A/V synchronization without accumulating delay.
See RECEIVER-PACING.md for the matched 180-second software scenario and exact
buffering/FPS results. This is a v2 defect fix, not a legacy comparison or external
capture-to-display/input-response latency measurement.

The production paths are now connected to the matched workload above. Remaining
comparison scope includes two-machine GPU presentation, physical latency,
matched network impairment, audio, sustained resource lifetime and service costs.
The current headless suite remains regression evidence, not a substitute for those
measurements.
