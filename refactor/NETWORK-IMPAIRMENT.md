# Real-packet impairment and separate-process proof

Latest correction: [CONGESTION-RECOVERY.md](CONGESTION-RECOVERY.md) identifies the
missing connection bitrate budget and records corrected runs. Earlier failures
below remain historical evidence; receiver latency is still an open gate.

`RoomImpairmentProof` uses the actual public RoomSession, authenticated local Worker,
H.264/Opus and encrypted WebRTC transport. A test-only packet-socket factory applies
the pinned upstream `SimulatedNetwork` to one receiver's UDP ingress. The remaining
three viewers use ordinary sockets. No system firewall/shaper configuration changes,
physical input or audible playback are required.

Build the existing proof configuration with
`-DSCREENSHARE_WEBRTC_TEST_SOURCE_DIR=C:/dev/projects/screen-share/build/webrtc/checkout/src`
and target `RoomImpairmentProof`. CMake verifies the source revision against the lock
and rejects modifications to the compiled simulator/queue sources. These upstream
test sources are not linked into the shipped application; packaged SDKs alone do
not supply them. The production MediaEngine only gains optional native packet-factory
injection, constructed on its network thread. Ordinary construction is unchanged.

```powershell
python scripts/test-room-impairment.py build/sdk-proof-release build/webrtc/network-NEW
python scripts/test-room-impairment.py build/sdk-proof-debug build/webrtc/network-NEW-debug
```

Each output directory must be new. `--scenario` selects one case. The runner uses
the existing Windows process job, bounded logs/deadlines, executable/fixture/runner
hashes, and durable per-second observations. It stops on failure and preserves logs.

| Case | Applied impairment |
| --- | --- |
| collapse | 20 → 4 → 20 Mbps on one viewer ingress; baseline must actually exceed 5 Mbps, recovery must exceed 4.4 Mbps |
| loss2 / loss5 | 2% / 5% loss plus Gaussian delay: mean 25 ms, standard deviation 10 ms |
| reorder | Same variable delay with reordering enabled and zero configured loss |
| duplicate | Every 50th ingress packet duplicated, with zero configured loss |
| processes | One host and four real receiver subprocesses, with distinct PIDs, decoded frames/audio and completed runtime release |

Network cases have 12-second baseline, impaired and recovery phases. High-detail
synthetic 640×360@30 video supplies load; Manual resolution/FPS remain fixed while
WebRTC controls actual send rate under a 20 Mbps manual cap. The actual UI/CLI
one-frame presentation handoff is consumed headlessly. Input is explicitly granted,
applied to a recording sink, revoked during impairment, and rejected afterward.
This shapes the receiver's downlink only; it is not an uplink impairment test.

Packets remain encrypted. TCP/client-UDP fallbacks are disabled only in the test
factory so they cannot bypass the selected path. Each socket retains at most 256
packets / 2 MiB, with at most eight sockets. Stop clears all packet ownership.
In-memory socket checks independently cover 100% loss, duplicates, overflow and
close with pending packets. Fifteen Python evidence checks reject false positives,
missing telemetry, wrong configurations, insufficient load and incomplete recovery.

Arrival timestamps use the simulator's delivery time, matching upstream
`LinkEmulation::Process`; original ingress timestamps would hide the injected delay,
while polling wake-up timestamps would add unintended jitter. Maximum scheduling
lateness is recorded separately. Seed 12345 is the base for per-socket seeds;
real scheduling/socket enumeration means this is not byte-identical replay.

## Results and boundaries

`build/webrtc/network-timestamps-{release,debug}/result.json` passed **6/6 each**.
Collapse baseline ingress averaged 7.50 Mbps (Release) and 13.36 Mbps (Debug),
settled to 2.36 / 2.32 Mbps in the impaired tail and regained bandwidth afterward.
All four viewers recovered to approximately 30 fps. Loss/reordering/duplication
were observed, healthy-viewer isolation passed, and packet/runtime owners released.
Separate-process cases completed in about 14.3 / 14.2 seconds.
After bounding both child output channels, the separate-process cases passed
again in `build/webrtc/process-bound-{release,debug}/result.json`.

Preserved failures: `impairment-final-debug` failed healthy-viewer isolation while
compiler work overlapped; `impairment-isolated-debug` passed without compilation.
`network-final-release` failed the offered-load precondition before the simulator
timestamp correction. Neither failure is counted as a passing run.

This completes the local functional harness, not Stage 4 performance acceptance.
Gaussian delay is not strictly capped at 50 ms. Healthy 1080p60 hardware load,
uplink/state-loss/delayed-event scenarios, real interface changes/NAT/TLS, external
image/input latency, A/V skew and matched legacy comparison remain open. In particular,
the Release collapse receiver's cumulative mean jitter buffering rose from 84 ms
at baseline end to 151 ms at recovery end. Recovery FPS is not proof of low latency;
stale-frame-age settling and physical latency acceptance must remain open.

## Recent-buffer measurement follow-up

