# Congestion settling and displayed-image freshness — 2026-09-19

Final unattended follow-up: five of six network cases pass in
`build/acceptance-network-final-20260919`; collapse fails the strict 150 ms
settled-image limit at **151.2051 ms**. An isolated rerun with the identical
policy and unchanged thresholds passes: stale images clear by **1.400 s** and
maximum settled age is **141.6709 ms**. The failed run occurred alongside other
work, but scheduling is not established as its cause. Preserve both outcomes;
the narrow margin does not establish robustness under arbitrary CPU contention.
See `evidence/unattended-closeout-2026-09-19.json`.

## Cause and retained change

The pinned WebRTC controller normally permits 350 ms of additional in-flight
data before its congestion-window pushback drops source frames. Under the
20-to-4 Mbps capacity step, this allows a large backlog and loss burst before
the producer slows down. Renderer changes and extra keyframes did not address
that buildup (CONGESTION-STAGES.md).

`MediaNetworkPolicy` reduces the additional in-flight allowance to **50 ms**.
The window still accounts for RTT and uses the upstream pushback controller
with its 30 kbps minimum. `DropFrame:false` forwards the pushback bitrate to the
encoder instead of the special congestion-window frame-drop mode. The latter
can remove at most half the source frames in this SDK, which is insufficient
for a sharp capacity collapse. Normal upstream encoder frame dropping remains
available. The retained policy does
not drop arbitrary encoded H264 fragments, override bandwidth feedback, clamp
the user's settings differently, or change the simulated link. The existing
screen-share pacing policy is unchanged. This is not a 50 ms packet-expiry or
end-to-end latency guarantee.

The policy lives at the private WebRTC boundary. An engine regression uses the
pinned SDK's actual `RateControlSettings` parser to verify that the window,
pushback, bitrate mode and minimum are recognized. No vendor source or SDK
binary was patched. Both Gaming and Quality retain their existing receiver
playout policies and preset-switch behavior.

## Stronger acceptance evidence

The existing input response starts two seconds into each phase, so a short
initial freeze could disappear before the first press. The fixture now embeds
a bounded frame ID, CRC16 and complementary high-contrast cells in the generated
scene. The marker occupies 256x16 pixels (about 1.8% of the 640x360 scene);
the rest remains changing noise, and offered-load checks remain mandatory.
Matched old/new runs use the same marked scene.

The marker crosses the real software codec and encrypted packet path. Its
timestamp table remains local to this same-process proof. On consumption, the
fixture measures capture-to-consumption age and continues aging the last image
while no new frame arrives. This includes frozen displayed images rather than
measuring only frames that successfully arrive. CRC/contrast checks reject
uncertain IDs; strict settling validation requires zero invalid markers.
The fixture's 5 ms polling adds timing granularity. It does not measure physical
display, external input or synchronization between machines.

The approved PLAN.md asks reduced-capacity operation to settle within three
seconds. `--require-settling` enforces that all four viewers stop showing images
older than the diagnostic **150 ms freshness threshold** within three seconds
of each collapse phase transition. Maximum age and the last stale observation
are retained; an initial freeze is never erased from the report. The 150 ms
diagnostic threshold is not a replacement for the healthy-LAN physical Gaming
targets of <80 ms image / <120 ms input-response p95.

Both old-policy controls pass the previous recovery checks but fail this new
settling check: the affected image remains stale until 3,411 / 3,229 ms, with
maximum displayed ages 2,915 / 2,649 ms. The first marked 50 ms-window run settles
by 1,517 ms, with maximum displayed age 1,236 ms and no invalid markers. A sudden
capacity collapse can still cause a temporary freeze; the retained change
addresses buildup and recovery, not an impossible promise of zero network delay.

## Explored alternatives

- Shorter loss-estimation observation window: first response 1,299 ms; not kept.
- Shorter delay-trend window: first responses 785 / 931 / 864 ms; improvement,
  but not retained because reducing the in-flight allowance addresses buildup
  more directly. Default delay/loss estimators are retained.
- 50 ms in-flight allowance, original unmarked scene: impaired responses
  62 / 57 / 57 / 41 / 56 ms, with unchanged isolation and recovery checks.
- The initial 50 ms/drop-only candidate passed one marked run but failed the
  final collapse repetition: a late stale image at 5,943 ms violated the same
  strict three-second requirement. The other four packet scenarios passed.
  `build/congestion-window-final-release` preserves that failure. A diagnostic
  repeat passed with 141 ms maximum age after the first three seconds. Passing
  repeats do not erase the failed run; this mode was not retained.
- The 50 ms/bitrate-pushback candidate passed its first strict run: last stale
  image at 1,527 ms, maximum age 1,168 ms, and maximum age after three seconds
  141 ms. Final repeated and full-matrix results below determine acceptance.

These exploratory first-response numbers do not describe the earlier frozen
image. Use the matched visual-age controls and final matrix for acceptance.

## Final retained-policy validation

The local congestion-settling group passes with `QueueSize:50,MinBitrate:30000,DropFrame:false`.
The strict threshold and three-second requirement were not relaxed after the
drop-only failure. Five collapse runs (four Release, one Debug) pass; the complete
Release loss2/loss5/reorder/duplicate matrix also passes. Healthy-viewer isolation,
offered-load, input revoke, ownership and recovery checks remain mandatory.

| Run | Last stale image after capacity drop (ms) | Maximum displayed image age (ms) | Maximum age after three seconds (ms) |
| --- | --- | --- | --- |
| Release repeat 1 | 1,361 | 1,030 | 117 |
| Release repeat 2 | 1,498 | 1,168 | 123 |
| Release repeat 3 | 1,467 | 1,104 | 122 |
| Final Release | 1,632 | 1,214 | 125 |
| Final Debug | 1,295 | 937 | 129 |

All collapse markers are readable. The maximum age column deliberately retains
the initial freeze; this is improved recovery, not zero-delay capacity switching.
The 2%/5% loss runs can still show brief stale images during continuing loss
(maximum ages 223/227 ms), while meeting their existing recovery/isolation gates.
The strict three-second check applies to the capacity-drop scenario, not a claim
that every packet-loss pattern stays below 150 ms.

All 34 selected Release/Debug native regressions pass, including software and
hardware codecs, lifecycle/fallback, preset settings, public-room media and UI/CLI
integration. All 24 Python evidence tests pass. See COMPARISON.md for the final
normal-load legacy/v2 scorecard. The compact artifact
`evidence/congestion-window-2026-09-19.json` preserves report/binary/source hashes,
failed controls, rejected candidates and final measurements.

Next acceptance is the consolidated device/two-PC/network pass, including physical
Gaming latency and sustained resource behavior. Relative private-memory optimization
remains in BACKLOG.md; neither lower memory than legacy nor another capture rewrite
is required to close this local congestion group. Default cutover remains gated.

## Repeatable silent checks

```powershell
python scripts/test-room-impairment.py build/sdk-proof-release build/congestion-NEW --scenario collapse --scenario loss2 --scenario loss5 --scenario reorder --scenario duplicate --require-settling
python scripts/test-room-impairment.py build/sdk-proof-debug build/congestion-debug-NEW --scenario collapse --require-settling
```

Old reports remain readable without the new requirement. Fresh runs require
valid image-age evidence; closeout collapse runs additionally require settling.
The fixture checks marker roundtrip, retained IDs, corruption rejection and
packet-delay accounting. Python tests reject missing/invalid markers, impossible
stale intervals, unknown image ages, and late settling despite eventual recovery.
