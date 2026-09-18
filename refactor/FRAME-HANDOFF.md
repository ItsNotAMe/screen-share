# Decoded-frame handoff ownership and timing

The shared UI/CLI `LatestRoomVideoFrame` now reports the actual pending slot,
in-flight conversion, completed delivery, replacement, failed conversion and
stop-discard counts. A snapshot preserves this identity:

`received = pending + inFlight + delivered + replaced + failed + discardedOnStop`

Previously, a failed conversion or stopping with a pending frame left an apparent
outstanding frame when callers subtracted delivered/replaced from received.
Stop is idempotent. It discards pending work and rejects later callbacks; a Take
already in progress keeps ownership until completion. This does not cancel an
in-progress conversion or allocate another queue. Presentation has one consumer.

Timing uses local `steady_clock`, starting when a decoded frame enters the slot
and ending when Take removes it. Replacing the slot replaces its arrival time.
The report carries the current pending age (null when empty), last consumed wait
(null before consumption), and lifetime maximum consumed wait. No remote frame
timestamp is subtracted from a local clock. There are no new service requests.

The UI details and saved report, CLI final presentation JSON, four-viewer lifecycle
stress and encrypted-packet impairment observations use these measurements.
`FrameQueueDiagnostics` has its own schema and explicit `decoded-frame-handoff`
scope. The Python evidence checks reject missing fields, impossible ownership,
nonfinite ages, backward cumulative counters and mismatched presentation counts.
New runner executions require these fields; historical artifacts remain readable
and are explicitly marked unmeasured when the fields are absent. The validator
source hash is recorded alongside the runner hash.

These measurements exclude conversion time, the UI presentation-worker slot,
WebRTC sender/receiver buffering, graphics queues and actual display output.
They do **not** complete full queue-age, congestion recovery or external latency
acceptance. The maximum is retained across healthy/slow/recovered phases rather
than reset to hide an earlier stall. Sampled pending ages are not a continuous
maximum, and this diagnostic batch does not invent a new latency acceptance limit.

Validation includes ownership/repacking/conversion failure, stop with pending
work, repeated stop, and a blocked conversion observed concurrently with stop.
The real four-viewer slow-consumer run checks isolation and recovery through the
same production frame handoff; packet tests preserve their original recovery gates.

## Collected evidence — 2026-09-18

- `handoff-final-release` and `handoff-final-debug`: 12/12 headless room/UI/CLI
  checks each, including the blocked-conversion/concurrent-stop regression.
- `handoff-slow-release`: 60-second four-viewer run with heap/virtual-memory
  accounting and 20 seconds of 250 ms consumption on viewer 0 passes. The slow
  viewer consumes 3.86 fps while the other three stay near 30 fps; all recover
  near 29.87 fps. Maximum handoff wait is 34.880 ms; sampled pending age peaks
  at 31.081 ms. This early build precedes the final concurrency test/UI report
  addition; its original executable hash is preserved in the artifact.
- `handoff-slow-debug`: the final Debug build also passes the 60-second
  slow-viewer/ownership/memory-accounting scenario. Maximum handoff wait is
  37.563 ms; the slow viewer consumes 3.93 fps and all four recover near 30 fps.
  This is a short regression, not another two-hour soak or full memory acceptance.
- `handoff-network-release`: 5% loss passes functional isolation/recovery with
  maximum handoff wait 6.589 ms across all viewers. The affected receiver's recent
  recovery buffering averages 158.8 ms and its single internal input response is
  348 ms. Short handoff waits do not establish low end-to-end latency.
- The same network run's collapse case **fails** its unchanged offered-load
  precondition (baseline tail 3.10 Mbps, below the required >5 Mbps). Its affected
  viewer's handoff maximum is 6.469 ms versus 145.8 ms recent recovery buffering.
  These are diagnostic observations, not a valid collapse/recovery pass. Together
  they point the next investigation toward WebRTC buffering/pacing, without
  proving a cause or clearing any previous bandwidth-recovery failure.

Raw artifacts are under `build/webrtc/`; no failed run was retried or replaced to
obtain a passing benchmark. The 21 impairment-evidence and 17 lifecycle-evidence
Python checks also validate rejection of missing/inconsistent handoff records.
Compact results and original artifact hashes are preserved in
`evidence/frame-handoff-2026-09-18.json`.
