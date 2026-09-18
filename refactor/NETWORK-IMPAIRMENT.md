# Real-packet impairment and separate-process proof

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
