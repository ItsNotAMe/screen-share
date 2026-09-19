# Stages 2–4 acceptance status

The requested scope is to finish Stages 2–4 before cutover or the later UI redesign.
Implementation and locally reproducible evidence are separate from field acceptance.
No unchecked physical requirement is waived by the unattended test run.

For the short list of remaining backend groups and the frontend boundary, use
the **Working agreement** table at the top of [TODO.md](TODO.md). This document
keeps the detailed acceptance requirements; its historical updates are not
additional milestones.

Latest unattended closeout: the 32-run matched packet comparison is complete
and supports retaining v2 (MATCHED-NETWORK.md). The physical HID regrant fault is
fixed (42 grants, ten reader lifetimes). A separate real virtual-driver allocation
test fails and requires administrator cleanup before retesting (CONTROLLERS.md).
The longer eight-minute standalone GPU probe still shows Section-handle growth;
software receiver compatibility remains the qualified path. These are the current
results; older “comparison/regrant pending” updates below are historical.

Unattended closeout (2026-09-19): Release/Debug room matrices pass 17/17 each.
Both PCs pass silent native audio lifecycle, all four audio capture selections,
generated-window minimize/restore/close privacy and injected capture recovery.
Ten controller fixtures pass 140 grants plus ten expected backend denials after
the permission-owner fix. The subsequent physical-reader fix passes 42 grants
and ten polling-thread lifetimes; cancelled HID reads were the reproduced fault.
Earlier failures remain preserved (CONTROLLERS.md). Four independent laptop software
viewers pass five-minute LAN load at 53.6 fresh FPS each with stable resources.
The isolated collapse rerun passes the unchanged limit (141.6709 ms), while an
earlier 151.2051 ms failure is preserved. These are partial gate results, not a
backend cutover decision. See CONTROLLERS.md, HARDWARE-LAN.md, CONGESTION-WINDOW.md
and `evidence/unattended-closeout-2026-09-19.json`.

2026-09-19 field follow-up: GameSir Bluetooth delivery and held-input release/
disconnect pass; repeat-grant now passes the unattended physical-reader check. Reverse LAN
delivery sustains 49.49 fresh FPS for five minutes with no invalid images, using
laptop software encoding after a hardware deadline failure. Hardware-host
qualification therefore fails rather than being silently waived. See CONTROLLERS.md
and HARDWARE-LAN.md. Actual account usage in SERVICE-COST.md also leaves the cost
gate open: the observed request headroom is below target and chart intervals/
exceptions still need reconciliation.

Latest completed portion: five-minute native presentation in software-decoder
compatibility mode passes at 47.7 rendered / 48.3 fresh decoded FPS. Real GPU
rendering, one-frame latency configuration, resize/minimize/restore and valid
post-recovery images are verified; viewer median private memory is 89→91 MiB
and Section handles stay at 11 during streaming. This is the production handoff/
renderer in an owned test window, not full Qt field or external-latency acceptance.
Earlier failures and the 44.4-FPS polling control are retained in
[RECEIVER-RESOURCES.md](RECEIVER-RESOURCES.md). The event-driven fixture passes
the unchanged 45-FPS gate. Continue physical device/source/input/audio checks
and group 2 closeout from TODO.md; do not repeat this completed native renderer pass.

Earlier completed group: [RECEIVER-RESOURCES.md](RECEIVER-RESOURCES.md) adds an
explicit local software-decoder compatibility setting in the runtime, UI and
CLI. A five-minute desktop hardware-host → laptop software-viewer run passes
at 45.4 fresh FPS with no invalid images, flat Section handles (five), and
31→33 MiB private-memory medians. The graphics/kernel hardware-retention issue
remains open; this is a measured CPU-consumer workaround, not hardware repair,
physical presentation, 60-FPS or gaming-latency acceptance. Release/Debug
UI/CLI integration and strict mode-evidence tests pass. Continue the remaining
consolidated device/network/physical checks, not another capture rewrite.