The receiver now reports both lifetime and recent-interval buffering. The recent
value differences the actual emitted-frame delay/count; missing/reset/stale
intervals stay unknown. The harness records a fixed 20-second warmup before its
three 12-second phases because WebRTC starts at 3 Mbps, below the collapse gate.
Warmup is retained in the artifact; no trial is retried or selected automatically.
The runner now completes all requested scenarios even when one fails, and supports
repeated `--scenario` arguments. Overall failure is retained.

`network-recent-release` failed the collapse offered-load precondition. Adding the
fixed warmup did not resolve it: `network-warmup-release` also failed. Those runs
cannot establish a 20→4 Mbps collapse, regardless of their native process success.
The unchanged offered-load/recovery bounds are preserved. The earlier 6/6 results
above remain historical evidence, not proof that every later run passes.

`network-recent-remaining-release` passes loss2, loss5, reorder, duplicate and
separate-process checks (5/5). The affected receiver's recent recovery-tail means
are respectively 97.8, 88.6, 113.0 and 113.4 ms. These observations do not meet or
prove the external Gaming latency target. No production jitter/codec knob was
changed merely to obtain a passing benchmark.

The follow-up Debug reordering run (`network-recent-all-debug`) overflowed 57
packets when delay was added at a nearly saturated 20 Mbps link. It correctly
failed the no-added-loss gate. Pure loss/reorder/duplicate cases now use 100 Mbps
headroom, with the sender still capped at 20 Mbps; collapse alone retains
20→4→20 Mbps. This separates delay/reordering from unintended capacity loss.
`network-isolated-final-debug` passes 6/6; Release passes 5/6, with collapse still
below the offered-load requirement. These are bounded functional checks, not
proof of repeatable bandwidth convergence or the external latency target.

The explicit proof-only `--fast-audio-experiment` passes through WebRTC's existing
`audio_jitter_buffer_fast_accelerate` configuration. Run it with an explicit packet
case, e.g. `--scenario loss5 --fast-audio-experiment`. Production defaults remain
unchanged. In the Release/Debug comparisons, affected-viewer recent recovery means
were 48.0/55.0 ms by default and 140.4/62.6 ms with acceleration; initial conditions
also differed. Artifacts: `network-fast-audio-{release,debug}`. These few runs do
not establish a causal benefit, so the setting was not adopted.

Schema 2 adds an internal input-to-image measurement during impairment. The existing
recording sink changes a synthetic response scene on an authorized press; the
viewer must consume the decoded response, then consume its removal after revoke.
It uses one machine's monotonic clock, never OS input or physical display output.
One observation per scenario is not a p95/p99 latency claim. Sample intervals now
include time spent waiting for this response, keeping FPS/ingress rates honest.
Older schema 1 evidence cannot claim this input-image measurement.

Final localized-marker runs: `network-marker-final-{release,debug}`. The input
response uses a 32×32 patch instead of replacing the whole noisy scene, so it does
not simultaneously change the whole encoder workload for all viewers. Explicit
unknown bandwidth and no-emission recent-buffer intervals during impairment are
retained as unknown; missing fields/nonfinite values and missing recovery
observations still fail. The corrected evaluator records observation counts and
preserves the original reports. See `network-marker-reevaluated.json` and the
committed evidence/UNATTENDED-RESULTS.md.

Final result: Release 5/6, Debug 6/6. Release collapse still fails the unchanged
recovery gate (19.10→2.38→3.95 Mbps phase-tail means). This is not a passing Stage 4
performance result. Input-marker, revoke, ownership and separate-process evidence
are retained independently. Nineteen fail-closed Python evidence tests pass.

## Return-to-desktop investigation — 2026-09-18

`return-target-baseline` repeats collapse with sender target, encode/keyframe
counters and mean completed-packet send delay recorded. It passes the unchanged
functional gates, but recent recovery buffering averages 192 ms and the single
internal input response is 340 ms. A passing repetition does not erase the
earlier Release failure or establish the gaming latency target.

An explicit trial mapped Gaming to WebRTC's `kFluid` track hint (motion policy)
instead of inheriting the source's screen-content policy. Its independent artifact
`return-motion-collapse` fails recovery: 15.95 → 1.95 → 1.99 Mbps phase tails,
with a 446 ms internal input response and elevated receiver buffering. The change
and its preset-specific tests were reverted; **it is not in the shipped code**.
No pacing/jitter field trial or alternate controller was adopted. Original and
experimental executable identities remain in their respective native reports.

New packet-proof runs also record decoded-frame handoff ownership and local wait
ages for every viewer/sample. The evaluator requires complete, consistent fields;
the same instrumentation is used by UI/CLI and lifecycle stress. Historical files
without it remain explicitly unmeasured. See FRAME-HANDOFF.md; network recovery
and physical latency gates are unchanged.

The production change in this batch is the independently tested MF unavailable-FPS
resume fix and optional sender diagnostics. It is not a demonstrated solution to
bandwidth convergence. The native source remains screen content in both presets.
