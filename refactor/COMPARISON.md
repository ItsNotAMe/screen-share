# Before/after validation scorecard

Status: **The replacement is not yet proven better than the legacy backend.**
User requested comparative validation as part of B on 2026-09-15. Passing a
component test or Gate A is not a comparative performance result.

## Matched production-path workload

`BackendComparison` now invokes the retained legacy `RunShareSession` /
`RunWatchSession` APIs and v2 `RoomSession` / `WindowsRoomRuntimeFactory` in an
optimized application build. It captures only a generated, owned WGC window;
there is no desktop capture, audible output or physical input injection.

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

## 2026-09-18 measured scorecard

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

### Closeout decisions from these results

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
| Sustained delivery and stale queue age | Paired local 1080p frame/marker counts and image age | Same workload; high-motion age and fresh-FPS regressions | Short-run comparison available; sustained/full-queue and physical gates remain |
| CPU/GPU/memory efficiency | Paired process CPU/private-memory samples above | Lower one-viewer private memory; four-viewer CPU/private-memory regressions | CPU-consumer result only; normal GPU presentation and GPU utilization remain unmeasured |
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