Earlier hardware group: [HARDWARE-LAN.md](HARDWARE-LAN.md) verifies one actual desktop WGC
host and laptop hardware decoder at 1080p60/12 Mbps for five minutes: 52.8 fresh
FPS, no fallback and no invalid images. Resource sampling exposes laptop viewer
growth (118→157 MiB median private memory; 645→833 median handles), so sustained
resource acceptance remains open for hardware decoding. The compatibility
result above qualifies only its stated scope. This is not a physical
GPU-display/input latency or four-viewer pass.
Follow-up [RECEIVER-RESOURCES.md](RECEIVER-RESOURCES.md) attributes increasing
Section handles to a reproducible graphics-level path: direct D3D texture
creation has the same trend without the v2 frame wrapper, decoder or network.
The long-term resource gate remains open. The 90-second diagnostic LAN run had
five invalid scene markers and is retained as a failure.
The paired 1080p legacy/v2 workload is documented in COMPARISON.md. Its first matrix
is superseded because an unpaced silent sink distorted CPU and synchronized-video
measurements. Use the corrected paced-sink comparison and sender-pacing/timer fixes;
local CPU-image consumption is not physical latency, GPU-display or matched network
acceptance.
Fairness/hardware follow-up: the portable timer and one-submission encoder fixes
are also applied to legacy, and COMPARISON.md separates pre-fix from improved
legacy controls. The earlier default-configuration latency win is not an
architecture verdict. Tuned legacy remains a real latency competitor; v2
software throughput and four-viewer private memory were outstanding performance
work. HARDWARE-ENCODING.md records the reproduced legacy queue delay, its shared
fix, hardware recovery checks and the bounded ten-minute v2 hardware evidence.
The subsequent capture/decoder investigation is in CAPTURE-LATENCY.md: capture
already shares the legacy implementation, the owned wrapper is retained, and
the one-frame decoder hold is fixed for both backends. Use the newest fair
scorecard in COMPARISON.md rather than the earlier absolute latency values.
This closes the capture replacement decision, not resource/field acceptance.
The software-efficiency follow-up (SOFTWARE-THROUGHPUT.md) restores reference
software delivery to 52.9/48.2 fresh FPS for one/four viewers, with the same
CABAC fix in legacy. The latest sixteen fair runs favor v2 local image age in
every configuration and hardware CPU, but four-viewer software CPU/private
memory and hardware private memory still exceed legacy. Total resource,
congestion and physical acceptance remain open; do not advance the default
cutover on the basis of image-age results alone.
User update: relative memory optimization is deferred to BACKLOG.md. The latest
combined host/four-receiver fixture uses about 522 MiB hardware / 924 MiB software
private memory. Lower memory than legacy is no longer a gate. Leak/unbounded
growth, normal desktop/laptop usability and congestion responsiveness still are.
The subsequent congestion-window follow-up is in CONGESTION-WINDOW.md. It retains
upstream RTT-aware bitrate pushback with a 50 ms additional in-flight allowance and adds
continuous held-image age to the packet fixture. The matched old controls fail
three-second settling despite eventual recovery. The latest 16 normal-load fair
controls pass with portable fixes still shared by legacy. Local settling evidence
does not replace physical latency, cross-machine GPU load or Internet/NAT checks.
Local congestion settling is now accepted: five retained-policy collapse runs
clear stale images by 1.29–1.63 seconds and remain at 117–129 ms maximum age after
three seconds. All four other Release packet cases pass, as do 34 native and 24
evidence tests. The failed drop-only candidate remains in the evidence. Next is
the consolidated physical/device/network and remaining resource pass below.
The decoded-frame handoff now has exact ownership and local wait/age measurements
([FRAME-HANDOFF.md](FRAME-HANDOFF.md)); this is one measured queue, not full pipeline
queue-age or physical latency acceptance.

Continuation: the user has a Windows laptop and GameSir Nova Lite.

Physical controller update (2026-09-19): laptop Bluetooth GameSir input, using the
native DS4 HID reader, reaches the desktop virtual controller. The user confirmed
button/stick response and clean explicit release while holding a button; the host
records 1,373 applied reports, zero rejected and a cleared grant afterward.
The subsequent held-button power-off check also passes by user observation;
host evidence ends at 1,592 applied reports, zero rejected and a cleared grant.
See CONTROLLERS.md for the padded-HID and cancelled-read fixes and evidence.
Repeat-grant now passes automatically; other physical controller models and
external latency remain unverified.

FIELD-TESTING.md
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
| 4: stability/performance/service | 100-room restart evidence, completed two-hour software run with ownership/accounting, Release/Debug local tests, strict collapse settling, normal-load and 32 selected matched-impaired legacy comparisons, bidirectional LAN media/recovery through live TLS, five-minute desktop hardware delivery and four-viewer software-decoder compatibility with stable laptop resources, native GPU presentation/recovery, encrypted input-marker/process tests, and ten-room/eight-hour service cost model. | Laptop hardware-decoder/kernel retention (software workaround verified), original immediate capture-handle failure, remaining physical presentation/full-pipeline queue-age acceptance, strict four-viewer/reverse hardware qualification, Internet/NAT/interface changes, external image/input latency and A/V skew, actual billed usage/account headroom and production hibernation reconciliation. Relative memory optimization is backlogged. |

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
