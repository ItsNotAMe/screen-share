# Stages 2–4 acceptance status

The requested scope is to finish Stages 2–4 before cutover or the later UI redesign.
Implementation and locally reproducible evidence are separate from field acceptance.
No unchecked physical requirement is waived by the unattended test run.

| Stage | Implemented / locally verified | Remaining acceptance |
| --- | --- | --- |
| 2: user experience | Normal-shell opt-in share/join, pushed directory, profiles, settings, capture/audio recovery, GPU receive, and redacted UI/CLI reports. Release/Debug headless matrices pass 12/12 each. | Physical audio formats/quality/unplug, source privacy/identity/HDR/adapters, supported-desktop DXGI and occlusion. Final legacy command/default cutover remains Stage 5. |
| 3: gaming input | Consent, source-bound mapping, per-peer ownership, controllers, queues/watchdogs, neutralization, UI/CLI input diagnostics, recording-sink real-channel tests. | Physical mouse/keyboard confinement and foreground/UIPI behavior, XInput/PlayStation and virtual-driver/local-slot behavior; measured external input response. |
| 4: stability/performance/service | 100-room restart evidence, resource accounting, silent soak runner, real encrypted packet impairment suite, and ten-room service cost invariants. | Two-hour result, original immediate capture-handle failure, full sustained-memory/queue-age acceptance, hardware 1080p60 load, separate machines/NAT/TLS/interface changes, external image/input latency and A/V skew, matched legacy comparison, measured eight-hour service/account headroom and production hibernation. |

## Physical checks that cannot be replaced by this fixture

- On actual host/viewer machines, record display mode, GPU/driver, capture backend,
  codec path, audio devices and controller/virtual-driver versions. Capture a v2
  report at each failure and after recovery. Do not put passwords or tokens in notes.
- Validate each shared window/output remains the selected source through movement,
  minimize/restore, display/source replacement and HDR mode changes. Confirm that
  window sharing never exposes a different window/desktop. Test DXGI only on a
  supported active desktop; an unavailable backend is not a passing fallback test.
- Verify actual mono/stereo/surround capture, microphone quality, volume/mute,
  selected-device switching and unplug/replug, with video continuing on audio failure.
- With explicit host consent, test held key/button/pad release on revoke, focus
  loss, source change, disconnect and unplug. Check pointer confinement/letterboxing,
  prohibition of keyboard control for window shares, local controller slot retention,
  three independent remote pads and missing-driver behavior. Do not inject into
  unrelated applications as part of headless tests.
- Use an external timing method on two machines for Gaming 1080p60: p95 image
  latency <80 ms, p95 input-visible-response <120 ms, Quality p95 image <250 ms,
  steady A/V skew within ±50 ms. Retain p50/p95/p99, sample counts and method.
  Receiver jitter-buffer means and local input timings do not establish these.

The service-cost invariant test demonstrates zero application/storage work for
automatic heartbeats across 50 participants plus 10 directory subscribers, and
zero room-object fanout for ten listing requests. It is deliberately not labelled
an eight-hour usage, billing, quota-headroom or production-hibernation measurement.

The production-code eight-hour steady-operation model now passes both alarm
orderings (SERVICE-COST.md). Real packet/network and separate-process tests pass
6/6 in both configurations (NETWORK-IMPAIRMENT.md). Capture/input physical tests
remain blocked while the input desktop is `Screen-saver`; no unlock or input was sent.
