# Capture resource acceptance

This batch revisits the earlier +76/+10 handle-growth failures without weakening
the original +8 bound or removing the MTA lease, owned dispatcher, GraphicsCapture
module pin, frame ownership or teardown ordering. The earlier failures remain valid
historical evidence. A passing repeat does not identify why they differed, and no
new production leak fix is claimed by this batch.

Follow-up: [ROOM-STRESS.md](ROOM-STRESS.md) records full-room restart testing and a
longer **500-cycle capture failure (+226 handles)**. The 100-cycle passes below
remain valid, but do not establish sustained handle stability.

## One-command reproduction

Build `LiveCaptureTest` and `WindowsCaptureLifecycleTest` in the proof build, then:

```powershell
python scripts/test-capture-lifecycle.py build/sdk-proof-release build/webrtc/capture-acceptance-NEW
```

The default batch runs four modes sequentially, 100 cycles each:

1. Rapid source close using direct WGC capture.
2. Rapid source close with a fresh joined owner thread each cycle.
3. Hardware encode, resize, retained textures and software recovery after device retirement.
4. Production `CaptureSession` plus `WindowsCaptureSource`: minimize/restore,
   resize, injected recoverable loss, actual device rebuild and alternating active
   stop/source closure. It validates worker ownership, stop/join, captured frame
   shape, input-target generation invalidation, capture identity property removal
   and final resource release. Native driver removal is not simulated.

Only generated test windows are captured. There is no audible playback, physical
keyboard/mouse/controller injection, global driver reset or automatic desktop unlock.
The batch checks desktop availability and reports unavailable desktops as blocked.
It refuses to overwrite an evidence directory, stops on failure, bounds native logs,
and uses per-case watchdogs plus a kill-on-close process job. Native calls themselves
remain unpreemptible; timeout terminates only the test process tree.

`--cycles 20` runs a shorter check, explicitly recorded as 20 rather than 100.
The production-owner executable also has a three-cycle CTest smoke case when
`SCREENSHARE_TEST_LIVE_CAPTURE` is enabled. The existing room regression matrix
remains separate: these are capture resource tests, not full room restarts.

## Resource interpretation

The unchanged handle criterion compares median samples from cycles 6–10 with the
last five cycles, requiring growth <=8. Every cycle must complete exactly once and
every handle sample must be a nonnegative integer. Missing or malformed evidence,
nonzero native exit, timeout and excessive logging cannot pass.

Results additionally report baseline/final medians, final values and peaks for
private bytes, working set, GDI and USER objects. These are observations by default.
An explicit `--max-private-growth BYTES` adds a memory bound, using the same windows;
unknown memory cannot pass an explicitly requested bound. No arbitrary new memory
threshold is substituted for the plan's sustained-growth/soak requirement.

Each case retains native stdout/stderr, all lifecycle samples, exact command,
binary SHA-256, elapsed time and acceptance results. The aggregate records runner
hashes and links to each case. `tests/LiveCaptureStressTests.py` exercises malformed
evidence, growth rejection, missing samples, native failure, actual child timeout,
and both completed and continuous log overflow.

## Verified results — 2026-09-18

Final Release matrix: **4/4 modes, 100 cycles each (400 total), 500.06 seconds**.
Final Debug matrix: **4/4 modes, 20 cycles each (80 total), 102.26 seconds**.
All passed the unchanged +8 handle-growth bound:

| Mode | Release baseline → final median handles | Debug baseline → final median handles |
| --- | --- | --- |
| Rapid close | 329 → 329 (0) | 339 → 339 (0) |
| Fresh owner thread | 329 → 329 (0) | 339 → 339 (0) |
| Hardware/recovery | 386 → 385 (−1) | 397 → 396 (−1) |
| Production capture owner | 298 → 297 (−1) | 305 → 305 (0) |

Artifacts: `build/webrtc/stress-group-final-{release,debug}/result.json`, with
per-case reports and logs beneath each directory. A separate **100-cycle Debug
production-owner** run passed at 305 → 302 (−3), in
`build/webrtc/stress-group-owner-final-debug/result.json`. An earlier current-binary
100-cycle full run passed narrowly at +7 (`stress-group-full-before/result.json`);
this variation is retained rather than selecting only the most favorable result.

Both proof builds passed, and rebuilt focused CTest suites passed **5/5 each**:
stress evidence, capture session, distributor, backend/cursor policy and recovery
policy. The stress-evidence entry includes **17 Python tests**, including actual
child termination and malformed/mismatched report rejection. No production
teardown code changed in this batch.

Private memory has **not** passed acceptance. In the final Release matrix its
median growth was 9,461,760 / 12,197,888 / 33,284,096 / 13,541,376 bytes respectively
for the four modes above. The separate 100-cycle Debug owner run grew 10,358,784
bytes. These observations need allocation/long-duration investigation; they are
not attributed to harmless caches without evidence. Native GUI object counts and
working-set samples are retained in the same reports.

The exploratory `stress-group-owner-first` run preceded the explicit owner-mode
label and reports an incorrect generic hardware-recovery mode; use the final
reports above. Earlier exploratory matrix script hashes may span edits made
during that investigation; only the final matrices identify the finalized runners.

## Remaining acceptance

Capture handle repeats cannot close full resource acceptance. Keep these grouped
requirements open:

- Account for retained private memory and reproduce long-duration trends; driver
  caches and leaks must not be treated as interchangeable explanations.
- Full-room start/stops now pass 100 functional/handle-bound cycles in each build;
  networking/audio/runtime destruction and callback barriers are covered in
  ROOM-STRESS.md. Memory accounting and complete resource acceptance remain open.
- Run a continuous two-hour four-viewer soak with memory, queue-age and delivery
  observations, plus slow-viewer/network-impairment isolation.
- Complete physical media/input, hibernation/driver-loss, NAT/TLS, service-cost and
  externally measured image/input latency acceptance and the matched legacy comparison.

No default-backend cutover or later visual redesign is included in this batch.
