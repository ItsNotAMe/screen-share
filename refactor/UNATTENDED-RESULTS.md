# Unattended Stage 2–4 batch — 2026-09-18

Stages 2–4 are **not fully accepted**. This batch completes the available reporting,
input diagnostics, impairment/process harness and service-cost work, and obtains
the two-hour software-path evidence. It does not turn failed performance checks or
unavailable physical checks into passes. No default cutover or deployment occurred.

## Delivered

- UI/CLI redacted, atomic JSON reports, compiled app version and JSON/CLI report
  paths. Input diagnostics include queue pressure, explicit stopping reasons and
  fresh local queue/backend timings. See SESSION-REPORTS.md.
- Recent-interval receiver buffering alongside lifetime averages, through the
  encrypted telemetry channel, diagnostics and reports. Unknown/reset/stale values
  remain unknown. See DIAGNOSTICS.md.
- Real encrypted-packet bandwidth/loss/reorder/duplicate scenarios, four viewers,
  separate receiver processes, bounded packet ownership and an authorized
  input-to-decoded-marker response without physical input or audio output.
  See NETWORK-IMPAIRMENT.md for failures and scope.
- Configurable global room budget, real ten-room heartbeat/listing invariants,
  an eight-hour production-code cost model, and coalesced capacity alarms.
  The model saves 4,320–4,800 alarm writes; admission cleanup still follows the
  last request so rate limits cannot reset early. See SERVICE-COST.md.

## Verification

| Check | Result |
| --- | --- |
| Release and Debug applications | Built successfully |
| Final UI/CLI/input/report headless matrices | 12/12 each (`closeout-final-*`) |
| Worker tests / TypeScript | 201/201 / passed (`stage4-worker-final-201.log`) |
| Native telemetry/report checks | Passed in both builds |
| Impairment evidence / lifecycle evidence tests | 19/19 / 16/16 |
| Final localized-marker network matrix | Release 5/6; Debug 6/6 after explicit-unknown validation correction |
| Two-hour four-viewer software soak + 60-second idle | Passed continuous timing, progress, buffering and tracked-ownership checks after evaluator correction |
| Final rebuilt runtime / input-media proofs | Passed Release/Debug input-media proofs and 60-second four-viewer runs with ten-second idle (`lifecycle-latest-*`); maximum mean buffering 14/13 ms |

The final Release collapse result is a **failure**: baseline ingress averaged
19.10 Mbps, impaired traffic 2.38 Mbps, recovery 3.95 Mbps, below the unchanged
4.4 Mbps recovery requirement. Explicitly unavailable bandwidth or recent-buffer
values during impairment are recorded, not fabricated as zero. Healthy/recovered
telemetry and actual packet traffic remain checked. Original reports are preserved;
`build/webrtc/network-marker-reevaluated.json` records current validation and hashes.
Its compact validation results are committed in
[evidence/network-marker-2026-09-18.json](evidence/network-marker-2026-09-18.json).

## Two-hour evidence

The run uses the cadence-corrected executable associated with `3f5559c`, not the
later telemetry/report build. Its executable SHA-256 and compact results are
committed in [evidence/two-hour-soak-2026-09-18.json](evidence/two-hour-soak-2026-09-18.json).
Raw artifacts are under `build/webrtc/room-soak-two-hour-release/`.

- Active duration: **7,200.01 seconds**, followed by 60 seconds idle.
- 1,438 continuous samples; last sample at 7,197.57 seconds contained 861,411
  consumed frames across four viewers. Normal recovery was approximately 30 fps.
- Slow viewer: about 3.86 fps while the other three stayed about 30.02 fps.
  At most one presentation frame and one capture resource were retained.
- Maximum reported lifetime-mean receiver buffering: **25 ms** (100 ms regression
  guard). This is not an external display/input latency measurement.
- Active median handles: 975 → 972; peak 978. Active median private bytes:
  198,438,912 → 207,585,280, an increase of 9,146,368. Ten-minute medians plateaued
  around 197–198 MiB after initial growth; this does not prove full allocation
  acceptance. Final idle private bytes were 24,944,640 with 348 handles.
- All tracked owners released and all nine memory-accounting snapshots completed.

The original evaluator incorrectly required an ideal five-second sample count.
Real scheduling drift made intervals slightly longer; no records were missing.
The corrected evaluator checks each elapsed-time/resource interval, counter
continuity, maximum gap and end coverage. Tests reject missing and truncated data.
`result.json` retains the original failure; `reevaluated-result.json` contains the
passing reevaluation with original/evaluator hashes. `soakAcceptanceComplete`
remains false because this does not establish every memory/queue/device gate.

## Remaining acceptance work

1. Resolve/reproduce bandwidth recovery and buffering under controlled load;
   measure full queue ages and external Gaming image/input latency, A/V skew and
   a matched legacy baseline. Do not enable unproven jitter/pacing knobs to pass a test.
2. Close the original immediate WGC/RPC handle-bound failure and complete hardware
   capture/source privacy/HDR/DXGI, audio device/format/recovery, and real controller/
   pointer/foreground/virtual-driver checks. The input desktop was rechecked as
   `Screen-saver`; no unlock or physical input was attempted.
3. Validate separate-machine LAN/Internet/TLS/NAT/interface changes and actual
   production hibernation, billed usage and account-wide 50% free-tier headroom.

The authoritative scope remains STAGE-2-4-ACCEPTANCE.md and DETAIL-CHECKS.md.
Default migration, legacy removal and the later appearance redesign remain separate.
