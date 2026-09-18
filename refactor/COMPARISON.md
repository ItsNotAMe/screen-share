# Before/after validation scorecard

Status: **The replacement is not yet proven better than the legacy backend.**
User requested comparative validation as part of B on 2026-09-15. Passing a
component test or Gate A is not a comparative performance result.

## Current evidence

| Requirement | Legacy evidence | V2 evidence | Verdict |
|---|---|---|---|
| Gaming capture-to-display p95 <80 ms | No external measurement | No external measurement | Unmeasured |
| Gaming input-to-visible-response p95 <120 ms | No external measurement | No external measurement | Unmeasured |
| Quality capture-to-display p95 <250 ms | No external measurement | No external measurement | Unmeasured |
| Image quality at equal bitrate | No matched reference capture | No matched reference capture | Unmeasured |
| Sustained delivery and stale queue age | ~60 FPS, 126 ms peak sender queue in old local Debug run | Bounded-frame and recovery proofs, different workload | Not comparable |
| CPU/GPU/memory efficiency | Old CPU time available, GPU unmeasured | Retained NV12 handoff proven; no matched production run | Not comparable |
| Native resource lifetime | No matched old-backend soak | 100 full-room restarts pass the handle bound in both builds (+5); extended 500-cycle capture fails (+226). Reproduced retained ports trace to WGC/RPC; cleanup experiments still fail the immediate bound. See CAPTURE-HANDLES.md | Allocation origin and delayed cleanup are identified; immediate resource acceptance and room memory remain open. Relative comparison unavailable |
| Congestion recovery and viewer isolation | No matched impairment run | Controlled sink/source-delay tests and four-peer rejoin pass | Network comparison unmeasured |
| Room responsiveness/free-tier cost | No matched workload report | Authenticated push transport integrated; no matched service workload/cost report | Unmeasured |

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

Next: connect both integrated session paths to a matched-workload harness. The current headless suite is useful
regression evidence and must remain easy to run, but is not a substitute for
the paired runs or external measurements above.
