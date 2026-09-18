# Congestion recovery — 2026-09-18

The connection's congestion-control budget was never set. RTP sender settings
correctly capped video, but the pinned WebRTC probe controller uses a **5 Mbps
maximum probing rate** when its connection maximum is unspecified. That is not a
hard throughput cap: older runs sometimes climbed beyond it, while others became
application-limited near 3 Mbps or failed to recover after bandwidth returned.

The production runtime now sets the connection maximum after each accepted stream
settings revision: the viewer's allocated video rate plus its existing 128 kbps
audio reservation. Aggregate allocation and inactive video are respected. Minimum
and starting bitrate remain unspecified; this neither forces constant bitrate nor
resets the estimator. A rejected RTP update retains the working settings. If the
subsequent transport update fails, the partially updated peer is retired through
the existing failure path.

## Diagnosis and controlled trials

Before changing policy, two alternatives were measured and rejected:

- Screen pacing `2.5,200,80,40,-60,3` versus upstream's default
  `1.0,2875,80,40,-60,3`: receiver buffering improved in some runs, but collapse
  still failed its offered-load gate. Desktop-to-laptop response p95 remained
  about 108 ms. The pacer's queue-time parameter accelerates draining; it is not
  a hard queue-age bound or packet expiry policy.
- Disabling prerender smoothing: laptop internal response median improved to
  79.302 ms, but p95 worsened to 138.537 ms against the earlier 108.425 ms baseline.

Both changes were reverted. Default pacing and render smoothing remain in place.
Raw trial evidence is retained under `build/webrtc/pacing-policy-{baseline,trial}`,
`pacing-cross-host/{host,viewer}` and `render-queue-cross-host/{host,viewer}`.
These were sequential exploratory runs, not a controlled physical-latency study.

Opt-in RTC tracing then showed every peer's probe clusters capped at 5 Mbps.
The pinned source confirms the fallback in
`modules/congestion_controller/goog_cc/probe_controller.cc`. After setting the
connection budget, the affected peer's maximum probe reached **20.128 Mbps**.

The first traced corrected collapse run measured phase-tail ingress of
**17.939 → 1.810 → 17.925 Mbps**. Both collapse and 5% loss passed the unchanged
offered-load, recovery, healthy-viewer isolation, input revoke and ownership checks.
Evidence: `build/webrtc/congestion-transport-budget/result.json`.

Final Release checks, with event logging disabled, pass **5/5**: collapse, 2%
loss, 5% loss, reordering and duplication. Collapse phase-tail ingress is
**18.427 → 1.769 → 18.406 Mbps**. Debug collapse also passes with traces enabled.
Artifacts: `build/webrtc/congestion-budget-release-final/result.json` and
`build/webrtc/congestion-budget-debug-final/result.json`. The final Release
collapse still has 318.8 ms recent receiver buffering during recovery; no latency
threshold was relaxed or declared passed.

Desktop host → laptop viewer also passes production TLS, decoded video/audio,
fixed/Auto settings, restart, fresh rejoin, 40 synthetic input events and shutdown.
The corrected host uses the existing baseline laptop viewer. Its 20 internal
response samples have p95 **138.886 ms**, so this is compatibility evidence, not
a latency improvement. Artifacts: `build/webrtc/congestion-budget-cross-host`.
The six targeted CTests pass in both Release and Debug: settings/adaptation,
public room media, engine failure/lifecycle, trace output bounds, probe parsing
and impairment evidence validation.
The Release application builds successfully; its native-runtime, CLI-media and
headless Qt-UI integration tests also pass **3/3**.

Compact results, binary/report hashes, original rejected trials and all laptop
response samples are preserved in
[evidence/congestion-recovery-2026-09-18.json](evidence/congestion-recovery-2026-09-18.json).

## Repeatable silent diagnostics

```powershell
python scripts/test-room-impairment.py build/sdk-proof-release build/webrtc/congestion-NEW --scenario collapse --scenario loss5 --event-logs
```

The optional native event-log factory is empty in ordinary application sessions.
The proof records only its four synthetic host peers, with a fresh output directory,
exclusive file creation and an 8 MiB limit per peer. Overflow, write failure,
missing logs or incomplete shutdown fail evidence collection. Logs flush on peer
shutdown; files cannot overwrite earlier evidence.

`scripts/rtc_probe_evidence.py` reads the pinned version-2 protobuf schema without
additional packages. It extracts only probe clusters/results and application-
limited-region transitions. It rejects unsupported versions, malformed/truncated
fields and missing log start/end. Repeated results for one probe are retained;
they are not independent probes. Compressed bandwidth-estimate batches are not
decoded. Raw logs may contain transport metadata and stay in private build
artifacts; only the restricted summaries are suitable for review.

## Remaining acceptance

Restored throughput does **not** establish low latency. The first corrected
collapse run retained roughly 260 ms recent receiver buffering and measured a
430 ms internal input response during impairment. Full pipeline queue age,
receiver recovery latency, external Gaming/Quality targets, hardware 1080p60,
physical devices and Internet/NAT conditions remain open. These measurements use
synthetic 640×360@30 software video and discarded audio. No physical input is sent.
