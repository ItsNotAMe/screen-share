# Gaming playout and response recovery — 2026-09-18

The previous connection-budget fix restored throughput, but a receiver could
retain hundreds of milliseconds of buffering after congestion cleared. The native
encoder did not send a playout-delay request, so the Gaming preset had no direct
effect on WebRTC's receiver timing policy.

Configured Gaming frames now request best-effort short playout
(`min = max = 10 ms`). Quality explicitly restores the upstream adaptive range.
Simply omitting the request when switching to Quality would leave the receiver's
previous Gaming request active. Unconfigured codec probes retain their original
behavior. These requests use the negotiated RTP extension inside encrypted media;
they introduce no signaling-service requests or separate receiver queue.

The preset is an immutable property of each frame's existing metadata wrapper,
captured from the same settings snapshot as resolution/FPS adaptation. Asynchronous
encoding and latest-frame replacement therefore cannot apply a later preset to
an earlier frame. CPU/GPU buffer ownership and input coordinate mapping are
preserved. Manual resolution, FPS and bitrate selections are unchanged.

Gaming trades smoothing for freshness on an unstable connection. It is a request,
not a deadline or an end-to-end latency guarantee. Quality retains WebRTC's normal
adaptive buffering and synchronization behavior. The preset tooltip explains the
tradeoff without exposing codec implementation details.

## Stronger silent measurement

The impairment proof now performs five synthetic press/clear cycles in each of
baseline, impairment and recovery. Each response requires a newly visible marker;
the clear frame must be consumed before the next press. Every phase ends with
revocation and rejection of further input, and the following phase obtains a new
grant. No OS input or audible output is used.

Schema 3 retains the old single impaired response field for historical readers,
and adds all 15 samples in `inputResponseByPhaseMs`. The runner requires complete
phase evidence for new runs, rejects malformed/missing samples and inconsistent
legacy aliases, and preserves the existing load, FPS, isolation and ownership
gates. Five samples per phase are exploratory measurements, not a stable p95 study.

```powershell
python scripts/test-room-impairment.py build/sdk-proof-release build/webrtc/playout-NEW --scenario collapse --scenario loss5
```

The initial unscoped minimum-playout experiment passed collapse and 5% loss;
its artifact is `build/webrtc/playout-minimal-trial`. The retained candidate is
preset-specific. The matched repeated-response baseline is
`build/webrtc/playout-phase-baseline`; the retained implementation's Release run
is `build/webrtc/playout-final-release`.

Matched collapse measurements (five response samples per phase, final 10 ms policy):

| Measurement | Previous adaptive behavior | Gaming request |
| --- | --- | --- |
| Recovery response samples, ms | 434, 427, 344, 346, 157 | 107, 73, 81, 104, 94 |
| Recovery recent-buffer mean, final five observations | 190.4 ms | 30.4 ms |
| Mean affected-viewer FPS during collapse | 23.8 | 19.4 |
| Mean affected-viewer FPS during recovery | 29.9 | 27.4 |
| Worst sampled response during collapse | 1863 ms | 1906 ms |

The large initial-collapse response is not improved in this final run. Removing
excess receiver buffering does not remove an already congested sender/network path.
Healthy-viewer isolation, offered load and throughput recovery gates are unchanged.

## Preset switching and decoder resize

The original 0/0 experiment passed steady-preset packet tests but failed the
two-machine Quality-to-Gaming transition. The pinned SDK's `VCMTiming::RenderTime`
returns zero for zero-minimum low-latency rendering. Its existing
`VideoRenderFrames::AddFrame` rejects timestamps lower than the last queued render
time. After Quality has supplied real clock timestamps, returning to zero can
therefore stop delivery indefinitely even while decoding continues.

The retained 10/10 ms request is the smallest nonzero RTP playout unit. Both
presets retain clock-based timestamps. No vendor code, pacing field trial or
prerender smoothing setting is changed. The failed zero-delay experiments and
diagnostic traces remain under `build/webrtc/playout-*-diagnostic` and the
`playout-gaming-*`, `playout-resize-fixed-cross-host`, and
`playout-sps-fixed-cross-host` directories. Their steady-state measurements are
exploratory evidence, not final-policy acceptance.

The investigation also added a decoder regression: 320→640→320→1280 without
reconfiguration. A decoder can legitimately deliver one pending old-size frame
after receiving the next keyframe. The test therefore associates expected sizes
with RTP timestamps instead of the latest submitted size; it checks CPU/GPU
output, timestamps and retained texture ownership. This passes with the existing
decoder. Provisional transform-restart/SPS changes were removed after decoded-output
traces established that the persistent freeze was downstream of decoding.

The final desktop-host/laptop-viewer test passes in
`build/webrtc/playout-final-cross-host`: 354 video frames, 1,458 discarded audio
blocks, 20 synthetic input/image responses (62–80 ms), settings changes, both
preset transitions, ICE restart, fresh rejoin and teardown. Both machines ran the
same hash-verified binary. These are internal synthetic timings, not physical
controller or glass-to-glass measurements. The local four-viewer test now also
requires fresh 640-wide receiver telemetry after Quality-to-Gaming restoration.

## Retained validation

- Release codec/settings/room regressions: 8/8; Debug: 5/5. Includes CPU/GPU
  ownership, coalesced frame presets, decoder resizing and four-viewer transitions.
- Release application build and native runtime/CLI/Qt UI tests: 3/3.
- Impairment evidence validator: 22 Python cases, included in the above suites.
- The 10 ms policy passed collapse, 2% loss, 5% loss, reorder and duplication
  (5/5) in `playout-short-release`. That build still contained the provisional
  decoder experiment; these scenarios use fixed-size streams. After removing it,
  the retained build passes Release collapse/5% loss (2/2), Debug collapse (1/1),
  and the two-PC transition test above.

[Machine-readable evidence](evidence/playout-recovery-2026-09-18.json) preserves
report/binary hashes, each phase's response samples, buffering/FPS measurements,
failed two-machine diagnostics and the final successful reports. Earlier 0/0 runs
are explicitly labeled rejected despite their passing steady-preset packet tests.

## Acceptance boundary

This is synthetic 640×360@30 software media. The encoder policy also has local
hardware/owned-GPU tests, including no CPU readback, but that does not establish
1080p60 two-machine latency. Physical audio/video synchronization, image quality
under loss, external input/display latency and transient sender/network queues
remain acceptance work. A low receiver-buffer measurement alone cannot close them.
