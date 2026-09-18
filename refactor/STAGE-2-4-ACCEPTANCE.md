# Stages 2–4 acceptance status

The requested scope is to finish Stages 2–4 before cutover or the later UI redesign.
Implementation and locally reproducible evidence are separate from field acceptance.
No unchecked physical requirement is waived by the unattended test run.
The paired 1080p legacy/v2 workload is documented in COMPARISON.md. Its first matrix
is superseded because an unpaced silent sink distorted CPU and synchronized-video
measurements. Use the corrected paced-sink comparison and sender-pacing/timer fixes;
local CPU-image consumption is not physical latency, GPU-display or matched network
acceptance.
The decoded-frame handoff now has exact ownership and local wait/age measurements
([FRAME-HANDOFF.md](FRAME-HANDOFF.md)); this is one measured queue, not full pipeline
queue-age or physical latency acceptance.

Continuation: the user has a Windows laptop and GameSir Nova Lite. FIELD-TESTING.md
contains the prepared five-minute first pass and test-scene/package instructions.
The desktop became available and a 16-case Release matrix plus WGC window/display
checks passed, but it later returned to Screen-saver and blocked the final desktop
rerun. DXGI still reports unsupported on the current HDR desktop. This is not a
passing DXGI fallback result. The isolated ten-room v2 service is now approved
and deployed, with HTTPS health verified from both PCs. Laptop software codec
checks pass; SSH-session hardware encoding falls back. See FIELD-TESTING.md.
The updated two-PC viewer is confirmed working (PRESENTATION-SIZING.md). The full
event-driven native lifecycle passes live HTTPS/WSS on both PCs (LIVE-SERVICE.md),
using same-machine media peers. NAT/interface-change and external-latency gates
remain open. The earlier timed CLI failures are reproduced and fixed as fixture
ordering errors. Bidirectional LAN media/settings/restart/rejoin and synthetic
input-response checks now pass; see TWO-MACHINE.md for the exact scope.

| Stage | Implemented / locally verified | Remaining acceptance |
| --- | --- | --- |
| 2: user experience | Normal-shell opt-in share/join, pushed directory, profiles, settings, capture/audio recovery, GPU receive, and redacted UI/CLI reports. Release/Debug headless matrices pass 12/12 each. | Physical audio formats/quality/unplug, source privacy/identity/HDR/adapters, supported-desktop DXGI and occlusion. Final legacy command/default cutover remains Stage 5. |
| 3: gaming input | Consent, source-bound mapping, per-peer ownership, controllers, queues/watchdogs, neutralization, UI/CLI input diagnostics, recording-sink real-channel tests. | Physical mouse/keyboard confinement and foreground/UIPI behavior, XInput/PlayStation and virtual-driver/local-slot behavior; measured external input response. |
| 4: stability/performance/service | 100-room restart evidence, completed two-hour software run with ownership/accounting, Release/Debug local tests, corrected Release 5/5 packet impairment and Debug bandwidth recovery, bidirectional LAN software media/recovery through live TLS, encrypted input-marker/process tests, and ten-room/eight-hour service cost evidence. | Receiver buffering/recovery latency, original immediate capture-handle failure, full sustained-memory/queue-age acceptance, hardware 1080p60 cross-machine load, Internet/NAT/interface changes, external image/input latency and A/V skew, matched legacy comparison, actual billed usage/account headroom and production hibernation. |

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
6/6 in both configurations in the initial batch; later stricter/final runs retain
a Release recovery failure (NETWORK-IMPAIRMENT.md and UNATTENDED-RESULTS.md).
The subsequent connection-budget correction is documented in CONGESTION-RECOVERY.md;
its traced collapse/loss checks pass, while receiver buffering and external latency
remain unresolved. Earlier failed evidence remains preserved.
PLAYOUT-RECOVERY.md records the next Gaming receiver-policy improvement and its
lower collapse FPS. Transient congestion backlog, reference-load receiver latency,
external targets and physical A/V sync remain open despite better software recovery.
The desktop later became available: the expanded Release generated-window/UI/CLI
matrix now passes 17/17 (TWO-MACHINE.md). No unlock or physical input was sent.
