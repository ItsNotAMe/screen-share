> Historical log: campaign documents mentioned below were removed during cleanup.
> Current guidance is in [docs](../README.md#documentation) and [todo](todo.md).
> Recover an old document with `git show 376d59a:<its recorded path>`; historical
> milestones and test counts are not current implementation or release status.

# Project Memory

Owner direction (2026-11-06): prioritize substantial core features, reliability
and measured performance improvements ahead of small UI/copy polish during
open-ended continuation work. Deliver one meaningful improvement at a time and
commit each separately. Address related UI defects when they block that feature.
The owner also requested actual compute-time measurements for optimizations;
distinguish process CPU, elapsed encoder work and end-to-end sharing latency.

## Recover viewer audio when the render buffer stops draining - 2026-11-07

Separate from prior host-capture PCM watchdogs and output device-error/routing
recovery. Native WASAPI Playout::Write formerly retried successful full-padding
queries and 10 ms event waits forever when the engine stopped draining without
an HRESULT failure. That prevented the existing controlled output from retiring
and reopening the endpoint. It was a code-path gap, not a diagnosed physical
driver fault or a repeat of capture/recovery-budget renewal.

PcmRenderLoop now owns that same 480-frame write loop with native callbacks for
padding, buffer commit and the existing event wait. One unfinished block expires
after one second; events/tiny writes cannot renew it. Capacity is checked first,
allowing a delayed worker to finish into a healthy buffer. Stop is checked both
before polling and after padding, before the next commit. No new owner, queue,
timer or replay mechanism. PcmRenderStalled propagates through the existing
ControlledPcmPlayout failure path: retire on the playout worker, paced discard,
250 ms..5 s capped retry, prime with silence and pull fresh PCM. Selected output,
volume/mute and revision remain; no fallback from pinned output. Native calls
that block inside the driver still cannot be preempted in-process.

The extracted old write loop failed the new regression with "Render buffer wait
remained unbounded"; the one-second deadline passes. Supplied-clock tests cover
full capacity stalls, exact timeout, tiny-progress bounds, partial sample order,
999 ms healthy completion, a five-second delayed owner with fresh capacity,
pre-stop/stop-at-deadline/stop-during-padding and no PCM commit after cancellation.
Controlled-output tests use the same loop with fake capacity to prove retirement,
exact retry backoff, pinned settings, silence prime/fresh gain-applied PCM, muted
stall recovery and owner-thread teardown. The real H264/Opus room regression adds
a permanently full fake render buffer: video continues through expiry, output
reopens, nonzero decoded PCM resumes, revisions remain and no peer fails.

Focused NativePcmAudioTests and AvSyncRecoveryTests passed. Release `all` build,
packaged CLI/UI hashes and all 46 serial CTests passed (314.69 s; expanded A/V
recovery 36.95 s). No physical device reset/playback/capture was requested; no
native-driver qualification or CPU/latency improvement claim. Commit locally.

## Make host mouse-monitor shutdown reliable - 2026-11-07

Follow-up to the intermittent desktop-input timeout during idle-capture work.
The old monitor published its thread ID before creating a Windows message queue,
waited only 200 ms for startup and ignored a quit-post failure before joining.
That can lose shutdown during delayed startup or when the queue is full. The
original timeout's exact cause remains unproven; this change fixes a reproduced
lifecycle defect, not a diagnosed driver/input-device issue.

HostMouseMonitorThread now owns a manual-reset event created before thread start.
RequestStop signals it, including before installation or queue creation; teardown
joins, unhooks on the worker and then closes the event. The worker checks the
event before entering a message wait and between messages. MsgWaitForMultipleObjectsEx
blocks on cancellation or messages with MWMO_INPUTAVAILABLE; no polling timer,
ID-readiness sleep or quit post remains. Existing low-level mouse callback,
300 ms host-priority policy and last-owner reference counting remain. Event or
thread creation failure cannot increment the owner count. Hook-install failure
retains its previous behavior; it is exercised as a shutdown boundary.

The delayed-install regression failed with the extracted legacy quit-message
implementation ("Mouse monitor lost stop during delayed queue startup") and
passes with the event implementation. Other checks fill a private thread queue
to ERROR_NOT_ENOUGH_QUOTA, cancel it before pumping, run 100 immediate/repeated
stop cycles (including null hook results), and verify sent/posted delivery and
owner-thread cleanup through a message-only window. These seams install no input
hook and inject no input; existing desktop grant/health checks still use the
production composition without Apply/SendInput. This does not qualify physical
mouse behavior or interrupt a blocked Windows hook-install/uninstall API call.

Windows contracts: [hook owner message loop](https://learn.microsoft.com/en-us/windows/win32/winmsg/lowlevelmouseproc),
[event/message wait and already-seen input](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjectsex).
Release `all` build and packaged CLI/UI hashes passed. Twenty consecutive
desktop-input runs passed in 22.26 s, including 2,000 immediate stop cycles plus
20 delayed-start/full-queue/message-pump scenarios each. All 46 serial application
checks passed in 313.76 s (desktop-input 1.11 s). Commit locally; no publication
or version bump. No performance claim.

## Reduce capture work without active video demand - 2026-11-07

Each Windows runtime now owns an atomic captureDemand signal, shared with its
initial/replacement capture sources. The coordinator sets it from actual per-peer
deliverFrames eligibility, not membership alone. Empty rooms, connecting-only
peers, all-video pause or a zero video allowance use min(5, configured FPS) health
sampling. A returning eligible viewer restores normal cadence without an idle
deadline wait. Native bootstrap/restore stays full-rate until valid owned pixels
exist; minimized sources advance cadence too. Audio, input identity, capture
health and stop cancellation remain independent. No new worker, queue or timer;
automatic encoder threading remains. Separate runtimes have separate signals.

WGC now closes at most two older queued frames before converting the newest
available one. The final frame has an outer Close guard, including poll failures
and source resize/recreation paths. Portable queue tests exercise bounded work,
discard ownership, empty queues and errors; supplied-clock cadence tests cover
1/3/30/60/240 FPS, exact idle/active boundaries, immediate wake, no catch-up burst
and rebuild reset. The real-peer/synthetic-source regression failed before wiring
the signal with "Connected video did not restore full capture demand". It now
records six samples in each 1.2 s empty/paused interval, continues audio, resumes
connected video, ignores a connecting-only viewer and idles after the last leave.
Stop leaves no source/resource ownership behind. All portable room fixtures now
exercise optional demand using the production synthetic source/cadence.

Six active/idle AB/BA phase pairs on one generated 1080p CaptureSession owner
measure median process CPU 120.92→15.61 ms/s (87.1% lower, pairs 74.9–91.7%) and
120→10 samples per approximately two seconds. Scope: luma generation, capture
polling and handoff, excluding native WGC, codecs, networking and presentation.
This is not measured native-game or whole-app CPU savings. Process CPU timing is
coarse at the low idle load. scripts/measure-idle-capture.py records the raw
phases and source/executable hashes in docs/benchmarks/idle-capture-2026-11-07.json.
The final benchmark's hashes match the Release executable and measured sources.

NativeCaptureTests --demand[-check] compiles and covers native pixels on first
resume, input identity, minimize/restore/closure and optional native CPU phases.
It could not open its owned generated window: IGraphicsCaptureItemInterop returns
"The specified service does not exist as an installed service." Those native
assertions and native capture CPU savings remain unverified. Do not broaden this
window proof to capture the desktop or claim physical-driver qualification.

Release `all` build and packaged CLI/UI hashes passed. A fresh full serial CTest
run passed all 46 checks in 314.64 s. No version bump or publication.

The first full CTest run passed 45/46; desktop-input hit its 15 s timeout without
diagnostic output. Ten isolated repeats then passed in 11.22 s total, followed
by the clean full-suite rerun above (desktop-input 1.12 s). This test
does not create a capture source/runtime. Inspection found a pre-existing mouse
monitor startup/stop ordering risk: it publishes its ID before queue creation,
waits at most 200 ms for an ID and ignores PostThreadMessage's result before join.
[Windows requires the destination queue to exist](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-postthreadmessagea).
This is a possible cause, not an established diagnosis of that timeout; recorded
in the maintenance backlog for separate work. No input lifecycle code changed.

## Restore responsive software encoder defaults - 2026-11-07

Owner asked whether the worker-budget change was faster/slower, then delegated
the default decision for a professional sharing app. CPU milliseconds sum work
across threads; elapsed batch time is the wait for encoding completion. Chose
automatic software threading for normal sharing: the previous two-worker limit
saved CPU but increased encode delay and detailed four-viewer batch p95 crossed
the 16.67 ms frame budget. It was not a general speedup or a proven game benefit.
Do not silently make an efficiency/latency tradeoff the default. There is no
user-facing efficiency mode; explicit low-level worker requests remain possible.

The factory again leaves softwareWorkerThreads at its default zero/automatic.
The optional MF setting, two-stage negotiation setup, queried diagnostics and
benchmark evidence remain. Tests now require automatic defaults for 1/2/16-core
assignments and after FPS restart/resume, plus larger pictures; separate low-level
tests verify explicit 1/2-worker requests and invalid counts. The new default
regression failed against 89b3522 with "Normal sharing inherited an implicit
CPU-saving worker cap" and passes after restoring automatic threading.

The compatibility comparison passed all 32 runs against the original automatic
production sources (two AB/BA pairs, smooth/detailed input, CPU/GPU input and 1/4
encoders, 120 measured pictures each). Encoded hashes/byte counts are identical
across versions, and process thread counts again match the original automatic
policy. This is a compatibility check, not a new timing improvement claim;
short-run CPU measurements are noisy. Local evidence is ignored under
build/encoder-default-restore/default-compatibility.json. Release `all` build,
packaged CLI/UI binary hash checks and all 46 sequential application checks
passed (312.95 s), including the default/explicit-worker regression, recovery,
multi-viewer media/input, reconnects and UI pause/resume. No version bump or
publication; unreleased.

Next candidate is idle capture demand (see todo), which could reduce conversions
without changing active encoding latency. No capture change is committed here.
Initial incomplete drafts remain ignored under build/idle-capture. Suggested
composition: one per-runtime atomic demand bit, driven by actual deliverFrames
eligibility, read on the capture owner; 5 Hz idle health/geometry, immediate
active resume, and bounded latest-frame draining of WGC's two-frame pool. Keep
audio/input independent and qualify startup/minimize/replacement/closure behavior.

## Bound software encoder slice workers through 1080p - 2026-11-07

Historical experiment; automatic normal-sharing defaults are restored above.

The WebRTC adapter previously ignored its available-core assignment and let each
MF software encoder independently choose an automatic slice pool. It now requests
at most two workers through 1080p (one for a one-core assignment). Larger pictures
retain automatic threading. Hardware and low-level encoder callers' default
remain unchanged; the optional config defaults to zero/automatic and rejects
counts above the documented maximum of 16. Zero available cores is invalid.

Ordering matters in the tested inbox codec: set the worker count before format
negotiation to size the pool, then reapply after negotiation resets the exposed
property. Applying only afterward updates GetValue but keeps the old process
thread count and bitstream. Applying only beforehand sizes the pool but GetValue
reports zero. The new encoder-software-workers event queries the final property;
zero means automatic/unavailable. Optional setting rejection keeps compatibility.
Rate/FPS restarts, resume and software fallback retain the configured budget.

Real MF tests cover 1/2/16-core assignments, reported worker count after FPS
restart/resume, automatic larger pictures, invalid counts and existing low-latency,
keyframe, mapping, coalescing, late-output/cancellation and NV12 behavior.
EncoderComputeBenchmark now checks reconstructed luma/chroma and a deterministic
detailed moving pattern, records live process resources and batch p95/p99, and
uses a 16-core assignment to exercise the production cap. Quality checking occurs
after the CPU/elapsed measurement. The comparison runner keeps bit-exact checking
by default; explicit --quality-max-loss-db permits different slice bitstreams only
with bounded Y/UV/minimum-frame PSNR loss and stable output within each version.

All 96 matched runs passed (six AB/BA pairs for two patterns, CPU/GPU input and
1/4 encoders, 480 measured pictures each after 30 warmup). Every paired CPU
result improved. Four-encoder median CPU savings are 19.4–23.5%, single-encoder
25.4–32.9%; four-encoder paired reductions range 12.4–28.2%. Mean batch elapsed
time increases 1.30–1.69 ms for four encoders and 2.38–2.64 ms for one. Detailed
GPU/four-encoder batch p95 increases 12.04→16.89 ms and p99 13.50→18.52 ms, so
do not claim preserved 60 FPS under game load or lower end-to-end latency.
Smooth-pattern decoded Y/UV PSNR falls 0.217/0.466 dB; detailed improves
0.747/0.444 dB. Actual VBR rates differ despite the same 12 Mbps assignment;
bitstreams are stable within each version, not equal across versions. Four-
encoder process threads drop 57→21 (CPU input), 80→44 (GPU input). Slightly
lower private bytes belong to this benchmark, not the whole application.
See docs/performance.md and its tracked raw runs/source/executable hashes.

Release `all` build and packaged CLI/UI binary hash checks passed. Final
sequential application validation passed all 46 checks (311.47 s), including
the expanded real-MF worker policy test, audio/capture/decoder recovery,
multi-viewer media/input, reconnects and UI pause/resume. No version bump or
publication; unreleased.

## Recover open host audio devices that stop delivering PCM - 2026-11-07

The earlier host-audio recovery handled endpoint errors and a recovery attempt's
first-PCM deadline, but an initially opened, established or manually switched
System/Microphone endpoint could remain open with Read blocked indefinitely.
SwitchablePcmCapture now applies a five-second missing-PCM deadline throughout
these recoverable endpoints' lifetime. Every fresh queued PCM block renews it,
including all-zero audio; generated filler silence does not.
WASAPI's bounded idle read already emits zero PCM after 100 ms without packets,
so ordinary idle loopback keeps this deadline alive. Fresh queued PCM
is consumed before checking expiry so a delayed owner tick does not falsely
retire a working device. On expiry the same existing owner cancels/releases the
producer and uses the pinned-device 250 ms..5 s retry policy. No new thread,
queue, default-device substitution or revision change; microphone processing is
recreated through the existing factory. Manual selection and Close keep priority.
Shared-source capture retains its inner validated-target recovery, and explicit
process targets gain no automatic reopening policy.

The real adapter/synthetic-endpoint regression failed against the old code with
"Open host audio device stalled indefinitely without PCM recovery". Tests cover
initial/established/manually selected stalls for System and Microphone, exact
4999/5000 ms expiry and 249/250 ms retry boundaries, zero PCM recovery, owner
delay with queued fresh PCM, pinned identity/revision, fresh microphone processor,
nonrecoverable-target policy and worker-thread cleanup. No physical audio device
or driver stall was injected; this cannot preempt a driver that ignores stop.

An encoder output-buffer reuse candidate was measured and removed before this
fix: six AB/BA pairs at 1/4 software encoders with CPU/GPU input preserved output
but gave inconsistent CPU/time results, while retaining about 3.7 MiB per 1080p
encoder. Raw local experiments remain ignored under build/encoder-output-reuse.
Do not repeat this candidate assuming fewer allocations prove a CPU improvement.

Release `all` build and packaged CLI/UI binary hash checks passed. All 46
sequential application checks passed (313.35 s), including the expanded PCM
checks (15.58 s), multi-viewer media/input, reconnects, capture replacement and
UI pause/resume. No version bump or publication; unreleased.

## Bound missing output after restoring minimized capture - 2026-11-07

CaptureSession refreshed its first-frame deadline during minimization but left
awaitingFrame false once the source had previously delivered healthy pixels.
If restoration produced no frames, it could remain Minimized indefinitely.
Minimization now rearms that existing watchdog. Its deadline continues moving
while minimized; the first valid restored sample clears it. A silent restore
times out after the configured interval (5 s by default). Switchable capture
releases the failed source and waits for explicit replacement; ordinary capture
fails. Healthy restoration retains the source and generation. No new timer,
thread, frame queue or automatic reopening. Recovery and unavailable-source
paths already rearm the same watchdog; this is separate from budget renewal.

The new real CaptureSession/synthetic-source test failed against the old code
with "Silent restore never reached its missing-frame timeout". With the fix,
both ordinary and switchable sources pass a healthy restore followed by a
silent restore. Each minimization lasts twice the shortened test timeout while
the source continues producing pixels, checking no stale delivery or premature
timeout. The test also checks failure classification, owner-thread disposal,
no implicit rebuild/reopen and successful explicit replacement. This does not
reproduce a physical WGC/driver stall or make a performance claim.

Release `all` build and packaged CLI/UI binary hash checks passed. All 46
sequential application checks passed (308.48 s), including multi-viewer media/
input, reconnects, UI pause/resume and capture replacement. No version bump or
publication; unreleased.

## Renew capture recovery after healthy output - 2026-11-07

CaptureRecovery previously spent its three rebuild attempts across the entire
capture session. A fourth device loss stopped an ordinary source or abandoned a
replaceable one even after healthy video between every outage. The capture owner
now renews that budget after 30 consecutive valid owned capture samples. Pending
rebuilds, empty polls, elapsed time and minimized/unavailable sources earn no
credit. Skipped GPU frames, minimization, unavailability and each loss clear the
healthy streak. Frame validation precedes credit; delivery need not be enabled
for capture itself to establish health. No new worker, timer or retained frames.

The three-attempt limit still bounds unstable capture, retirement still happens
on the terminal loss, failed reconstruction stays terminal, and the existing
250 ms cancellable backoff remains. Generations remain cumulative across renewed
budgets. Only explicit replacement resets a terminal source's recovery policy.

The new real CaptureSession/synthetic-source regression failed against the old
code on the fourth loss ("Healthy capture exhausted its lifetime recovery budget")
and now recovers five losses separated by healthy capture on the same owner.
Application tests additionally cover exact 29/30-frame boundaries, no pending/
time-only credit, interrupted streaks, terminal non-resurrection, replacement
and the existing rapid-loss/rebuild/consumer/startup failure boundaries. The
headless capture ownership proof passed 100 stop/restart cycles, including Stop
during backoff. The optional native capture proof now expects five healthy-
separated recoveries; it was compiled and its headless policy mode passed, but
its live Windows capture mode was not run. These injected faults do not qualify
physical driver loss, WGC cleanup or the reported laptop's handle growth.

Release `all` build and packaged CLI/UI binary hash checks passed. The final
sequential application suite passed all 46 checks (310.20 s), including the new
capture regression, four-viewer media/input, reconnects and UI scenarios.
No version bump or publication; unreleased.

Owner correctly noted this repeats the earlier decoder/presentation budget
pattern. Capture's history confirms it was a separate missed path, not a
regression of those commits. Related limits were checked together: decoder and
presentation already renew after healthy output; ICE uses a rolling one-minute
budget; viewer settings reset attempts per revision; audio uses capped retry
delays rather than a lifetime attempt cap. Hardware encoder quarantine remains
intentionally latched for the session after a backend fault. Check related paths
together when changing recovery policy rather than presenting each occurrence
of the same pattern as an unrelated discovery.

## Recover transient viewer settings rejection - 2026-11-07

NativeRoomRuntime previously marked a settings revision attempted before asking
WebRTC to accept it and never retried a rejection. A rejected resume could leave
the previously applied paused sender/source indefinitely. ViewerStreamSettings
now polls per-peer recovery on existing coordinator ticks: up to three attempts,
250 ms apart, refreshing RTP parameters each time. New revisions bypass backoff;
success and terminal errors stop polling work. No new threads or queued updates.
An initial sender rejection retains the peer only during this bounded recovery;
permanent initial failure still retires it. A partially committed transport-cap
failure remains terminal. Existing working settings survive permanent rejection.
Capture delivery also gates directly on requested video pause, even if the RTP
pause update is rejected. This stops new capture submissions, not already queued
network packets. Pending retries remain pending in stream status until exhausted.

Deterministic fake-sender/real-source tests cover transient resume rejection,
fresh parameter reads, retry timing and bounds, another independent viewer,
immediate superseding pause, permanent rejection, structural/validation/stale
errors and recovery during initial settings. The UI removal scenario now repeats
three pause/resume cycles by default (3..30 via SCREENSHARE_PAUSE_CYCLES), can
hold each pause up to 5 s via SCREENSHARE_PAUSE_HOLD_MS, and prints redacted
runtime/input plus renderer/freshness/focus evidence if resume times out.

Forty-eight baseline cycles (including twelve held 1.5 s) and twelve updated
short-pause cycles passed. The first full suite then reproduced the timeout
(45/46 passed, 319.33 s). Its new failure evidence records fresh presented video,
Live telemetry, an empty video status, both host settings applied, the controller
grant retained, and an inactive target with the explicit click-to-resume hint.
The fixture was waiting for held input without restoring focus; the application's
focus protection correctly kept input neutral. The test now waits for fresh video
separately, checks that permission survived, confirms neutral input while inactive
and reactivates the target before expecting controller output. Alternate cycles
deliberately focus the survivor to exercise this protection deterministically.
Application focus protection is unchanged. This diagnoses the captured fixture
failure; it does not establish the state of older failures without diagnostics.
The injected rejection regression independently establishes the recovery gap
fixed in production. No physical input, native-window presentation or actual
network error-frequency claim.

With the focus-aware fixture, twelve cycles held paused for 1.5 s passed
(34.74 s), including deliberate focus changes and restoration of the same grant.
Release `all` build and CLI/UI packaged-binary hash checks passed. No version
bump or publication; unreleased. The final sequential full suite passed all
46 checks (301.57 s), including the focus-aware viewer-removal scenario.

## Copy decoded visible pixels directly from MF - 2026-11-07

Cropped CPU decoder output previously allocated/copied the whole coded frame,
then allocated/copied its visible rows. H264StreamDecoder now copies the crop
straight from the locked MF buffer into independently owned frame pixels,
removing that temporary coded-frame vector/copy (about 3 MiB for 1920×1088).
The visible output allocation remains. An unlock guard covers crop validation/
allocation exceptions. Uncropped output and GPU texture ownership retain their
paths. Existing crop geometry validation is reused against actual locked bytes.

Matched 850fd02 versus updated Release executables use the identical benchmark
and fixed 64-picture 1080p fixture, one worker per decoder, 64 warmup pictures
then 960 measured pictures each. Six AB/BA pairs each for one/four decoders:
four-decoder median process CPU 6.952→6.104 ms/picture (−12.2%), elapsed
4425.46→3708.36 ms (−16.2%). Every four-decoder pair improves CPU (5.9–19.0%)
and elapsed time (13.3–24.4%). Single-decoder elapsed improves 16.4%, but CPU is
noisy: median −4.3%, paired range −5.2..32.6% reduction. Decoded byte counts and
full-frame checksums match across all 24 runs. Source/fixture/benchmark/binary
hashes are tracked in docs/benchmarks/decoder-visible-copy-2026-11-07.json.
Both versions already reuse MF scratch buffers; their warmed allocation counter
stays zero and does not count the removed pixel vector. Scope is decode, output
copy/crop, full-frame checksum and drain, excluding startup/warmup/teardown,
capture, transport and presentation. No total application CPU/memory or physical
input-to-display latency claim; hardware handle lifetime remains unqualified.

Real MF tests now also require uncropped 1280×720 among changing cropped/resized
outputs, explicitly verify cropped 1080p geometry, and retain earlier frames
across reuse, resize and Stop/restart. Generated GPU decoding, retained textures,
pressure, retirement and software fallback passed with the production change;
the later added geometry assertions also pass in the CPU lifecycle test.

Release `all` build passed and packaged CLI/UI hashes match. The sequential
full application run passed 45/46 checks (318.82 s): viewer-removal timed out
at RoomUiTests.cpp:777 waiting for Live video, held controller input and a cleared
video status after host unpause. This same intermittent pause/resume condition
was recorded before this change. Its focused rerun passed (11.75 s). Do not
claim a clean full-suite run or a fix for that timing/runtime issue. The decoder
regressions, four-viewer media/input, UI and presentation checks passed.
The final production source and benchmark binary still match recorded hashes.
No version bump/publication; unreleased.

## Theme the white horizontal scrollbar - 2026-11-07

Owner pointed out the white horizontal scrollbar/end buttons in Stream details.
Only QFileDialog had horizontal scrollbar rules; the viewer diagnostics table
inherited Qt's light fallback while vertical scrolling already used the theme.
Promoted the existing horizontal track/thumb/hover/end-button/page and scroll-area
corner rules to the shared stylesheet, preserving their geometry and palette.
This also covers horizontal scrolling elsewhere in the application.

Compact/normal Stream details renders were inspected: the track, rounded thumb
and corners are dark, with no white end buttons. The focused admission/report-
export UI scenario passed after the change. Release `all`, all 46 sequential
application CTests (301.29 s) and packaged CLI/UI hashes passed with both the audio
recovery and theme changes. No image mockup or layout redesign was needed.
Committed separately from audio recovery at the owner's request; unreleased.

## Recover host microphone/system capture automatically - 2026-11-07

SwitchablePcmCapture previously stayed silent after a System/Microphone producer
failed until a manual selection/retry. A regression against b41a474 reproduced
the missing recovery. It now retries the committed factory with 250 ms–5 s
capped backoff, preserving explicit device IDs and audio/room revision. Recovery
starts asynchronously on a producer owner; paced 10 ms silence continues while
activation waits. First PCM marks Running, and a five-second first-PCM deadline
retires stalled producers that honor cancellation. Each microphone open recreates
speech processing. Old PCM queues are discarded and drop counters survive.

Manual Retry bypasses backoff; source changes/mute supersede in-flight automatic
activation. Close/Stop prevent reopening. Shared-source mode retains its separate
validated-target policy; explicit process IDs retain manual retry rather than
following PID reuse. No default-device/app fallback is introduced. Native calls
that ignore cancellation remain non-preemptible.

The injected-clock/device suite covers repeated activation/read failures,
capped delays, two capture kinds, pinned selections, unchanged revisions, fresh
PCM, processor recreation, mute/change/manual-retry priority, first-PCM timeout,
Close/stop and owner-thread cleanup. The source-switch public UI proof resumes
default system capture without a Retry click while video continues, preserving
selection/revision. The synthetic UI factory does not propagate initial native
device IDs; pinned recovery is covered at the shared control/producer boundary.
Host recovery messages were rendered/inspected at compact and normal sizes.
No physical capture, audible output, hot-plug or driver-reset qualification.
This is reliability work, with no compute-time/latency improvement claim.

Release `all` build passed. All 46 application CTests passed sequentially
(301.29 s), including the expanded native PCM suite (13.89 s), room media/input,
CLI and UI. The focused source-switch public UI proof passed (14.02 s), and
admission-feedback/recovery-message renders passed at compact/normal sizes.
Packaged CLI/UI hashes match. No version bump or publication; unreleased.

## Renew presentation recovery after healthy playback - 2026-11-07

Presentation previously allowed only three GPU rebuilds across a viewer's whole
session. A fourth isolated device fault stayed terminal even after healthy video
between outages. A deterministic regression reproduced this against 82b2fc4.
Thirty successfully presented incoming frames now renew the attempt budget.
FramePresentationSession identifies fresh-frame work explicitly; Update/redraw
success and busy/occluded/minimized/unavailable frames cannot earn health credit.
Any recoverable device fault clears that health count. The existing 250 ms
drop-only backoff, failed-frame discard, supported DXGI errors, nonrecoverable
error handling and terminal exhaustion remain. Diagnostic recoveries stay
cumulative; only Clear resets that total and can revive terminal presentation.

Supplied-time policy checks recover five isolated outages, then require three
29-frame recoveries to exhaust on the fourth unstable fault. Redraws/skips cannot
cross the boundary, and elapsed time alone cannot revive terminal presentation.
The shared UI/CLI session checks redraw/skip isolation. The actual widget worker
with an injected renderer recovers five outages with 30 accepted incoming frames
each, cumulative counters, correct ownership and the one-frame queue setting.

The native Windows UI test was extended with four healthy-separated losses and
real GPU resource recreation. Two local attempts could not reach the existing
first-frame assertion: DXGI reported occlusion, zero accepted frames, no renderer
errors and roughly 925 occluded drops. A test-window visibility experiment did
not help and was removed. These runs did not reach the new native assertions;
they do not qualify GPU recovery or physical driver loss. The independent policy,
shared-session and offscreen-widget checks pass. No physical input/virtual-pad
allocation or graphics driver reset. This is reliability work, not a CPU or
end-to-end latency claim.

Release `all` build passed. All 46 application CTests passed sequentially
(306.68 s), including the expanded policy/shared-session/widget checks (6.89 s),
CLI, four-viewer media/input and room UI. Packaged CLI/UI hashes match.
Native GPU recovery remains unqualified for the occlusion reason above.
No version bump or publication; this improvement is unreleased.

## Keep software video alive after a completed encode stall - 2026-11-07

The software encoder treated a successful encode taking over 500 ms as a fatal
error, setting failed_ and rejecting subsequent input until reinitialization.
A real MF regression with a 550 ms input-mapping delay reproduced this against
7b561f5: "Completed software stall poisoned subsequent encoding".

Late completed output now has a separate nonfatal drop path. Keep the live
transform/latest pending input, force the next fresh picture to an IDR (the
receiver never saw the dropped reference), and record encoder-late-frame-dropped
with elapsed work milliseconds. Generation cancellation suppresses the late
callback/event. Actual software codec errors retain terminal behavior and
hardware's missing-output deadline still quarantines/falls back as before.

The regression exercises three completed stalls, including latest-only queued
input, exact drop accounting, fresh RTP/NTP/mapping and independent decoding of
each recovered IDR with a new MF decoder. Codec-ready events prove no implicit
restart during recovery. A fourth stall is cancelled by Release; old callbacks
stay suppressed and a new configuration produces a clean initial keyframe.
Generated GPU-readback software encoding also passes. This injects input mapping
delay, not a physical game/driver hang, and cannot preempt stuck native calls.
This is a reliability change, with no CPU/latency improvement claim.

Release `all` build passed. All 46 CTests passed sequentially (299.58 s),
including the extended encoder regression, multi-viewer media, input and UI.
`EncoderAdapterTests --gpu-readback` passed separately with the final test source;
packaged CLI/UI hashes match. No physical input or virtual-controller allocation.
No version bump or publication; this fix is unreleased.

## Reuse software decoder output storage - 2026-11-06

H264StreamDecoder previously allocated a large caller-owned MF output buffer on
every ProcessOutput poll, including NEED_MORE_INPUT. It now retains one buffer
per decoder, resets its length and creates a fresh sample on each poll. Published
CPU pixels still own copies, format changes discard scratch storage, Stop frees
it and Start resets allocation counters. Existing size limits stay authoritative;
MFT-supplied hardware samples retain their path. The retained scratch buffer is
about 3 MiB per idle running 1080p software decoder, at most 128 MiB.

An identical generated 64-picture 1080p fixture and benchmark were compared with
instrumented ff38fa3 in six alternating AB/BA pairs for one and four independent
decoder threads: 64 warmup pictures, then 960 measured pictures per decoder.
Median elapsed time improves 3595.84→3363.89 ms (6.5%) and 5162.26→4623.26 ms
(10.4%). CPU ms per decoded frame improves 5.916→5.420 (8.4%) and 7.731→7.153
(7.5%), but paired CPU ranges include regressions: −5.4..17.2% and −1.3..11.2%.
Every elapsed pair improves. Measured large-buffer allocations fall 1921/7684→0;
two allocations per decoder occur during warmup. Decoded byte counts and full-
frame checksums match. Raw runs/source/binary/fixture hashes are tracked in
docs/benchmarks/decoder-compute-2026-11-06.json; scope is in docs/performance.md.
This measures decode, owned output copies/crop and checksum, excluding startup,
warmup, teardown, capture, networking and presentation. No total application CPU,
input-to-display latency or hardware handle-lifetime claim.

Real software regression checks changing pixels across 320×180→640×360→1080p→
320×180, bounded allocations, timestamp association, retained immutable frames
and Stop/restart cleanup. The explicit generated-GPU variant also passes hardware
resize, retention, pressure and software fallback. No physical desktop capture,
input injection or virtual-controller allocation is used.

Release `all` build passed. All 46 CTests passed sequentially (299.93 s);
`DecoderAdapterTests --gpu` passed separately. Packaged CLI/UI hashes match.
The measured decoder sources, benchmark source and executable still match the
recorded hashes. No version bump or publication; this improvement is unreleased.

## Preserve control grants through temporary lag - 2026-11-06

Owner report: released v1.0.12 revoked controller permission when the captured
game/capture lagged. Two code paths could do this: the 300 ms input watchdog
fully revoked grants, and expired desktop capture geometry failed mixed-input
ownership health after one second. The exact physical game incident was not
reproduced. A regression reproduced watchdog revocation against the old code.

Sink::Pause now neutralizes held input on the input owner while retaining grants,
devices and slots. Native GamepadSink verifies a zero-state submission without
Destroy; DesktopSink releases held keys/buttons and delegates pad neutralization.
After a gap, the host rotates the existing Permission epoch with unchanged
capabilities, clearing pre-gap queues. Viewer cache clearing/poller recreation
requires a fresh report, including unchanged physical state; heartbeat alone
cannot resume a paused sink. Receipt time is independent of delayed backend
execution, so fresh queued updates prevent an unnecessary watchdog pause.

Actual backend failure, failed/unsupported Pause, source invalidation/change,
disconnect, explicit release/revoke and reliable-channel pressure retain full
release/revocation. Source identity survives stale geometry; desktop injection
and new grants still require fresh Read(), while ownership uses Identity().
This avoids stale injection and preserves exclusive ownership during capture lag.
Host UI keeps grant icons checked and shows Input paused; reports include paused.

A second regression caught viewer Release crossing renewal: pending withdrawal
is now forwarded under the renewed epoch. Unsent zero-capability acknowledgements
are retained before a newer explicit grant, preserving quick release/regrant.
Host revoke also wins over a blocked Pause completion. No wire format changes;
use updated clients at both ends for the complete withdrawal/fresh-video behavior.

Injected tests cover three repeated gaps, allocation/slot retention, stale-token
rejection, no cache/heartbeat replay, unchanged-report recovery, a delayed owner,
failed Pause and concurrent revoke. A four-viewer public-room proof stalls runtime
advancement and resumes transported input without another host grant. The UI
fixture checks two gaps, checked icons, neutral state and automatic fresh-reader
resumption; its paused controller card was rendered/inspected. No physical input,
native virtual-device allocation or physical game/driver-lag qualification.

Release `all` build passed. All 46 CTests passed sequentially (299.20 s),
including UI, viewer removal, four-viewer input/media and presentation checks.
The focused controller UI scenario also passed, and the paused grant card was
visually checked with the existing dark theme. Packaged CLI/UI hashes match.
No version bump/publication; this fix is unreleased. No performance claim.

## Keep rooms open after capture source failure - 2026-11-06

A failed acquisition/rebuild or exhausted capture recovery previously terminated
the room for all viewers. CaptureSession now lets a switchable source abandon
its failed native state and enter SourceUnavailable while preserving the source
selection mailbox and capture distribution. The failure reason remains visible
until fresh output; abandonment advances the source generation and clears the
recovery budget for the next explicit selection. It never automatically opens
a replacement. An already pending replacement survives failure of the old source.
Native release/destruction stays on the capture owner. Initial Start failures,
unwrapped sources and consumer errors retain terminal behavior.

NativeRoomRuntime releases held input and disables new grants while capture is
unavailable; grant availability resumes with fresh capture and no implicit
regrant. The host sees Capture unavailable plus Change source guidance, with
disabled grant icons and explanations; normal closed-window copy remains.
Viewers use the existing source-unavailable feedback, and audio connections stay
open. This follows the existing UI/UX skill patterns without an image mockup.
Compact host layouts stack capture actions and leave secondary measurements in
Details so Change source remains readable; compact and normal renders were
inspected with the injected failure active.

The new regression failed against the previous capture loop and covers read/
rebuild failure, exhausted recovery, first-frame timeout, bounded recovery after
replacement, monotonic identities, failed/pending replacements, cancellation and
failure boundaries. The four-viewer production-room proof injects read failure,
requires continued audio and held-input release, then explicit source replacement
with unchanged room/peer/connection identities. Two-viewer UI coverage exercises
failure/replacement and grant controls. No physical graphics reset, driver hang
or reported deferred WGC cleanup behavior is qualified by these injected faults.
The first full run had a viewer-removal pause/resume timeout; its focused rerun
and next full run passed. No fix for that intermittent behavior is claimed by
this source feature. The next run exposed a PCM/status-observation race in the
public output-recovery proof: audible blocks could precede the cached Running
status. That proof now waits for both observations before checking recovery.
Another run exposed the decoder recovery proof's immediate-output assumption;
it now counts actual valid output with bounded pending associations and checks
short-burst exhaustion instead of depending on exact input/output simultaneity.
A presentation backoff test also failed its wall-clock call-count assertion in
one parallel run; it had passed earlier runs. These intermittent checks are
recorded separately from the consistently passing capture-failure scenarios.

Release `all` build passed; packaged CLI/UI hashes match. All 46 CTests were
exercised, but the parallel full runs were not uniformly clean. After the proof
corrections, the final sequential decoder/capture/four-viewer/presentation checks
all passed (40.51 s), and the focused two-viewer source-switch UI passed with
normal/compact renders inspected. Do not report a clean full-suite run or a fix
for the intermittent viewer pause/presentation timing behavior. No version bump,
publication or performance claim.

## Renew decoder recovery after sustained valid output - 2026-11-06

The MF decoder previously exhausted its three software-rebuild attempts across
an entire configuration, even when long stretches of healthy video separated
failures. Thirty validated output frames now renew the budget; any decode error
clears that healthy count. Accepted/buffered input alone cannot renew it. Valid
GPU output dropped for consumer retention pressure still counts. The 250 ms
backoff, keyframe requirement and session-wide hardware quarantine remain.

A real MF regression test uses a complete valid picture with an undersized
declaration to fail output validation deterministically. It failed on the old
implementation at the fourth isolated recovery. The fix recovers five outages
with 30 real output frames between them (150 callbacks); an additional scenario
with only 29 valid frames between failures still exhausts the three-restart
budget. No corrupted driver behavior or physical capture is required.

DecoderAdapterTests is now part of ordinary application CTest, bringing the
suite to 45 tests. Its generated-input --gpu checks also pass: texture pressure,
retirement, software fallback, malformed-input exhaustion, quarantine across
reconfiguration/replacement, cropping and retained-frame lifetime. The reported
laptop Section-handle growth and hardware encoder deadlines remain unresolved.

Release `all` build and all 45 CTests passed (151.39 s); explicit generated GPU
decoder checks passed, and packaged CLI/UI hashes match. No version bump or
publication. This is a recovery correctness change, with no performance claim.

## Follow default host audio routing - 2026-11-06

Default System audio and Microphone capture now register Windows console-role
default-device notifications before endpoint resolution/activation. Render and
capture flow filtering share the existing playback notification implementation.
Callbacks only increment an atomic revision; RoutedPcmCapture retires/reopens
native endpoints on the existing capture worker, discards old native pending or
in-flight PCM and primes replacement capture with paced silence. Changes during
reads/startup are detected, coalesced notifications avoid duplicate activations,
and cancellation prevents further opens/adoption. Drop counts accumulate across
endpoint retirement. Explicit device IDs and process capture remain pinned.

This follows healthy default-route changes without changing room/audio settings
revisions. Replacement failures retain the existing policy: shared-source mode
retries with capped backoff, explicit System/Microphone failures use manual
retry. It does not add a separate worker, retry loop or replay queue. Mute/Stop
retires the producer. Injected tests cover route races, fresh PCM, cumulative
drops, cancellation, callback roles/flows and factory classification; the four-
viewer proof requires continued audio/video and stable room/audio state across
a capture route change. Physical device hot-plug/driver behavior is unqualified.

Release `all` build and all 44 CTests passed (141.15 s). The packaged CLI/UI
executable hashes match the build. No version bump or publication.

## Direct MF sample input and paired encoder timing - 2026-11-06

Software NV12 now copies plane views directly into an owned MF input sample,
removing the temporary per-frame CapturedFrame vector and packing copy. The new
EncodeNv12 API validates dimensions, strides and minimal plane spans before
consuming frame state. Views are consumed synchronously and MF owns independent
bytes afterward. I420/hardware routes retain their existing behavior; synchronous
software submission is shared with the original EncodeFrame path.

The requested timing audit found the preceding NV12 change (2c2a5fa→05582cc)
reduced CPU-backed elapsed encode time 8–12% across four AB/BA pairs, with no
clear GPU-backed elapsed gain. CPU time was noisy. The new direct-plane change
was compared against 05582cc across six AB/BA pairs, 480 measured frames after
30 warmup frames, using generated changing 1080p NV12 at 60 FPS/12 Mbps settings.
Elapsed encoder-workload time fell 8.9–17.7%; four GPU-input software encoders
fell 8.486→7.571 ms/input batch (10.8%), all six pairs faster. CPU readings are
mixed: four-GPU-input median 16.553→16.036 CPU ms/encoded frame, while one CPU-
input encoder's median increased. Every paired CPU range includes a regression;
do not claim general CPU savings. Output byte counts and 64-bit hashes match.
These unpaced encoder measurements exclude capture/network/decode/presentation.

EncoderComputeBenchmark and compare-encoder-compute.py retain the reproducible
workload/alternating-run method. The earlier source was built in an isolated Git
archive snapshot with the same SDK/Release settings; active source was preserved.
Raw runs, binary hashes and source identities are tracked under docs/benchmarks;
docs/performance.md contains the full table and limitations. No machine-wide
scheduling changes or physical input/audio/capture were used.

The final Release build and all 44 CTests passed (141.86 s), including new plane
boundary/rejection/start/stop checks and valid minimal final rows. Explicit GPU
NV12 encoder checks passed, packaged executable hashes match, and measured source
hashes still match the final implementation. No version bump or publication.

## Direct NV12 software encoding and readback - 2026-11-06

The MF adapter now packs CPU NV12 directly, including shared GPU NV12 readback,
instead of converting to I420 and back to NV12 for each software encoder. Native
same-device hardware submission is unchanged. GPU readback allocates the first
requested CPU format only; I420-first callers retain their original direct
conversion path. A second requested format derives from owned CPU pixels without
another GPU readback. Mapping follows requested format order, stays under the
frame mutex and rejects retired devices even when cached. Explicit GPU preview
CPU materialization also uses NV12 directly. Per-viewer packing/MF sample copies
remain; no total CPU/memory/latency percentage or zero-copy claim is established.

The new four-encoder test failed before implementation with 32 ToI420 calls for
eight changing padded NV12 frames delivered to four real software encoders.
Afterward it reports zero calls, while decoding all 32 outputs validates luma,
chroma, dimensions, timestamps and input mapping. The generated GPU variant
encodes the same number of frames with eight shared readbacks and zero I420
conversions. Additional probes cover preview pixels, both format request orders,
cache ownership and retirement. The existing encoder lifecycle proof now runs
in application CTest as encoder-adapter-lifecycle (44 tests total).

The final full Release build and all 44 CTests passed (139.44 s). Explicit
EncoderAdapterTests --gpu-readback and StreamSettingsTests --gpu runs passed;
the latter preserves the earlier 96-to-24 shared-scaling/readback result. The
portable archive matches both executables. Tests use generated pixels and silent
synthetic endpoints, not physical input/audio or desktop capture. Full-session
resource benchmarks and broader hardware qualification were not rerun. No
version bump or publication occurred.

## Pause desktop remote control when video stalls - 2026-11-06

The desktop viewer now requires a frame actually presented within two seconds
before forwarding remote input. Decode/enqueue acceptance alone cannot enable
control. Presentation retains each frame's arrival time across redraws; skipped
presents do not refresh it. Clear immediately invalidates mapping/freshness and
advances a generation so an in-flight old render cannot restore them afterward.
The input gate starts closed, releases held mouse/keyboard input and sends neutral
controller state on a stall, retaining consent and the host's permission. Fresh
presentation restores availability under the existing focus policy; Game mouse
requires another click. The blank video surface says Video interrupted. Explicit
host pause/source notices take priority. This changes the Qt desktop flow, not
the CLI's input policy.

The new desktop-input scenario failed before implementation, with a live room
and decoding but a frozen UI consumer. It now checks held releases, blocked clicks,
retained permission/revision and recovery. Controller neutralization/recovery
also passes, and unit proofs cover the freshness boundary, redraw/skip behavior
and a render completing after Clear. Offscreen controller fixtures exposed zero
successful native presents despite hundreds of enqueues: input/media UI proofs
now inject the existing recording presenter, including through RoomApplication's
config constructor. Explicit Windows UI proofs retain native presentation. This
does not qualify every real renderer or reproduce a specific WAN failure.

The full Release build and all 43 CTests passed (143.17 s). Focused desktop-input,
controller, viewer-removal and source-switch scenarios passed; compact/normal
interruption renders were inspected and packaged CLI/UI hashes match. All input
sinks were synthetic. No version bump or publication occurred.

## Shared video scaling for matching viewers - 2026-11-06

Immutable D3dVideoFrameBuffer inputs now cache at most four successful scaled
outputs keyed by canvas size and the complete image rectangle. Matching viewer
tracks reuse those GPU pixels and their existing lazy CPU conversion, while
adaptation, settings/preset/mapping metadata and encoding stay independent.
Eviction retains consumer ownership; new inputs cannot reuse old pixels.
Retirement/quarantine bypass the cache and busy/failure results are not cached.

The generated-texture GPU proof first failed on unchanged behavior: both
separate and shared inputs required 96 scales/readbacks for 24 changing 1080p
frames delivered concurrently to four 720p sources. With caching, shared input
requires 24 scales/readbacks and one output texture per frame instead of four.
Pixel freshness, preset isolation, complete geometry keys, four-entry eviction,
device retirement, independent adaptation, fallback and teardown pass. This
measures 75% fewer scaling/readback operations for matching outputs, not total
session CPU/memory/latency. No full-session scorecard was rerun.

The full Release build and package hash checks passed. The existing stream
settings CPU proof is now included in application CTest (43 tests); its optional
GPU assertions were corrected to unwrap frame metadata before inspecting the
underlying pixel buffer. The full parallel run passed 42/43; viewer-removal hit
the previously recorded controller-resume timeout at RoomUiTests.cpp:685, then
passed standalone. That intermittent issue is not fixed by this change. The
explicit StreamSettingsTests --gpu run passed. No physical input/audio, version
bump or publication occurred.

## Automatic shared-source audio recovery - 2026-11-06

Shared-source audio now retries temporary endpoint startup/read failures on its
existing capture worker, using exponential backoff from 250 ms to five seconds.
The selected source remains authoritative: unavailable apps produce silence,
process failures never fall back to system audio, and a changed target bypasses
old backoff. Reads spanning source changes discard old PCM. Drop counters retain
retired endpoints' totals. Video and the outer 10 ms audio clock continue during
recovery; mute/stop cancel the old producer. The UI explains automatic retry.
Explicit microphone/system/process modes retain manual retry, and healthy host
default-output changes are outside this change.

The final full Release build and all 42 CTests passed. Fake-clock tests cover
capped retries, same-target recovery, fresh PCM, source isolation, cumulative
drops and cancellation during activation. The four-viewer public-session proof
uses the real shared-source wrapper with synthetic endpoints; audio recovers
without a command or changed room/audio revision, and muting during a second
outage leaves no capture endpoint or further retries. Compact/normal recovery
guidance renders were inspected and packaged CLI/UI hashes match. Native driver
failure/recovery remains unverified. No version bump or publication occurred.

## Automatic viewer audio continuity - 2026-11-06

Viewer default output now follows render/console endpoint notifications using a
nonblocking atomic callback. The playout owner retains registration until
unregister and catches changes during activation or a blocked write. Explicit
device selections remain pinned. Controlled output retries failures with 250 ms
exponential backoff capped at five seconds, keeping decoded audio paced/discarded
instead of queued during outages. Recovery validates with silence then resumes
fresh PCM, retaining volume, mute, selection and settings revision. Manual
updates take priority and Close/cancellation prevent further activation.

Playback settings explain automatic retry. Outage rendering exposed an existing
Retry button using its default 640x480 geometry over other fields: its row was
hidden with QFormLayout but restored through QWidget only. Status now restores
the row consistently, with visibility changes guarded to avoid layout churn.
Output/volume/mute remain reachable and Retry disappears after recovery.

The final full Release build and all 42 CTests passed (139.63 s). Deterministic
PCM proofs cover capped backoff, repeated failure, startup/write notification
races, coalescing, mute/gain, manual override, pinned output and Close/stop races.
The four-viewer public API proof reroutes and recovers one viewer while others
continue, without changing room identity or playback revision. Older manual-only
retry expectations were updated. A focused source-switch UI run passed with the
outage drawer rendered and inspected; packaged CLI/UI hashes match executables.
Notifications/devices were injected: physical headset hot-plug and all-driver
qualification remain unverified. No version bump or publication occurred.

## Persistent remote-control pause guidance - 2026-11-06

The viewer's existing Controls hint now reports actual forwarding pauses while
permissions remain granted. Video unavailability takes priority and says to
wait for video; a focus/mode pause explains clicking the video to resume. The
normal Game mouse guidance returns when the gates reopen, and shortcut reminders
remain in the same label. Input routing, focus policy and grant semantics are
unchanged; the label is explicitly plain text.

The full Release build, focused local-service desktop/controller scenarios and
three relevant UI/update checks passed. Tests require pause/recovery text on
focus and mode changes, held-state release, retained permissions, blocked input
while paused and video-wait guidance until fresh video. Compact/normal paused
renders were inspected and the portable archive matches both executables. All
input sinks/devices were synthetic; no physical input, version bump or
publication occurred.

## Room entry preflight and editable compact forms - 2026-11-06

Create validates room names with the protocol's existing canonical-name rules
before allocating a session. UI/admission share the extracted password rule
(128 UTF-8 bytes, valid Unicode, no ASCII control characters). Invalid drafts
remain editable and focus moves to the field. Join validates inside its existing
password popup, with room for wrapped feedback. Its input cap now exceeds the
admission limit so a long ASCII paste is rejected rather than silently becoming
a different, admissible password. Valid passwords are never trimmed.

Compact renders exposed squeezed/overlapping Create fields. Minimum-size layout
constraints on both details and responsive columns propagate content height to
the scroll area. Tests wait for full-height password/name fields and require
the password to fit its own card and the viewport at compact/normal sizes.

The full Release build, focused local HTTP fixture and five relevant UI/
admission/protocol/update checks passed. Coverage rejects invalid drafts without
sessions/HTTP admissions, accepts boundary Unicode names/passwords, and retries
inside the Join popup. Rendered feedback was inspected and both packaged
executables match. One earlier broad run exposed a keyboard proof clicking while
the button was still disabled after permission acknowledgement; the proof now
waits for UI readiness before clicking. Final parallel checks pass. This does
not address the previously recorded viewer-removal resume timeout. No physical
input, version bump or publication occurred.

## Fallback file-picker contrast correction - 2026-11-06

The owner identified the unreadable white file panes in the Save As screenshot.
The earlier export commit had left Qt's fallback view backgrounds unthemed
while inheriting pale text. Scoped QFileDialog styling now covers file views,
sidebar, selection, header, navigation buttons/arrows, scrollbar corners and
horizontal scrollbars. The standard native-dialog selection policy is unchanged.

The full Release build, focused export fixture and three relevant UI/update
checks passed. The fixture loads a folder and a JSON file, selects an actual
model row and requires dark bases/light text across fallback item views.
Populated renders were inspected after fixing both white panes and remaining
native arrow/scrollbar contrast. The portable archive matches both executables.
No native Windows picker interaction, version bump or publication occurred.

## Diagnostic report Save As - 2026-11-06

Details now opens an asynchronous platform Save As dialog instead of writing
directly to a preset location. Default JSON suggestions use role/UTC time in
Documents; an explicit diagnostic path remains the initial suggestion. Each
session remembers its selected destination, keeps overwrite confirmation and
disables duplicate exports while the picker is open. Cancellation preserves
feedback, and session Close rejects open pickers before draining. Reports are
snapshotted on selection using the existing redaction/atomic writer. Open log
folder remains independent; ended-session copy no longer promises exports there.

The full Release build, focused host/viewer export fixture and four relevant
UI/report/update checks passed. Tests cover suggestions/configured paths, chosen
locations, JSON suffix, cancel/reopen, duplicate actions, dismissal on Close and
a selected-path write-failure race with retry. Real local-service video frames
continue while the picker is open. Compact/normal result renders and the Qt
fallback picker were inspected; the portable archive matches both executables.
Automation forces nonnative dialogs and does not verify Windows native picker
interaction. No version bump or publication occurred.

## Room directory availability and capacity - 2026-11-06

Home/Join now retain the directory's viewer limit and typed room availability.
Rows show occupied/total places, Full for capacity, and Wait… for a reconnecting
host or unavailable list. Disabled actions provide the specific reason in their
tooltip/accessibility description, use readable muted text and omit the action
icon. An authoritative available update restores Join and its icon in place.
Existing rows and action geometry remain stable; legacy rows without a limit
retain the count-only display.

The full Release build, focused directory/admission fixture and three relevant
UI/update checks passed. Coverage includes full/reconnecting/stale/restored
states, capacity changes, blocked clicks, row identity and both Home/Join
adapters. Compact/normal renders were inspected and the portable archive matches
both rebuilt executables. Directory states are synthetic snapshots; no host
lease expiry, WAN fault, version bump or publication occurred.

## Preserve actionable session failures - 2026-11-06

RoomSessionWindow previously overwrote detailed admission guidance with a generic
failure on every failed status push. It now renders the typed cause through the
shared error helpers. Once the room has been active, fatal media/transport errors
describe that failure and give host Stop/start or viewer Leave/rejoin guidance.
New terminal failures replace stale action errors rather than preserving them.

An actual host-capacity window regression failed against the previous behavior.
The final Release build, focused admission fixture and five relevant UI/mutation/
report/update checks passed. Actual host/viewer windows retain capacity guidance
through status refresh; synthetic active-room failure pushes cover recovery
wording and stale-error replacement. Compact/normal failure renders were
inspected and the portable archive matches both rebuilt executables. No physical
device/WAN failure was injected, and no version bump or publication occurred.

## Responsive three-column source grid - 2026-11-06

The source picker now uses up to three columns when cells can remain at least
210 logical pixels wide, with two/one at narrower sizes. Normal picker geometry
fits six choices fully in view. Thumbnail aspect fitting and full-title tooltips
remain; scrollbar space is reserved using widget width to avoid wrap oscillation.
Resize updates cell geometry without rebuilding cards or losing selection.

The full Release build, focused source-grid fixture and three UI/update checks
passed. Tests inspect actual card rectangles for normal three-column/six-visible
and compact two-column layouts, selection identity and stable wrapping. Both
rendered sizes were inspected; the portable archive matches the rebuilt CLI/UI
executables. No version bump or publication was performed.

## Share-source title search - 2026-11-06

Create/live source pickers share an initially focused search field. Filtering
updates existing cards case-insensitively without restarting thumbnail capture;
visible selection stays stable and the draft source is retained through refresh.
Queries survive tab/refresh changes. An explicit tinted Clear search action
restores sources. The list centers no-match versus unavailable feedback and
provides an accessible description. Share rejects hidden/invalid selections and
has a disabled visual state. Enter from search moves to matching cards without
sharing or dismissing the picker; normal confirmation/focus return remains.

The full Release build and focused synthetic-source checks passed. Five relevant
UI/mutation/update checks passed overall; a parallel controller-resume timeout in
viewer removal passed on the isolated rerun. Final room UI/removal checks passed
sequentially. Coverage includes title/selection identity, icon updates, no matches,
tab/refresh retention, clear/Enter behavior and popup control bounds at compact/
normal sizes with live audio controls. Matched/empty/create/live renders were
inspected; the portable archive matches both rebuilt executables. Source-search
fixtures enumerate no desktop sources. No version bump or publication occurred.

## Cancellable room joining - 2026-11-06

Join now shows neutral progress, disables the primary action and reuses Paste
for Cancel. The invite stays read-only/copyable while pending; directory actions
cannot replace the in-flight target. Cancellation preserves the invite and waits
for the existing asynchronous session drain. Home cancels first and navigates
only after cleanup, avoiding a hidden join that later reopens the session.
Explicit joining/cancelling/returning-home states reject queued success and
password-error handling after cancellation. No automatic retry is introduced.

Action widths are measured after stylesheet parenting/polish, and feedback space
is reserved so pending/idle transitions do not move the invite or directory.
The full Release build, focused stalled-admission fixture and five relevant UI/
mutation/update checks passed. Coverage includes repeated actions, target lock,
cancel/Home drain, a queued success, preserved invites, no media startup, stable
geometry and recovery from admission failures. Compact/normal renders were
inspected; the portable archive matches both rebuilt executables. No physical
input, version bump or publication was performed.

## Room closure feedback - 2026-11-06

Authenticated room.closed reasons survive Stop as a typed optional closeReason;
reports serialize the bounded protocol vocabulary, with null for local exits.
The existing removedByHost flag remains compatible. After a viewer session
finishes cleanup, normal Home and standalone Join show a shared toast explaining
host departure, expiry or service shutdown. Local Leave and host Stop remain
quiet; removal retains its existing persistent Join explanation.

The full Release build and seven relevant room/UI/report/protocol/update checks
passed. A real local-service host Stop checks the retained reason, return Home,
released keep-awake state and visible notice. Voluntary viewer Leave keeps the
host active and creates no toast. Removal and all report reason values are
covered; expiry/service message policy is tested without waiting for real leases.
The rendered notice was inspected and the portable archive matches both rebuilt
executables. No physical input, version bump or publication was performed.

## Version 1.0.12 release preparation - 2026-11-06

The owner requested publication of 1.0.12 after the nine separately committed
audio, preview, moderation, video-state, placement and admission improvements.
CMake version, versioned release notes and in-app update notes now describe this
release. Use the maintained build/sign/verify/publish scripts and the existing
update identity. Merge before the final build and publication from clean main.

## Inline viewer removal action - 2026-11-06

Viewer removal is an icon-only user-minus SVG action beside mouse, keyboard and
controller controls. All four use the same 32px bounds and stay on one line.
The narrow sidebar required smaller buttons/tighter gaps to keep removal inside
the actual scroll viewport. Long nicknames elide, with full plain-text identity
in accessibility and an escaped tooltip. Pending removal stays icon-only and
disabled; the existing named confirmation and membership behavior are retained.

The full Release build, room UI, viewer-removal and UI self-test checks passed.
Row regressions verify every action stays within the scroll viewport, shares a
baseline and keeps its accessible name at compact/normal sizes. Both rendered
layouts were inspected; the portable archive matches the rebuilt CLI/UI binaries.
No physical input operation, version bump or publication was performed.

## Actionable room admission feedback - 2026-11-06

RoomSession preserves validated full, closed, not-found, rate-limited and
unconfirmed admission reasons as appended RoomError values. The Join form and
standalone sessions share RoomEntryError guidance. Hosting distinguishes service
capacity from a room's viewer limit. Invites remain editable after cleanup;
password retry remains specific to AdmissionDenied, with no automatic admissions.

The Release build and focused local HTTP fixture passed. The fixture checks all
five viewer failures, retry availability, preserved invites, no spurious password
prompt or media startup, and host capacity guidance. Compact/normal Join renders
were inspected. Seven relevant CTest checks passed in the final parallel run;
viewer removal timed out during controller resume, then passed in isolation.
That scenario also passed in the earlier three-test run. No physical device
operation, version bump or publication was performed.

## Window layout persistence - 2026-11-06

Normal Home startup opts into AppShellWindow placement persistence. Size, position
and maximized state use a bounded Qt geometry blob in ScreenShare/MainWindow.ini;
explicit profile/test paths use a separate .window.ini companion. Diagnostic
launches stay independent, and loopback tests without an explicit profile never
touch the user's settings. Move/resize/state events update memory only. The first
Close freezes/saves layout before async room drain; unshown startup never writes.
Fullscreen/minimized states retain the preceding windowed snapshot.

Do not restore a maximized blob directly onto the custom native chrome or call
Qt showMaximized for its startup restore. Native tests exposed a tiny normal
restore rectangle and fullscreen misclassification. Decode/clamp on an unshown
Qt helper, apply its normal screen/geometry, then use the existing title-bar
maximize path after first show. Native HWND checks retain a Qt fallback for the
offscreen backend. No manual geometry-blob parsing or native-coordinate storage.

The full Release build and five relevant UI/update/mutation checks passed.
WindowPlacementChecks runs with both Qt offscreen and native Windows; actual
title-bar maximize/restore and hit tests pass at 100% and 150%. Coverage includes
fullscreen from normal/maximized, minimized close, corruption, missing-monitor
simulation, deferred close and normal-app restart. The Home render was inspected;
the portable archive matches both rebuilt executables. No physical monitor
hot-unplug, version bump or publication was performed.

## Viewer notices for paused/unavailable video - 2026-11-05

Hosts publish video intent/capture condition through the existing bidirectional
telemetry channel. SHS v1 carries only connection ID, sequence and an enum: Live,
VideoPaused, SourceUnavailable, SourceMinimized or Recovering. Publication keeps
the latest state, bounds sends to four/second and heartbeats once/second. The
viewer inbox checks size/version/state, connection and increasing sequence,
bounds acceptance and withdraws observations after three seconds. Old hosts have
no observation; old viewers ignore these packets. No Worker change is needed.

Viewer status surfaces clear the old picture for non-live states. A Live notice
keeps Resuming video until a decoded frame reaches the UI. This is host intent,
not proof of remote presentation. Reports include remoteShare state/staleness/
receipt age. Before clearing a frame, RoomGamepadControl releases held input
under its old mapping and neutralizes controller polling through an independent
video-availability gate. Grants and the existing focus policy are preserved;
fresh accepted video lifts that gate, while Game mouse needs recapture.

The Release build and all 42 CTest checks passed. After the final input-gating
refinement, six relevant UI/input/report/runtime checks and the recording desktop
UI scenario passed again. Coverage includes pause with continued audio, delayed
UI consumption after Live, two-viewer close/replace/minimize/restore, protocol
replay/binding/expiry/bounds, neutral controller/resume under the same permission,
and mouse/key releases without revoking grants. Rendered paused/minimized/missing
source states were inspected at compact and normal sizes. The portable archive
matches both rebuilt executables. No physical input/listening/WAN qualification,
version bump or publication was performed.

The formerly combined UI run hit admission error 9 (RateLimited) at its last
source scenario. Viewer removal now has its own registered local-service test,
room-v2-viewer-removal, so admission counts do not depend on the minute boundary.
Production limits remain unchanged.

## Host viewer removal - 2026-11-05

Host viewer rows expose Remove viewer with the existing in-page confirmation
popup. Confirmation names the current member, uses plain text for nicknames,
focuses Cancel and handles departure while open. Removal uses the existing
peer.disconnect service command and authenticated peer ID, never the nickname.
No Worker deployment or protocol change is needed. It ends a membership, not a
ban; joining again creates a new peer identity.

RoomSession::RemoveViewer shares the bounded room-mutation lane, rejects viewers,
host self-removal and invalid/missing targets, and retains acknowledgement timeout
and no-retry semantics. Qt routes removal completion separately from profile/room
edits. Authoritative membership retirement releases input and media. The explicit
server kicked reason survives Stop as removedByHost and appears in reports.
Normal viewers return to Join with an explanation; standalone session windows
clear the video and retain the explanation until dismissed.

The Release build and all 41 CTest checks passed. A focused two-viewer test covers
duplicate nicknames, cancel/focus, recording controller release, rejected stale
input, unchanged surviving connection generation and continued video, departure
during confirmation, fresh admission and the browser explanation. Compact/normal
popup and return-page renders were inspected. The portable archive matches both
rebuilt executables. No physical controller/audio operation, remote-WAN check,
version bump or publication was performed.

## Host audio privacy before viewer arrival - 2026-11-05

Hosts can now mute or choose audio while the room is empty. AudioSwitchControl
commits inactive configuration under its attachment lock without constructing an
endpoint; the next recording uses it from startup. Active handovers retain their
first-PCM acknowledgement, bounded queue and cancellation behavior. Deferred
device failure stays silent and reports Failed, without reopening an old source.

The dashboard enables Mute before arrival, shows Audio muted and a matching icon,
and restores the exact prior source on Share audio. Explicit mute cancels a pending
automatic source edit so it cannot unexpectedly restore audio. The source picker
also saves audio choices without waiting for a viewer.

New PCM and UI regressions failed against the old behavior. The final offscreen
room UI run passed: first-viewer video continues with zero audible blocks and no
host capture endpoint opened, then unmute restores the selected process source.
The full Release build and the other 40 registered CTest checks passed. Muted
dashboard renders were inspected at compact and normal sizes. An additional
Windows UI run stopped earlier at the native-presentation startup assertion
(occluded surface, zero presented frames); it did not reach the audio scenario.
No physical audio listening, version bump or publication was performed.

## Initial audio-stall investigation - 2026-11-05

The custom ADM and capture producers used ordinary std::jthread scheduling,
omitting the MMCSS policy in the pinned WebRTC Windows audio device. Under a
750ms higher-priority task on the same CPU, synthetic playout's maximum callback
gap was 229ms. Registering thread-scoped Pro Audio/critical MMCSS reduced it to
10ms in the corrected test, which pins after registration and verifies affinity.
The first post-fix experiment lacked that affinity check; use only the corrected
comparison. This reproduces an initiating stall mechanism, not proof of the
specific remote report's trigger.

ADM capture/playout and device-capture producers now own their registration on
their audio threads and revert on exit. Registration failure remains nonfatal
and visible in reports. New phase/age and lock/callback/write maxima distinguish
in-progress device waits from callback stalls; completed slow operations emit
bounded, rate-limited audio-playout-stall events. The real loopback recovery test
checks the live blocked-write phase and retained event timing, then stopped state.
Audio-worker history writes use a nonblocking try-lock so report collection
cannot stall those workers; lifetime timing counters remain available if an
optional event is dropped.

The full Release build and all 41 CTest checks passed. After the final nonblocking
history refinement, the build and PCM/audio-sync/report checks passed again;
the corrected same-CPU scheduling experiment again measured a 10ms maximum gap.
Silent native WASAPI lifecycle checks also passed (50ms buffer, 10ms engine
period). No audible fidelity or original remote trigger is established.
`build/ScreenShare-audio-scheduling-preview.zip` matches both rebuilt executables;
SHA-256 is 8432b81e49e990cb76e5adfce1e2e0182d572e125ec79d28b8265dd8069c71eb.
This remains an unpublished 1.0.11 test build, including the prior recovery/UI
work; no release or version bump was performed.

## Performance and room-list review - 2026-11-05

Separate heavy diagnostic snapshots and GUI status copies from the short media/
control tick. The local four-viewer fixture counted 66 snapshots/second before
and ten after, with control advances unchanged. Command completions and final
stop snapshots still publish immediately. This is avoided work, not a measured
whole-application CPU or WAN latency win.

Create/host previews now fit within 640x360 on the GPU before BGRA readback;
source-picker thumbnails fit within 320x180. Host monitoring drops from about
30 FPS to five FPS. Preview pacing checks interruption rather than blocking
shutdown for an entire 200ms wait. Generated-window WGC checks preserve landscape,
portrait and small-window geometry: 1266x713 copied 3,650,560 bytes originally and
921,600 after fitting. Stream dimensions/rate remain independent.

Home/Join update access-badge styling and icons together while retaining row
identity/focus. Search distinguishes no matches from an empty, loading or stale
directory, offers Clear search and returns focus to the search input. UI
regressions cover these transitions and preserve disabled stale-room actions.
The Release build and all 41 CTest checks passed. After the final loading-state
and outlined Clear search refinements, the room UI, updater deferral and UI smoke
checks passed again. Native generated-window preview capture passed separately;
rendered Home search states were inspected. The portable archive matches both
rebuilt executables. This remains an unpublished test build with no version bump.

## Audio lag recovery investigation - 2026-11-05

Paired 1.0.11 reports showed low RTT, effectively no packet loss, fast hardware
codecs and bounded presentation handoff, but interval video buffering reached
about 1.5 seconds and audio buffering about 0.5 seconds. They do not contain PCM
worker timing, so the initiating stall cannot be identified conclusively.
The older static-sound reports contain no audio-device failure and cannot
establish signal fidelity or a distinct static-noise cause.

Real production room loopback with a stalled synthetic output reproduced sound
remaining roughly 600ms behind video after a 600ms stall. Bound NetEq to ten
packets and enable its fast acceleration. Let NetEq manage recovery: burst
draining missed callbacks distorted its jitter estimate and was discarded.
Preserve PCM pacer phase so wake-up overshoot and callback work do not accumulate
on the silent capture/discard clocks. Keep no-replay and cancellation behavior.
Reports now retain PCM block/drop/delay/callback-gap counters and transport
jitter-buffer target/minimum and inserted/removed audio sample counters.
The new silent room regression covers repeated 400/600/1200ms output stalls in
both presets; it does not qualify remote device drivers or every WAN condition.
The full Release build succeeded. All 40 registered checks pass after correcting
the new cadence fixture to use the high-resolution timer for its simulated work
and rerunning native-pcm-audio; ordinary Windows sleep rounded its 2ms wait up.
Stereo levels, room media/reconnect/UI and sync recovery passed in the full run.
The generated portable ZIP matches the rebuilt executables. No version bump or
publication was performed.

## Version 1.0.11 release preparation - 2026-10-15

Versioned notes and updater text prepare the shared source picker, selected-source
preview, closed-window room continuity, preview image ownership correction and
pending update-dialog layout improvements for 1.0.11. The owner explicitly requests
merging into main before release. Build, test, package and sign from that clean
merged commit, then publish through the maintained release scripts with the
existing update key. The broader crash investigation remains inconclusive.

## Follow-up crash audit - 2026-10-15

The owner identified Miyomu as the app shared during the friend's steady-sharing
crash. Reviewed capture/GPU ownership, encoder callbacks and fallback, audio
producer/endpoint lifetimes, WebView2 audio helper selection/restarts, automatic
peer recovery and room shutdown. No second confirmed product crash was found;
do not present the previous preview ownership fix as proof of the friend's cause.

Added an opt-in SCREENSHARE_SOURCE_SOAK_SECONDS interval to the existing room
source-switch scenario (0-40 seconds). A 30-second native run with two viewers
passed, kept hardware encoding active with zero software fallbacks, then closed
and replaced the generated shared window without changing viewer connections.
The 60-second four-viewer synthetic slow-consumer runner passed; the other three
viewers continued near 30 FPS and the slow viewer recovered. Source PCM,
application-audio-target and shared-audio-level checks also pass. These do not
reproduce Miyomu on the friend's machine or establish long-term driver/memory
stability. An initial native run exited successfully but its collector rejected
multiple JSON progress lines; metrics now appear in one result object and the
corrected native run passes. No production changes were added by this audit.

## Shared source chooser and closed-window recovery - 2026-10-15

Create room now reuses the host room's SourcePickerDialog with a larger panel,
responsive preview cards, full-title tooltips and explicit confirm/cancel. The
form retains the chosen source name and a bounded five-FPS live preview, paused
while the chooser is open or the form is hidden. SourcePreviewLabel paints both
create-room and host previews. Source thumbnails are captured only in the picker.

A switchable capture source now releases a closed window on its capture worker
and reports SourceUnavailable while waiting for a replacement. Capture delivery,
room identity and existing viewer connections survive. There is no automatic
fallback to a display. Direct non-switchable source closure stays terminal.
The host shows Change source guidance and Details records the unavailable state.

Queued preview images now explicitly own their pixels. QImage::scaled can retain
external frame memory when input and output dimensions match; this was a possible
steady-sharing host crash path. The ownership regression covers that exact-size
case. The friend's reported 1.0.10 crash with two viewers was not reproduced and
must not be claimed conclusively fixed without crash evidence.

The full Release build and 39 CTest checks passed. Final room UI checks also pass
with live selected-source preview and chooser thumbnails, inspected at several
window sizes. NativeCaptureTests --live passes closed-window replacement. The
focused native source-switch room test closes an actual generated window with two
viewers, rejects a bad replacement and resumes both without changing connection
generations. A wider native UI screenshot run stopped at an unrelated fullscreen
toast visibility assertion; it is not claimed passing. No physical input/audio,
remote-WAN crash reproduction, version bump or publication was performed.

## Remote 3D game input — 2026-10-10

Grant preparation no longer activates the shared game; the input worker validates
identity and grants while it is unfocused/minimized. The host selects the game
when ready. Viewer Game mouse is opt-in: the first video click consumes its own
press/release and registers foreground raw mouse input, hides/confines the cursor,
and sends relative motion/buttons/wheel with the displayed source generation.
Capture releases on blur, grant removal, mode change, hidden/cleared video and
destruction. Registration restores the prior process mouse registration. Native
capture failures use a toast. Relative input bypasses desktop physical-mouse
cooldown because game cursor warps can trigger it; host focus/revoke still stop it.

SIN v2 adds kinds 10/11/12 without changing existing payloads. Relative movement
has a separate bounded state slot and accumulates counts; buttons/wheel stay on
the reliable lane. Window mouse grants accept verified identity without exact
desktop bounds for relative control; absolute events still require exact mapping.
Both endpoints need the updated app; no Worker change is needed. Escape and Tab
reach the controlled video/game. Ctrl+Alt+Shift+F toggles fullscreen;
Ctrl+Alt+Shift+Q releases viewer control. Global host panic remains unchanged.
The viewer Controls panel always displays both shortcuts and Escape behavior.
Entering fullscreen and successfully capturing the mouse show a 6.5-second,
non-blocking reminder so the exit/release controls remain discoverable.

The complete native build and all 39 CTest regressions pass, alongside recording
desktop UI and native mouse registration/confinement scenarios. A local probe
using the production capture source and desktop sink captured the actual Stellar
Blade window at 2560x1440 and granted Mouse+Keyboard without changing focus;
it injected no input. A separate local keyboard/menu check succeeded. No remote
Stellar Blade gameplay or hardware mouse sampling is claimed. SendInput remains
subject to Windows integrity restrictions and game compatibility.
An isolated portable game-input preview (still compiled as 1.0.9) includes the
current workspace, runtime dependencies, notices and a friend-test checklist.
Extracted CLI help and UI self-test pass. Both endpoints must run this preview;
published 1.0.9 lacks these changes. No stable version bump or publication occurred.

Friend-test follow-up: the first preview still rejected keyboard grants when
starting with window capture because NativeRoomRuntime configured only Mouse and
Gamepad at construction. Display capture or a source switch configured all three.
Startup now enables all three and delegates target validity/focus to the sink.
The window keyboard UI fixture now constructs the production runtime instead of
a bare input Service, adds Keyboard to an existing Mouse grant, and delivers a
recorded key. It reproduces the first preview failure before the runtime fix.
The native window room proof also requests/grants Mouse+Keyboard rather than
asserting that keyboard must be rejected. Portable preview revision 2 includes
this correction; compiled app version remains 1.0.9.

Preview revision 3 follow-up: user initially missed Game mouse, then reported a
cursor at the edges after enabling it. Keep the existing control location and
label, but use the app's SVG toggle style (user explicitly requested a toggle,
with no extra footer button). Show ready/captured guidance. Confinement now uses
one native pixel at the video centre, avoiding borders/rounded corners and DPI
rounding. Native click fixture verifies confinement survives release and the
capture toast; input is recorded only. Verify OS confinement and Qt cursor state
rather than relying on pointer overlays in automation screenshots.

The friend-test host report ended with source capture
failure before runtime shutdown; no viewer departure appears in its session
events. Exact exception was not preserved. A GPU completion deadline was one
generic source-failure path: type it explicitly, skip isolated unfinished frames,
and use existing bounded device recovery after three consecutive skips. Report
the fixed vocabulary event capture-gpu-frame-skipped. The CaptureSession proof
checks skips/recovery and 100 restarts. Do not claim this proves the user's exact
leave/rejoin failure fixed. Failed/stopped room rows now say the room ended, rather
than telling viewers to rejoin it. Stable publication is still pending.
Revision 3 validation: full Release build, 38/39 CTest checks passed with one
desktop-input timeout; isolated retry passed in 0.07 s. CaptureSession deadline
proof passed including 100 restarts; native game mouse test passed. Native UI
fixture renders the toggle and verifies one-pixel clipping/blank Qt cursor after
real click/release/toast. Extracted v3 UI self-test and CLI help pass. ZIP SHA256:
6cf4b8dfd5f57316dc836c4d53d868c6a2056bc6f0345a68135b7140e7dd763e.

## Window keyboard grants without mouse geometry — 2026-10-09

The owner confirmed the game is launched normally. Capture previously published
no input generation when WGC source pixels differed from DWM frame bounds or
those bounds were unavailable, and the Windows device also required exact bounds
for keyboard-only grants. Capture now publishes a verified, live source identity
with the captured dimensions and an explicit mouseMapped flag. Mouse grants
require that flag; keyboard-only Windows device health checks identity/visibility
without requiring DWM geometry. Existing generation, focus, minimized pause and
held-input cleanup rules remain. Real source changes still require fresh grants.
Source diagnostics record marker setup failures and transitions among unavailable,
keyboard-only and mapped targets through the existing safe media event history.

A generated HWND regression fails the prior keyboard-grant check and passes the
correction. Mismatched/missing bounds admit keyboard and reject mouse/combined
grants; hidden windows, removed markers and changed process IDs are rejected.
Input service, video event, desktop, report and recording room UI tests pass.
Active-desktop NativeCaptureTests --switch passes a borderless source keyboard
grant, minimized liveness and six replacements. No physical key delivery or
Silksong validation is claimed. This follow-up and the arrow fix prepare 1.0.7
through the maintained build/sign/publish workflow with the existing update key.

## Qt extended-key capture correction — 2026-10-09

The owner reports arrows still fail with full-display keyboard grants in 1.0.6.
The pinned Qt 6.10.3 Windows key mapper encodes extended scans as E0xx, whereas
the input protocol accepts only a low scan byte plus bit 8 (maximum 0x1ff).
VideoFrameWidget forwarded the native value unchanged, so MappedInput rejected
arrows before transport. The 1.0.6 injection fix alone could not address this.
The viewer now converts Qt E0xx into the existing protocol format for presses
and releases, preserving unprefixed keypad scans. A regression through widget
and native video surface events, mapping, wire encode/decode and Windows payload
construction fails on 1.0.6 and passes with this correction; video-frame-input,
desktop-input and input-service pass. No physical key or Silksong test is claimed.
This follow-up is included in 1.0.7 preparation. The window-grant investigation is
recorded above; the arrow conversion itself does not affect grant availability.

## Version 1.0.6 keyboard/source-switch fixes — 2026-10-09

Windows keyboard injection now preserves the native scan byte and E0 flag for
presses and releases, including cleanup of held keys. Scan codes drive ordinary
keys; Pause retains its virtual-key E1 handling. Previously arrows/right modifiers
lost their extended identity. Minimized windows with previously captured geometry
retain input identity/permission while capture refreshes liveness; desktop health
accepts this pause and foreground checks suppress injection. Restore still requires
valid captured geometry; closed/hidden/replaced sources remain invalid.

Overlapping WGC sources now share ownership of their thread's capture dispatcher.
Retiring the original source previously shut down the replacement's borrowed queue.
Local media failures now send peer.leave after admission instead of retaining a
ghost viewer/full room or reconnecting host for the service lease. Transport
recovery retains its existing lease behavior. Host viewer rows expose asynchronous
backend grant refusal instead of silently returning to Watching.

Dispatcher and media-failure cleanup regressions fail against the prior code and
pass with fixes; key payload/service/presentation and recording desktop UI checks
pass. The production Windows keyboard sink also accepts a minimized test window
and rejects it after its capture identity is removed, without injecting input.
The release checks additionally pass NativeCaptureTests --switch on an active
desktop (six WGC source switches, continued frames/geometry and minimized grants)
and the native Windows room UI source-switch scenario. The full 37-test application
suite, update signing/tamper checks and packaged UI self-test pass. Full Silksong
and two-computer keyboard input remain unverified. Version and updater notes
prepare 1.0.6; the application release uses the existing signed update identity.
No Worker change is required.

## Shared-source window audio — 2026-10-09

Room UI/CLI hosts now default to shared-source audio. The Windows runtime follows
the committed capture selection: process-tree loopback for a window, system output
for a display. The audio worker replaces endpoints and discards a read spanning a
source change. Invalid windows and unsupported process capture produce silence;
explicit system, microphone, process and no-audio modes remain available. Muting
retains the shared-source mode so unmute resolves the current source. Windows
process isolation may include other windows/tabs of the same app and requires
build 20348+. Coverage uses synthetic PCM and room UI/CLI scenarios; audible
two-app isolation has not been physically qualified.

## Version 1.0.4 window keyboard control — 2026-10-03

Window sharing now supports keyboard grants through direct host clicks and
request acceptance. Desktop input separates temporary focus loss from invalid
capture identity/geometry: unfocused events are consumed without injection,
periodic health polling releases held input and permissions remain active.
Returning focus resumes new events without replaying discarded input. The Windows
injector also checks focus immediately before keyboard injection and ignores
unmatched window key releases. Source changes and invalid targets still revoke.
Input unit tests, the mapped desktop UI scenario and the normal room UI suite
passed with synthetic input. No physical two-PC keyboard qualification is claimed.
Versioned release notes and updater notes describe this behavior.

## Version 1.0.3 release preparation — 2026-10-01

Version 1.0.3 packages the room socket replacement fix and its regression coverage.
The updater manifest notes and versioned release notes describe reconnection,
restored controls/telemetry and isolation of unaffected viewers. Publication must
use the clean merged main commit and the existing build/publish scripts with the
pinned update signing identity. The v2 Worker deployment is separate from the
desktop release; its existing protocol also supports 1.0.2 clients.

## Room socket replacement recovery — 2026-10-01

Paired host/viewer reports showed video decoding continuing after viewer socket
reconnects while the host retained an old peer with closed SCTP channels. If a
replacement socket arrived before the old close callback, the service announced
only connected and the remote roster never retired that peer. V2Room attachment
now advances the socket generation and broadcasts reconnecting before connected
for an already attached member, using existing wire messages. Host replacement
uses the corresponding host.status transitions. Initial attachment is unchanged.
The fix requires deploying the v2 Worker; no application update is required.

The Worker replacement test and native delayed-close regression failed before
the fix and pass after it. The native test covers two viewer reconnects, one host
reconnect, peer replacement, fresh telemetry and real encrypted control delivery;
other viewers retain their peers during viewer recovery. All 202 Worker tests,
typecheck and existing native media/departure/service checks passed. Tests use
synthetic media/input and a loopback-only generated fault route. No deployment
was performed during implementation.

## Session details and log folder — 2026-09-30

The owner requested clearer host/viewer Details. SessionDetailsWidget renders
existing immutable status snapshots with an Overview for Connection, Video and
Audio. The host selector retains peer identity across updates/renames and stays
visible while metrics scroll. Advanced retains the complete diagnostic table and
text, with selection synchronized to Overview. Missing and stale measurements
are distinct; encoder output and viewer-reported decode are labelled separately.
Viewer frame size/rate uses the existing decoded-frame/FPS path. No extra polling
or service requests. The encoder target in Advanced correctly converts bps to Mbps.

Open log folder replaces Show in Explorer, is always enabled and opens the
configured report's parent directory (or the app reports directory for defaults).
OpenUiLogFolder creates the directory if needed before passing a local-file URL
to the native opener. It works before save, after deletion and after failed save.
Save report remains separate; log actions stay outside the scrolling tab pages.

## Room input request flow — 2026-09-30

Viewer controls start off and request immediately when clicked. Icon-only tiles
use amber for requested and teal for granted, with tooltips/accessibility names.
Keep them focusable while pending/granted: disabling a clicked tile moves Qt
focus to the next control and misleadingly highlights Keyboard after Mouse.
Duplicate clicks do not resend requests. Host requests gather per viewer, but
each amber icon's Allow/Deny menu applies only to that control. Pulses honor
Windows animation settings. Viewer state uses fill colors without selection
borders; focus indication is limited to keyboard navigation. Allow adds the
selected input to existing grants. RequestDenied (kind 9, one capability byte)
clears only the denied requests at the current permission epoch, on the reliable
lane with a newer sequence, without releasing held input. Pending denial packets
survive a following grant. Existing wire kinds/payloads remain unchanged; older
viewers ignore the new denial message. Same-epoch Permission messages cannot
alter grants. Release cancels ungranted requests. Focus changes pause input
without withdrawing permission.

## Backlog scope and retired laptop test target — 2026-09-30

The owner removed validation/qualification tasks from agents/todo.md, including
fresh-machine installation, virtual-device cleanup/hardware qualification and
field/resource/service validation. Do not recreate that validation backlog or
use the laptop for tests. Investigate the reported hardware decoder Section-handle
growth, encoder output deadlines and capture cleanup through code instead.
Historical failures in docs/known-limitations.md remain evidence, not completed
fixes. Routine checks for actual code changes remain separate from the removed
qualification campaign.

## Viewer departure signaling isolation — 2026-09-30

Late SDP/ICE for a disconnected/removed viewer used to close the sender's Worker
WebSocket (`target_unavailable`), disrupting every peer when the sender was the
host. Discard unavailable destinations and retired connection IDs without a
socket close; malformed/forbidden/rate-limited signaling retains its checks.
RoomSocket returns TargetUnavailable for cached absent/reconnecting destinations;
RoomSessionCoordinator treats that as peer cancellation instead of transport loss.
The workerd departure regression fails against the original Worker and passes
after the fix. Native `room-peer-departure` checks the actual network/coordinator
queue and healthy-peer delivery. Changes require the rebuilt client and v2 Worker
deployment to reach live users; editing/testing locally does not deploy them.

## Local profile defaults — 2026-09-17

RoomProfile persists normalized nickname plus allowlisted stream/v1 and playback/v1
JSON. Shared StreamPreferencesJson/ParseStreamPreferences are also the CLI config
parser/serializer boundary, so profile loads cannot bypass validation. Corrupt/unknown
fields fall back per group; invalid values never overwrite defaults. Failed QSettings
writes restore the prior in-memory key. New/invalid nickname profiles get a stable
persisted Guest-xxxxxxxx from the system RNG; existing valid names are preserved.
Browser-created RoomSessionWindow borrows the browser's profile (browser outlives it)
and offers explicit save-for-new-session buttons. Saving a draft never applies media
settings; Apply remains separate. New browser sessions load stream and playback
volume/mute; standalone config UI/CLI do not load/write profile defaults. No room
credentials, URLs, source handles, process IDs or device IDs are stored. Headless
tests use temporary profiles and actual reopen/join/runtime assertions.

## Reconciled checks and production regression runner — 2026-09-17

DETAIL-CHECKS.md has been reconciled beyond its latest-update blocks: old facade,
peer, stream-settings, profile/link and opt-in integration entries now match the
implemented code/evidence. Mixed requirements are split; physical/device/resource,
gaming input, default-shell, cost and external-latency gates remain unchecked.
Do not treat historical continuation notes as instructions to rebuild completed work.

scripts/test-room-regression.py runs app-build VideoFrameInputTests, RoomServiceTests,
RoomCliTests, RoomUiTests and delayed-ack UI scenario as one silent matrix. --repeat
restarts full rounds; --desktop adds generated-window UI/CLI GPU tests (no physical
input/audio). Requires Python 3.11+, Node and Worker dev dependencies. The Windows
bootstrap is gated until assigned to a kill-on-close job; all descendants are cleaned
even after successful parent exit. Fail fast, 90s outer watchdog, bounded retained
logs, no overwritten evidence, hashes/fixture metrics in result.json. It supplements
the lower-level test-headless-media.py; no separate native processes, impairment,
gaming input or continuous-soak claim. Unit tests cover actual child-tree cleanup.

## Shared UI/CLI presentation — 2026-09-17

FramePresentationBackend.h/.cpp and PresentationRecovery.h now live in backend/render;
the frontend-only header and media/webrtc policy path are removed. UI worker and
CLI preview both own a FramePresentationSession on their existing owner thread.
It centralizes factory/error handling, three-rebuild/250ms policy and terminal stats.
FrameView is borrowed synchronously; the caller owns pixels. No new copies/queues.
ReceiverPreviewWindow no longer owns duplicate D3D shaders/textures/swap-chain code.
Keep its window/keyboard controls, source sizing and legacy overload until cutover.
CLI reports presentation-status on errors and errors/recoveries/terminal at exit;
terminal renderer failure does not stop audio/membership. A terminal title cannot
be overwritten by normal status updates; unchanged titles avoid Win32 calls.
Preview closure releases resources and sets its local flag instead of posting
thread-wide WM_QUIT. Win32 callback exceptions are contained. Tests cover real
GPU rebuild/resize loss, fullscreen/scaling/minimize, malformed and legacy frames,
independent preview closure and owner/queue lifetime. No physical input/audio.
UI copy-link test now waits for actual button enablement, not merely room ID.

## UI presentation recovery — 2026-09-16

VideoFrameWidget's existing worker now owns a FramePresentationBackend (factory
injection for headless testing), uses PresentationRecovery for attach/resize/present,
and stops renderer calls after three rebuilds/fourth failure or nonrecoverable error.
Backoff drops frames for 250 ms; successes do not refresh the budget. clearFrame
explicitly resets the budget, while resize/configuration changes do not. V2 UI
shows a terminal rejoin instruction without stopping audio or room controls.
Tests include actual worker ownership, 1,000-frame pressure, backoff/exhaustion,
clear/reuse and actual GPU resource recreation after injected errors. Physical
device removal and hung driver calls remain open. Keep all audio tests silent.

## Retained NV12 presentation — 2026-09-16

backend/render/Nv12VideoFrame.h owns either a legacy moved vector or an immutable
aliased pixel owner/span. SessionEvent::VideoFrame is a compatibility alias; the v2
Qt/CLI handoff uses the portable frame directly. LatestRoomVideoFrame::Take retains
packed NV12 (no I420 round trip/no extra pixel copy), explicitly repacks padded NV12
and converts other formats. Statistics distinguish delivered/retained/converted/
repacked; Stop clears pending and rejects late callbacks. Qt preserves final stats.
V2 UI/CLI opt into DXGI queue limit1 and nonblocking present; busy/occluded frames
are drops, only successful presents count. Legacy entry points keep their defaults.
CLI emits final presentation JSON counters. Native decoder remains CPU NV12 and
presentation still uploads: hardware decode/GPU zero-copy acceptance remains open.
Tests prove buffer pointer identity/lifetime, packing/fallback and live fast paths;
Windows tests measure actual queue configuration. Audio stays synthetic/silent.

## Viewer-local playback settings — 2026-09-16

RoomSession.UpdatePlayback(PlaybackSelection) is viewer-only in NativeRoomRuntime,
one pending command, cancelled on Stop, with status.playback independent revision.
ControlledPcmPlayout runs on the existing ADM worker: no extra queue/clock/thread.
Volume/mute retain the endpoint; device changes construct/start/write a candidate
before committing. Failure retains previous healthy output/settings. Synchronous
device initialization can pause local sound and cannot be forcibly interrupted.
Native writes honor stop, and endpoint construction/destruction remain on worker.
PlaybackControl persists accepted selections across ADM restarts. Windows binding
never falls back to physical output when synthetic endpoints are injected.
Viewer UI and CLI playbackChanges share the public operation. Startup audio supports
playbackVolume/playbackMuted as well as playbackDeviceId. Timed changes are complete
selections (defaults: default device, 100%, unmuted), max64, ordered, viewer-only.
Tests remain silent; physical device loss/recovery and latency gates remain open.

## Live shared-audio switching — 2026-09-16

RoomSession.SwitchAudioSource(AudioSelection) is host-only in NativeRoomRuntime,
one pending operation, independently revisioned and cancelled by Stop. Requires
an active recording endpoint; no automatic retry. AudioSwitchControl retains the
successful factory/selection across ADM recording incarnations. SwitchablePcmCapture
owns current/candidate capture workers: first PCM commits, failure/5s timeout keeps
old audio, endpoint construction/destruction remain on worker. One 10ms handoff
block + WASAPI 20ms packet bound preserves 30ms application capture buffering.
Process activation accepts stop tokens; completion handler owns its event.
UI system/microphone/process selection and strict timed audioChanges (max64) share
the public backend. CLI emits audio/audio-ended and audioRevision, aborts on failed
scripted changes; UI preserves session. Physical audio is never used by routine
tests. Synthetic overrides cannot silently fall back to physical capture.
Tests cover decoder-observed silence/resume on four viewers and actual widgets,
unchanged room/settings revisions, failure, timeout, Busy, cancellation and restart.
Live playback-device selection, physical unplug/recovery and external latency
remain open. Default AppShell remains legacy; milestone 2 is not complete.

## Live capture switching — 2026-09-16

RoomSession.SwitchCaptureSource(CaptureSelection) is bounded to one pending operation,
host-only in the native runtime, and cancellable on Stop. Snapshot.capture exposes
independent selection/revision. SwitchableCaptureSource wraps the existing owner:
candidate creation/poll/destruction stay on capture thread, old source is polled
until first valid new frame, failure/closed/5s timeout retain old healthy source.
No room/peer rebuild or extra service requests. UI refresh/select/share controls
and config captureChanges (max64 ordered atMs + display/window + optional fps)
use shared backend. CLI fails scripted runs on failed changes; UI keeps running.
Switches can incur device readback/copies with the retained Windows encoder device;
zero-copy/hardware-only and live audio-device switching are not complete.
An end-to-end larger-source test exposed decoder rejection after startup at smaller
size. Pinned WebRTC sets max_render_resolution from its initial frame; decoder now
accepts bounded keyframe-declared changes, keeps global 4096 allocation bound and
validates decoded output. Synthetic/Windows source switching and oversized-declaration
tests cover this. See CHECKPOINT-B for current evidence; default UI remains legacy.

## Shared upload allocation and transport diagnostics — 2026-09-16

StreamPreferences now has optional aggregateUploadLimitBps; shared config key
stream.aggregateUploadBps (160000..1000000000). AllocateViewerVideo reserves 20%
plus 128kbps audio per viewer, splits video equally, then takes the individual cap.
Negotiating peers reserve shares too. Membership changes bump the runtime settings
revision only while a budget is enabled. Under 1kbps share deactivates RTP video;
audio stays active and budget recovery reactivates video without rebuilding peers.
RTP rejection preserves previous sender settings and reports rejection, not atomic
budget application. This is not an interface shaper and tiny budgets may not cover
audio alone. Existing fixed resolution/FPS and no-min-bitrate behavior remain.
Status/CLI report allocated and applied video caps plus optional transportSendBps.
TransportSendRate uses a per-generation mailbox, one asynchronous native stats
request per second (one outstanding), compatible counter deltas, and 3s freshness.
Late callbacks retain only the mailbox; UI aggregate requires all fresh samples.
Measured WebRTC transport bytes exclude IP/interface overhead. No rate feedback is
fed into allocation and no extra room-service requests are added. Tests cover four
viewers, departure/rejoin, video pause/audio continuation/resume, CLI reporting,
1..63-viewer arithmetic and stats reset/replacement. See CHECKPOINT-B evidence.

## Room links and acknowledgement fault coverage — 2026-09-16

Shared RoomLink.h accepts raw IDs or strict screenshare://room/v2/ID references.
Browser copy/paste and CLI configuration use the same parsing. No link can change
the configured origin or carry secrets/query/fragment/encoded IDs. No OS protocol
handler was registered. Clipboard tests are offscreen-only, never real Windows.
The new room-v2-mutation-ack-recovery CTest uses the native Worker runner's explicit
mutation-ack-delay mode: test-only subclass delays ack 1 by 12s and ack 2 by 4s,
while production commits/state pushes continue. Real 10s timeout, Unconfirmed,
no retry, late reply isolation and continued frames pass. This closes the earlier
missing-ack fixture gap. Suites 19/19 Debug+Release; Windows browser/link and
delayed-ack capture variants pass. See latest CHECKPOINT-B evidence.
User asked whether the new windows replace the UI: explained these are opt-in
backend integration screens using existing style/video widget. Normal AppShell is
unchanged; default adoption remains milestone 2, visual redesign remains phase 6.

## Live room/profile mutations — 2026-09-16

RoomSession exposes RoomPolicy/RoomMember, authoritative room revision and bounded
UpdateNickname/UpdateRoomPolicy futures. Shared wire validation normalizes names;
invalid UTF-8 and oversized/nonrepresentable revisions fail locally. One mutation
in flight; host policy authorization also checked server-side. Match acknowledgements
by unique request ID; no retries. Disconnect/stop/10s deadline => Unconfirmed, because
the server may have committed. Snapshot state is independent of acknowledgement.
QtRoomSession polls futures locally; RoomSessionWindow provides live edits, plain
member roster, independent nickname/policy drafts and explicit conflict/reload flow.
Session nickname does not change browser persistence. Tests extend the real Worker
UI scenario and deterministically hold signaling to check Busy and stop ordering.
No audible tests or physical input. Milestone 2/default cutover remain open.

## Sequencing preference — later UI redesign

User requested a broader UI appearance/feel/usability refactor only after all
current refactor milestones are finished. It is recorded as TODO milestone 6.
Continue necessary UI/backend integration now as already planned; do not expand
the current scope into visual redesign or postpone current integration for it.

## Pushed directory and browser admission — 2026-09-16

ScreenShareUi --room-v2-browser HTTPS_ORIGIN provides opt-in create/join without
a config file, display/window choice, default system/microphone audio, public/
unlisted visibility, optional password and saved nickname. RoomBrowserWindow
opens a RoomSessionWindow and stops its directory while hidden; closing a drained
session returns to the browser and a fresh subscription. Default shell unchanged.
Backend RoomDirectory owns RoomNetwork, drains a bounded local queue, filters
handles/generations, uses existing revision validation and caps reconnects at
three per rolling minute. No room-list HTTP polling or admission preflight.
RoomProfile reuses wire normalization for nickname-only QSettings persistence;
invalid stored nicknames fall back to Guest. Passwords/room IDs/tokens/origins are
not saved; the password field clears after launch. Nicknames apply to subsequent
admissions; live profile/policy mutation and room links are still outstanding.
RoomUiTests now exercise browser push/count/removal, password failure/recovery,
playback, persistence, hidden stop and rapid hide/show alongside earlier session
tests. Tests remain silent; no physical input. See ROOM-UI.md and CHECKPOINT-B.
Evidence: app 18/18 Debug + Release, CLI-only Release 12/12; final focused Debug
UI check 3.86 s. Final Windows browser/session WGC+renderer runs pass Release
4.32 s and Debug 4.56 s outside sandbox; exact artifacts in CHECKPOINT-B.
Logs build/webrtc/room-browser-app-{debug,release}.log and room-browser-cli-release.log.

## Opt-in Qt v2 session window — 2026-09-16

ScreenShareUi --room-v2 CONFIG.json uses RoomSessionWindow/QtRoomSession and the
existing video widget/style. Shared config parsing was extracted from RoomCli to
frontend/shared/RoomSessionConfig (ScreenShareRoomFrontend target); no duplicate
CLI/UI parser remains. QtRoomSession is UI-thread-owned, polls local status only,
coalesces edits into one in-flight + latest pending value, delivers latest NV12
frames and asynchronously drains before finished/close. It supports scheduled
changes, finite duration and reuse after stop. Normal destruction is after drain;
destructor remains a synchronous fallback. Configured audio devices are real in
production; tests inject silent synthetic PCM. Remote control is unavailable and
no input handler/grant is installed. Normal room browser/create/join, saved
profiles, directory integration, source switching and input remain outstanding.
Read refactor/ROOM-UI.md. New offscreen room-v2-qt-ui test uses real widgets against
the isolated Worker; explicit RoomUiWindowsTests adds WGC and actual presentation.
Do not claim default cutover or latency/resource/NAT acceptance from these tests.
Final suites pass app 18/18 Debug + Release and CLI-only Release 12/12, logs
build/webrtc/room-ui-{app-debug,app-release,cli-release}.log. Final Windows UI proof
passes outside sandbox in 2.72 s, 20 original + 20 changed frames with actual
presentation and silent audio; artifact recorded in CHECKPOINT-B. UI proof covers
invalid edits, 101 valid edits coalesced to one revision, close/drain, a held native
barrier with Qt heartbeat progress, and owner reuse. Next: normal create/join and
directory/profile integration, authenticated input/consent, source/budget work.

## Opt-in v2 CLI integration — 2026-09-16

ScreenShare --room-v2 CONFIG.json now uses frontend/cli/RoomCli (strict parser,
shared session controller, Windows factory) for host/watch, capture/audio choice,
scheduled live preferences, NDJSON status and graceful Ctrl+C/window-close/timed
stop. HTTPS only; no CLI plaintext bypass. Diagnostic loopback/factory injection
are available only through the internal test API. LatestRoomVideoFrame retains
one decoded frame; NV12 conversion/presentation runs on the owner thread. Real
CLI audio is normal WASAPI; tests inject silent PCM endpoints. No physical input.
Read refactor/ROOM-CLI.md for config/schema, tests and limitations. Existing UI and
other CLI commands remain legacy. Next integration work is UI adoption, input,
directory/profile, source switching, aggregate bandwidth and zero-copy presentation.
Do not mark milestone 2 or latency/resource/NAT acceptance complete.
Final suites: app 17/17 Debug + Release, CLI-only Release 12/12. CLI deploys Qt
networking/TLS independently of tests/UI; fresh-directory deployment test passes.
Windows CLI controller/capture/D3D preview proof passes outside sandbox, silent
synthetic audio: 53 original + 63 reduced frames in 6.78 s; artifact path recorded
in CHECKPOINT-B. Full logs build/webrtc/room-cli-{app-release,app-debug,only-release}.log.

## Routine tests must be silent

User explicitly requested no audible sine during tests. Omit `-AudioDevice` from
routine proof runs; the runner resets that cached CMake option OFF. Device tests
now additionally require `-AllowAudibleTests`; do not opt in without the user's
request. Synthetic PCM/Opus evidence still runs without speaker playback. Do not
change the user's system volume. Generated-window public-session tests also use
synthetic audio.

## Current integration evidence — 2026-09-16

Latest increment: public RoomSession::UpdateStreamPreferences is bounded to one
queued command, validates input and returns accepted revision; RoomStatus.stream
has requested preferences and per-peer applied/source-observed revisions,
dimensions and rejection. NativeRoomRuntime applies live revisions once per peer,
preserves prior settings on live sender rejection and gives new joins latest
preferences. Public four-viewer proof covers reduced decoded resolution, restore,
restart/rejoin, invalid/viewer/stopped commands. Silent media suites 31/31 Debug
and Release, app 14/14; Windows generated-window Release proof passes in 9.96 s.
Final Release public proof after stopped-status cleanup passes in 7.28 s. Evidence in
CHECKPOINT-B. UI/CLI adoption, capture reconfiguration and aggregate budget remain.

Milestone 1 is now complete for local integration; move to milestone 2 UI/CLI,
live settings and presentation adoption. NativeRoomRuntime replaces the diagnostic
runtime and uses HostPeerRegistry/RoomManagedPeer for asynchronous capture cleanup,
budgeted host restart and viewer restart requests. Native failures flow back to
RoomMediaSession without retry storms; individual capture delivery failures retire
only their peer. WindowsRoomRuntimeFactory uses WGC/WASAPI, first-device gating for
MF hardware setup and existing retirement fallback. Initial source preferences
apply before first delivery; sender settings apply after negotiation.
ScreenShareMediaAdapters removes proof-only adapter compilation; app link smoke
verifies the entire Windows binding is linkable without opening devices.
Full suites: media33/33 Debug+Release, app14/14, CLI9/9. Final public-runtime tests
include restart/rejoin, held stop, cancellation, startup/delivery failure. Generated
WGC-window tests pass Debug+Release outside the sandbox (inside it CreateForWindow
fails with missing-service error). No physical input; audio synthetic. This does
not establish hardware-only encoding, remote latency/NAT, resource or cost gates.

Public api/RoomSession.h now owns one v2 session incarnation: Qt network loop,
signaling executor, admission, coordinator, RoomMediaSession and injected runtime.
Start resolves at authenticated snapshot; Stop coalesces and awaits network/media
drain before signaling-side runtime destruction. Normal leave waits at most 1 s
for acknowledgement, then closes transport. Snapshot/status excludes credentials.
Transport recovery is bounded to three reconnects/minute and 35 s without a new
snapshot. There is no automatic retry of ambiguous admission. PublicRoomSessionProof
uses separate native engines for host/four viewers and individually observes
H.264 frames/Opus, cancellation, TLS enforcement and held media-drain barrier.
Next: standard Windows RoomRuntimeFactory/source-settings/presentation composition
and UI/CLI adoption. The injected diagnostic runtime is not production default.

RoomMediaSession now composes authenticated snapshots/signals with RoomPeerRoster
for both host and viewer. It runs under RoomSessionCoordinator, bounds events to
256/512 KiB, retires on transport loss, filters generations and isolates rejected
peer negotiation. RoomPeerRoster has readiness-gated pending additions: wait for
HostPeerOwner snapshot removal AND native reference retirement before slot reuse.
Pending adds retry; failed adds do not retry on profile updates. RoomMediaProof
uses this shared routing instead of its manual packet/roster queue. Next is owned
admission/start/join/stop/public facade integration, not rebuilding routing.

MediaEngine now owns the native factory and network/worker threads on the
signaling executor. RoomMediaProof uses its independent peer construction, host
track attachment and channel policy; audio/codec implementations are injected.
Destroy all peer/track references before engine teardown, on signaling, then stop
the executor. SSL stays application-owned. MediaPeer owns each native peer's ICE
lifecycle, negotiation, video sink and bounded validated channels. RoomMediaProof
uses it; its derived observer only collects evidence. Close derived sinks before
their members are destroyed. Outer session composition still needs facade adoption;
do not mark milestone 1 complete.

RoomSessionCoordinator now owns automatic room-to-signaling dispatch and bounded
send-completion handling; RoomSignalCodec is shared production wire conversion.
RoomMediaProof's wait loop only observes state. ICE restart completes during a
five-second caller pause. Normal facade adoption and runtime composition ownership
remain open; shared target is ScreenShareRoomSession.

RoomManagedPeer now joins authenticated negotiation to HostPeerOwner's scheduled
recovery and capture-cleanup barriers in the four-peer scenario. Capture attachment
and retirement are asynchronous on signaling. BeginStop shares completion across
requests and drains capture without blocking signaling; use it before destruction.
Normal UI/CLI facade dispatch is still unfinished. See CHECKPOINT-B.

RoomNetwork now owns one Qt network loop for admission/room sockets, asynchronous
commands/cancellation, bounded queues and terminal overflow. RoomPeerRoster drives
actual media lifecycle from authenticated snapshots in RoomMediaProof, including
socket reconnect and kick/rejoin. No caller Qt event pump is needed. The diagnostic
still waits on command/capture futures; shared automatic facade dispatch, cleanup
barriers and recovery-policy integration remain open. See CHECKPOINT-B.

RoomMediaProof now runs four actual H.264 peers and synthetic Opus against isolated
workerd via native admission/sockets and shared RoomPeerNegotiation. Slow-viewer
isolation, twelve encrypted channels, fresh-ID restart, stale candidate rejection,
kick/rejoin and close pass. Full Debug/Release proof suites: 28/28; Release app:
13/13; CLI-only: 8/8. The normal UI/CLI facade remains legacy and automatic roster/
failure/recovery ownership is still open. Local evidence does not close latency,
resource, remote-network or cost gates. See CHECKPOINT-B and HEADLESS-TESTING.

## User Preferences

- Native sources are now split into root-level backend/ and frontend/ (UI, CLI,
  updater). Keep backend include paths independent of frontend. Existing include
  spellings are retained through target-specific roots; no compatibility src tree.

- Work in the end-to-end delivery milestones in refactor/TODO.md, not one helper/check per turn. Detailed checks moved to refactor/DETAIL-CHECKS.md and are acceptance details, not stopping points. Carry integration and meaningful failure scenarios together; intermediate commits do not end the batch.

- Use the patch editing tool for source/document changes so edits are reviewable in Codex; do not use PowerShell/Python file rewrites. Continue committing at logical milestones. Committed work can be reviewed using the commit or branch diff rather than only unstaged changes.

- Build a native Windows C++ screen-sharing app for friends.
- No web app, no C#.
- Use local `git` and `gh` for GitHub work; do not use the GitHub connector for this repo.
- Run local `gh` commands with escalated permission from the start; do not first try them in the sandbox.
- Keep PRs cohesive and reviewable; bigger PRs are fine when they advance one clear step. Self-review before opening or merging.
- Commit at substantial logical milestones during implementation; avoid tiny commits and accumulating completed milestones without a commit.
- Continue through larger feature milestones in one turn; intermediate commits are checkpoints, not reasons to stop and require another “continue”.
- Prefer the cleanest simple design when it has no real product or maintenance downside, even if it takes more implementation effort.
- Do not keep compatibility/fallback code for a feature that is being replaced unless there is a concrete product or diagnostic reason.
- Avoid branch, commit, PR, or tracked-file text that depends on one contributor's local tooling.
- After each completed work step or PR merge, mention the next recommended step.
- During revamped UI work, do not grow one giant UI file. Split code into logical screens/widgets/styles/adapters while keeping the implementation simple.
- When a new UI screen first runs, pause and ask the user for a screenshot so the visual design can be validated before moving on.
- Durable repo memory lives under `agents/`; update it when project direction or implementation facts change.

## Current State

- Compiled Qt RoomAdmission/RoomSocket now have a one-command workerd integration harness (signaling-worker/tests/run-native-service.mjs), also registered with CTest when dependencies exist. Snapshot events carry accepted revisions for safe edits. Coverage joins native/service admission, directory push/resync, edits/conflicts, synthetic signaling, reconnect, visibility and closure. The actual application coordinator/media facade still needs adoption; remote TLS and media latency are not established.

- Directory/capacity delivery is now a bounded background outbox outside room input/state gates. A shared state serializer and version-matching acknowledgements protect newer changes and closure; the latest pending value survives failures for alarm retry. Actual workerd tests cover stalled service with concurrent control/signaling, late ACKs, close races and the real 5s abort. Normal native/media integration and production hibernation/load/queue acceptance remain pending.

- V2 directory completes the isolated service's six endpoints: SQLite snapshot/pushed deltas, no per-room listing fanout, publish-after-host-attach, persisted versioned retries, closure tombstones, 60s renewals/180s leases and silent lease-only updates. Actual workerd tests cover failures/retry, lifecycle/visibility, expiry and 500 max-name rows. Native/media adoption, hibernation/load/queue pressure and production cutover remain open.

- V2 directed signaling now enforces pair roles, current socket generations, fresh per-offer connection IDs (including restart), candidate limits and bounded recovery/history. Workerd tests relay offer/answer/ICE/restart without room revision changes and reject stale/cross-viewer traffic. Native media adapter must map its negotiation generations to fresh wire IDs; directory, production queue pressure and cost acceptance remain pending.

- V2 room mutations now enforce socket-derived host/viewer permissions, expected revisions, last-32 request deduplication and a 120-message/minute metadata budget. Live workerd tests cover profile/policy updates, reduced capacity without eviction, kick/leave token invalidation and host closure. Saves preserve earlier alarm deadlines. Directory/signaling and normal application integration remain open.

- Isolated v2 Worker admission and socket membership now live under signaling-worker/src/v2 with wrangler.v2.toml. Real Miniflare/workerd tests cover credentials, serialized joins, provisional alarm cleanup, socket replacement/reconnect, automatic pong and the global 500-room cap. Default v1 remains active and nothing is deployed. Directory, command/signaling authorization, per-socket bounds, native/media integration and real hibernation scheduling remain open; see refactor/CHECKPOINT-C.md.

- Native RoomAdmission now implements single-flight asynchronous create/join with HTTPS origin validation, bounded responses, no redirects/cookies/cache, exact credentials/role checks and unconfirmed cancellation/failure results without application retries. Success returns a same-origin RoomSocket config with expectedRole; conflicting snapshots reject. Actual HTTP and role-mismatch socket tests are in the application/headless suite. The exact future v2 service contract is in `refactor/ROOM-PROTOCOL.md`; service handlers/expiry, media routing and normal UI/CLI adoption remain open.

- ScreenShareRoom now shares validators/cache and private Qt RoomSocket across backend builds, including CLI-only builds. Live tests on a separate QThread cover headers, snapshot/resync, target/role checks, actual heartbeat/reconnect, directory lifecycle and write pressure. Remote endpoints require wss; plaintext is an explicit numeric-loopback diagnostic option. Normal UI/CLI, admission, media connection-ID dispatch and v2 Worker integration remain open. See `refactor/CHECKPOINT-C.md`. `--room-build-directory` adds this test to the headless runner; `run-webrtc-proof.ps1 -Application -NoUi` supports separate CLI-only validation.

- HostPeerOwner now binds HostPeerRegistry to SignalingExecutor and automatically advances deadlines, ready peer operations, restart and capture cleanup every 20 ms on signaling. IMediaPeer::Poll must never wait/reenter; failures are isolated and reported as operationFailed. Four-peer diagnostics now negotiate concurrently, then complete restart/rejoin through this owner instead of manually ticking the registry or synchronously transferring restart SDP. Owner/capture must be destroyed before the executor; delayed callbacks hold weak state. Authenticated room delivery and UI/CLI facade remain open. See `refactor/CHECKPOINT-B.md`.

- SignalingExecutor now owns the native WebRTC event loop in the shared application/proof library. It admits 64 pending commands, reports typed outcomes, cancels queued work on stop and joins externally; accepted closures are destroyed on signaling. Real single/four-peer media proofs use it, and a separate offer/answer test proves progress without manual pumping. The application peer owner, room transport and UI/CLI facade remain next. Close/destroy peers on the executor before stopping; never join it from its own thread. See `refactor/CHECKPOINT-B.md`.

- PeerNegotiation now implements asynchronous SDP creation/application with typed futures, weak callback cancellation and one in-flight operation, in a shared application/proof library. The old proof description observers are removed; real media/restart tests use the adapter. Application executor and authenticated room delivery remain next; UI/CLI media has not switched. See `refactor/CHECKPOINT-B.md`.

- HostPeerRegistry now optionally binds HostMediaSession for automatic asynchronous failure/removal cleanup. Tick retries queue pressure, snapshots expose pending/error state, and bound Remove is accepted asynchronously (wait for row disappearance before reuse). Full Stop joins capture first; capture must outlive the registry. The four-peer proof covers terminal failure cleanup/rejoin; application executor and room adapter integration remain open.

- HostPeerRegistry/IMediaPeer now own real four-peer proof connections and dispatch queued restart/close actions with generation validation and retained terminal snapshots. Adapter shutdown is idempotent; capture stops before peer release. Application signaling scheduling, room delivery and capture cleanup on peer failure remain open. Admission is in strictly increasing generation order; see `refactor/CHECKPOINT-B.md` for the embedding contract.

- Checkpoint B now includes a portable per-connection deadline/recovery policy and a real four-peer ICE restart scenario: fresh credentials, retired-candidate filtering, preserved settings and media isolation. Production action dispatch/room integration remains open. Restart JSON separates negotiation, frame-check and ICE-state-confirmation timing; early evidence showed prompt frames but delayed state notification, so do not report the latter as a media outage. See `refactor/CHECKPOINT-B.md`.

- Real native media proofs now use separately trickled ICE through portable `IceCandidateHandoff`, with description readiness barriers, stale-generation rejection, bounded candidates and callback release on close/failure. Debug/Release media suites pass 22/22. Authenticated room integration, connection deadlines/restarts and STUN/NAT tests remain open; normal application media is unchanged. See `refactor/CHECKPOINT-B.md`.

- Checkpoint B host capture/membership coordinator is implemented: serialized bounded commands, operation/session IDs, stale rejection, priority queued cancellation, subscriber isolation and joined workers. Four-peer proof uses it; peer/signaling ownership and the shared application facade remain next. Debug/Release media suites pass 21/21, applications 10/10, headless Debug smoke 6/6 and Release regression 27/27. See `refactor/CHECKPOINT-B.md`. Before/after claims require matched evidence under `refactor/COMPARISON.md`; native handle-growth failure remains open.

- Backend v2 is authorized on `refactor/backend-v2`. Gate A passed its original native integration/build criterion on 2026-09-15; current verdict: `refactor/CLOSEOUT-A.md`. Native media suites pass 20/20 in Debug/Release, application suites 10/10, and the rebuilt Release package passes extracted CLI/UI/GUI smoke tests with developer paths removed. Reusable capture ownership, bounded viewer delivery, four-peer headless/rejoin proofs and Auto/Manual source/RTP settings are implemented; complete session-facade integration is Checkpoint B. Important open defect: closeout current-binary resource checks completed 100 full and 20 rapid-close cycles without crash/timeout but FAILED handle bounds (+76 and +10, limit 8). Earlier passing MTA resource runs are historical evidence, not a current no-leak guarantee. Keep the application MTA lease, capture-owner dispatcher and GraphicsCapture.dll pin; investigate resource lifetime before production cutover. Full hardware/latency acceptance remains in E, Qt/distribution obligations and actual Inno/fresh-machine checks in release preparation. Normal application media remains legacy until integration is validated. Do not keep adding B/D/E work to A; use PLAN.md, TODO.md and CLOSEOUT-A.md.

- `main` is synced to `origin/main` after the backend API split and UI process-adapter merge.
- The app builds with CMake debug/release presets and produces `ScreenShare.exe`.
- Reusable native engine modules now build through `ScreenShareCore`; runtime-backed Share/Watch execution and the concrete session facade build through `ScreenShareAPI`. `ScreenShare.exe` compiles CLI parsing and links that API, while `ScreenShareUi.exe` links the API directly.
- The reusable typed session API step is complete; active build work is now the revamped native UI on top of `ScreenShareAPI`.
- Normal/default CMake builds now also create portable zip packages.
- The app can also build optional `ScreenShareUi.exe` when Qt 6 Widgets and Svg are available.
- `scripts/install-dev-deps.ps1` bootstraps Windows dev dependencies: MSYS2 native packages, optional Qt/FFmpeg, Node.js LTS, and signaling Worker npm packages.
- Current stable live run shape:

```powershell
.\build\release\ScreenShare.exe --udp-recv 5000 --preview --audio-playback --log receiver.log
.\build\release\ScreenShare.exe --display 0 --seconds 18000 --udp-send HOST:5000 --adapt-bitrate --adapt-resolution --audio-capture system --log sender.log
```

## Current Pipeline

```text
WGC capture by default
 -> D3D11 scaling / HDR handling / GPU NV12
 -> H.264 stream encoder; `--share` defaults to software for stability, raw stream encoding can still use auto/hardware
 -> paced UDP sender with feedback and optional adaptation
 -> receiver UDP reassembly and keyframe-aware H.264 decode
 -> D3D11 preview window with paced playout
 -> WASAPI system/mic capture, Opus by default
 -> receiver audio playback with A/V sync enabled by default for preview+audio
```

## Recent Merges

- PR #42 `Stage MinGW runtime DLLs`: static MinGW runtime plus staged Opus/UCRT/D3DCompiler DLLs.
- PR #43 `Trim delayed audio playout backlog`: audio playout backlog trimming.
- PR #44 `Use stable live streaming defaults`: Opus default and A/V sync default for preview+audio.
- PR #45 `Stabilize live A/V playout`: log files, sync fixes, sender queue diagnostics, adaptation pressure tuning.
- PR #46 `Add portable zip package target`: automatic portable zip in normal builds and explicit `package-portable`.
- PR #47 `Add live session presets`: `--watch PORT` and `--share HOST:PORT` shortcuts for common live sessions.
- PR #48 `Add save report command`: global `--save-report PATH` to capture one run's console output into a zip report with runtime info.
- PR #49 `Add shared diagnostic session metadata`: `--session` IDs, report fingerprints, and receiver session fingerprints in feedback.
- PR #50 `Add receiver feedback summary to reports`: sender reports include the latest observed receiver health snapshot when feedback is available.
- PR #51 `Add receiver preview window controls`: fullscreen, fit/1:1 scaling, and source-size resize shortcuts.
- PR #52 `Add receiver audio playback controls`: muted loopback-safe receiver playback, volume controls, audio status telemetry, and DXGI Alt+Enter handling fix.
- PR #53 `Add Qt control UI`: optional Qt desktop UI for Share/Watch presets, Start/Stop, live output, report creation, packaging updates, graceful stop-file shutdown, and video-only fallback when automatic sync sees video but no audio.
- PR #54 `Add LAN receiver discovery`: opt-in `--lan-advertise`, `--lan-discover`, UI Watch LAN discoverable checkbox, UI Share Find on LAN button, and directed IPv4 broadcasts for real LAN discovery.
- PR #55 `Add local access code gate`: `--access-code` / `--session-code` for matching local sessions, packet fingerprint filtering, UI access-code field, telemetry, and report/command redaction.
- PR #56 `Encrypt local UDP sessions`: access-code-derived AES-GCM encryption for video/audio/feedback payloads, crypto rejection telemetry, and report/README updates.
- PR #57 `Improve encrypted session UX`: random access-code generation, explicit plaintext acknowledgement, plaintext warnings, and UI Generate/Copy/security-choice flow.
- PR #58 `Advertise LAN session security metadata`: discovery reports encrypted/plaintext receiver state, access-code fingerprint metadata, safer generated commands, and UI Find-on-LAN session/security hints.
- PR #59 `Simplify LAN discovery access-code flow`: removed the separate invite-code direction, moved access-code fingerprinting into shared UDP crypto, and made the UI compare the typed access code to the receiver's advertised fingerprint.
- PR #60 `Stabilize share sender queue adaptation`: `--share` live UDP queue cap, sender-queue resolution pressure, 125% video pacing headroom, and software encoder default for `--share` after hardware MFT input drops were confirmed.
- PR #61 `Quiet idle receiver telemetry`: receiver logs one `waiting_for_stream` line while idle instead of repeating full zero-value stats.
- PR #62 `Keep receiver sync catch-up real-time`: preview is the live timeline, audio catch-up can drop queued audio, and video safety gating avoids reciprocal sync deadlocks.
- PR #63 `Keep preview running without audio`: automatic video-only fallback stops using stale audio renderer state as a preview/audio gate.
- PR #64 `Add receiver discovery list to UI`: Share tab has an auto-refreshing Receivers list, relaxed 15-second background refresh, de-duped loopback/LAN entries, automatic UI sessions, single-field access-code retry behavior, tighter settings layout, and no wheel focus/input changes while scrolling.
- PR #65 `Warn before sharing to localhost`: UI warns before starting Share to loopback targets like `127.x.x.x`, `localhost`, or `::1`, while still allowing explicit local tests.
- PR #66 `Add STUN endpoint diagnostic`: standalone `--stun HOST[:PORT]` command sends a STUN Binding Request and prints local/public/server/manual invite UDP endpoints.
- PR #67 `Add audio output device selection`: Share UI output-device picker, `--audio-device-id` command wiring, and multichannel capture downmix before Opus.
- PR #68 `Add Tailscale peer picker to UI`: Share UI Targets list includes optional online peers from `tailscale status --json`; these are quick targets, not confirmed ScreenShare receivers.
- PR #69 `Add NAT invite generation command`: `--make-invite` emits manual invite blobs with public/local endpoint and security metadata.
- PR #70 `Add NAT probe exchange diagnostic`: `--nat-probe` sends probe/reply packets to peer invite endpoints for manual UDP hole-punch testing.
- PR #71 `Allow UDP sender local port binding`: `--udp-local-port` binds Share/live UDP send to a known local port.
- PR #72 `Send NAT punch probes from Watch socket`: Watch/UDP receive can use `--peer-invite` to send NAT punch probes from the real receive socket while waiting for media; sender feedback ignores probe datagrams.
- PR #73 `Allow Share to target NAT invites`: `--share "nat_invite=..."` resolves the receiver invite public endpoint, validates invite security, and documents the two-invite manual flow.
- PR #74 `Add NAT invite endpoint selection`: `--invite-endpoint auto|public|local` lets Share choose the invite endpoint for manual NAT/LAN/VPN experiments; local endpoint startup was validated with an approved one-second Share run.
- PR #75 `Retarget Share from NAT probes`: Share auto mode can retarget its UDP media destination to the source endpoint of incoming Watch NAT probe datagrams; access-code fingerprints reject mismatched retarget attempts.
- PR #76 `Bind Share sender from local NAT invite`: Share `--local-invite INVITE` binds the sender UDP socket to the local port inside this side's invite, reducing the chance that live NAT tests forget the matching `--udp-local-port`.
- PR #77 `Add NAT setup status diagnostics`: sender/receiver logs include `nat_status` / `nat_hint` fields.
- PR #78 `Print guided NAT invite commands`: `--make-invite` prints Watch/Share/probe command templates using `<PEER_INVITE>` and `CODE` placeholders.
- PR #79 `Add NAT invite fields to UI`: Share can paste receiver/local invites, Watch can paste sender invites, and the command preview masks invite blobs.
- PR #80 `Add UI invite creation flow`: Share/Watch can create and copy their own local NAT invite blobs from the app.
- PR #81 `Guide NAT invite exchange in UI`: paste/extract buttons, compact status hints, and Share-side guardrails for receiver invite flow.
- PR #82 `Reset receiver pipeline on stream restart`: Watch now recovers when Share stops and starts again without restarting Watch.
- PR #83 `Show live NAT status in UI`: Internet hints switch to live probe/media states from engine `nat_status` / `nat_hint` output while NAT invite mode is running.
- PR #84 `Add UI invite test checklist`: README has a two-computer checklist for future UI invite/Tailscale validation.
- PR #85 `Add direct NAT invite sharing flow`: merged the full direct STUN/manual invite/UDP hole-punching flow into `main`.
- PR #86 `Clarify room UI setup`: Share/Watch wording, segmented connection tabs, display/audio chooser polish, header status pill, receiver stale-preview blanking, and Windows UDP reconnect resilience for late/restarted Watch.
- PR #93 `Add NAT multi-viewer room invite targets`: one sharer room invite, optional watcher response invite list, NAT probe-learned fanout through one sender socket, and direct multi-target Share UI cleanup.
- PR #94 `Add Cloudflare signaling worker scaffold`: signaling-only Worker project, Windows dependency bootstrap script, Worker lockfile/typecheck config, and hidden room-key direction docs.
- PR #99 `Secure Worker room signaling`: hidden app-generated room keys in secure room links, Durable Object live room state, static-candidate fanout fix, and clearer direct-UDP-blocked diagnostics.
- PR #100 `Add active room directory`: KV-backed active room summaries plus safe `GET /rooms` and `GET /rooms/:roomId/summary`, with Durable Objects still verified as the live source of truth.
- PR #102 `Add joinable room list with room access keys`: UI room list joins public Worker rooms without copied secret links; Durable Object keeps the hidden room access key.
- Recent backend/UI integration: `ScreenShareSession` emits typed diagnostic events for stream/audio media status, NAT status, access-code/password failures, room-open conflicts, and preview-close handling, replacing the remaining live-session stdout parsing in `ScreenShareUi`. Live Share/Watch starts now flow through typed session configs for Worker rooms, direct/Nearby targets, and manual invite fallback. Runtime control is typed around stream settings, with live resolution changes as the first implemented field.

## Active Memory Files

- `agents/todo.md`: single source for upcoming tasks and backlog.
- `agents/repo-map.md`: source layout and build shape.
- `agents/live-pipeline.md`: sender/receiver pipeline and important stats.
- `agents/av-sync.md`: A/V sync behavior and test command shape.
- `agents/adaptation.md`: adaptive bitrate/resolution notes.
- `agents/packaging.md`: runtime DLL and portable zip notes.
- `agents/ui.md`: Qt desktop control UI notes.
- `agents/lan-discovery.md`: LAN discovery protocol and test notes.
- `agents/nat-traversal.md`: STUN/manual invite/hole-punching direction.
- `agents/security.md`: local access-code and future encryption notes.
- `agents/signaling.md`: signaling backend direction and room-flow constraints.
- `backend/api/ScreenShareAPI.h`: public concrete `screenshare::ScreenShareSession` API facade used by the UI and intended for the CLI path as it gets thinner.
- `backend/api/ScreenShareAPI.cpp`: `ScreenShareSession` implementation using memory runtime control, typed Share/Watch runner entrypoints, and typed event/status translation.
- `backend/core/ScreenShareSession.h`: shared session data types and helpers used by the API, CLI, and UI.
- `backend/core/SessionCommand.*`: typed Share/Watch session config to engine-argument bridge still used for UI command previews/self-tests, not normal live session execution.
- `backend/core/SessionRuntimeControl.*`: shared stop/runtime stream-settings control interface. CLI runs use the file-backed implementation; `ScreenShareSession` uses the memory-backed implementation for stop/settings requests. Resolution is the first implemented live setting.
- `backend/runtime/ScreenShareRuntimeInternal.h`: private runtime bridge used by the session runtime; not a UI-facing API.
- `backend/runtime/ScreenShareRunContext.h`: shared run context for runtime control and captured output callbacks.
- `backend/runtime/ScreenShareRuntimeOptions.h`: shared `Options` model, runtime constants, and small option enums/target specs used while CLI parsing and runtime execution are being split.
- `backend/runtime/ScreenShareSessionOptions.*`: shared typed Share/Watch config-to-runtime-options conversion plus session/access-code validation and NAT target helpers. CLI presets and `ScreenShareSession` typed runs use this instead of duplicating typed setup logic in `ScreenShareCLI.cpp`.
- `backend/runtime/ScreenShareSessionRunner.h`: typed Share/Watch runner entrypoints used by the concrete session API; this is separate from the CLI app header.
- `backend/runtime/ScreenShareSessionRunner.cpp`: runtime-backed typed Share/Watch runner entrypoint implementation. It owns the typed-run report/log wrapper, then calls shared runtime execution directly.
- `backend/runtime/ScreenShareRuntimeExecution.cpp`: shared normal runtime execution for capture/send, receive/preview/audio playback, standalone audio capture, live signaling setup, adaptation policy, and typed Share/Watch execution entrypoints.
- `backend/runtime/ScreenShareRuntimeSupport.*`: shared session ID/fingerprint, stdout/stderr capture, saved-report zip, and argv support used by both CLI and typed runtime paths.
- `frontend/cli/ScreenShareCLI.*`: CLI parser/report wrapper compiled only into `ScreenShare.exe`. Normal CLI Share/Watch presets parse into typed session configs and use the same typed execution path as the UI/backend path; diagnostic-only CLI modes still parse into internal options directly and route through CLI-owned command dispatch.
- `frontend/ui/QtSessionBackend.*`: Qt-thread bridge over `screenshare::ScreenShareSession`. Live Share/Watch and normal display/audio-device discovery in the desktop UI no longer launch `ScreenShare.exe` as a child process.
- `assets/brand/` and `assets/ui/icons/`: first-pass logo and button icon SVG sources for the revamped UI.
  The Qt UI embeds the current mark/icons through `frontend/ui/resources.qrc` and links/packages QtSvg for SVG rendering.

## Current Direction

- The remaining freeze/stale-frame work is intentionally backlog, not the default next PR.
- PR #47 merged the first run/session preset slice:
  - `--watch PORT` expands to receiver preview plus audio playback.
  - `--share HOST:PORT` expands to UDP video send, system audio capture, adaptive bitrate, and adaptive resolution.
  - `--share` defaults to infinite run time; `--seconds S` still overrides it for tests.
- Report bundle support is merged:
  - `--save-report PATH` captures the current run's console output into a zip with `ScreenShare-report.txt`, `logs/console.log`, and the runtime dependency manifest when available.
  - `--log PATH` remains useful for plain text logs and can be combined with `--save-report`.
- Shared session metadata is merged:
  - Add global `--session ID` / `--session-id ID` with an automatic generated session when omitted.
  - Save session ID and fingerprint in `ScreenShare-report.txt` and console telemetry.
  - Include the receiver session fingerprint in existing feedback packets, appended for compatibility with the previous feedback packet shape.
  - Sender stats report `udp_feedback_session`, which can be matched to the receiver report's session fingerprint.
  - Elevated localhost live loopback validated that the sender sees the receiver fingerprint in feedback.
- Receiver feedback report summary is merged:
  - Preserve the latest sender-observed receiver feedback snapshot in the saved-report context.
  - Add a compact `Latest receiver feedback` section to `ScreenShare-report.txt`.
  - Elevated localhost sender/receiver validation confirmed sender reports include receiver health, completed frames, and receiver session fingerprint.
- Receiver UX controls are merged:
  - Preview: F11/Alt+Enter fullscreen, Esc exit fullscreen, F fit/1:1, 1 source-size resize.
  - Audio: `--audio-playback-muted`, `--audio-playback-volume PERCENT`, M mute, + and - volume.
  - DXGI's default Alt+Enter handling is disabled so the app-owned fullscreen restore keeps the title bar.
- Qt control UI is merged:
  - optional `ScreenShareUi.exe` when Qt 6 Widgets and Svg are available.
  - dark-mode default with theme toggle.
  - Share/Watch presets, Start/Stop, command preview, live output, session/report controls.
  - portable zip includes Qt plugin folders and transitive runtime dependencies.
  - Live Share/Watch runs now go through `frontend/ui/QtSessionBackend.*` and `screenshare::ScreenShareSession`, so the UI calls the in-process session API on a worker thread instead of launching `ScreenShare.exe`.
  - Live Share/Watch now builds engine options directly from typed configs for Worker rooms, direct/Nearby targets, and manual invite fallback. Normal CLI Share/Watch presets produce the same typed configs; advanced diagnostic-only flags remain on the internal CLI options path until they become real app controls. Stop/runtime stream-settings controls route through `backend/core/SessionRuntimeControl.*` with file-backed control for CLI runs and memory-backed control for UI runs.
  - `ScreenShareSession` now translates live telemetry into typed `SessionEvent` snapshots: Share viewer rows use typed viewer status, Watch/Share live indicators use typed activity, and NAT hints, access-code/password failures, room-open conflicts, and preview-close handling flow through typed events instead of UI-side stdout parsing.
- LAN discovery is merged:
  - `--lan-advertise` on watch/receive mode.
  - `--lan-discover` search mode.
  - UI Watch LAN discoverable checkbox and Share Find on LAN button.
- LAN discovery security simplification is merged:
  - One access code remains the user-facing secret.
  - Discovery advertises encrypted/plaintext state and fingerprint metadata, never the raw access code.
  - The UI can compare the typed access code against the discovered receiver fingerprint.
- Receiver discovery UI is merged:
  - Share has an auto-refreshing Receivers list plus manual Refresh.
  - Selecting a discovered receiver fills address/port; encrypted receivers use the main Access code field only.
  - Wrong discovered access codes clear/focus the Access code field for direct retry.
  - The UI warns before Share starts with localhost/loopback targets.
- Tailscale peer picker is merged:
  - It uses optional local `tailscale status --json` output in the UI.
  - Entries are quick targets only; they do not prove Watch is running and do not carry ScreenShare security metadata.
  - Keep this separate from UDP LAN discovery.
- Direct NAT traversal building blocks are merged to `main`:
  - `--stun HOST[:PORT]` defaults to port 3478.
  - `--stun-timeout-ms MS` controls the query timeout.
  - Validated against `stun.l.google.com:19302`; output includes local and public UDP endpoints.
  - `--make-invite`, `--nat-probe`, `--udp-local-port`, Watch `--peer-invite`, Share `--share "nat_invite=..."`, `--invite-endpoint auto|public|local`, Share `--local-invite`, and Share auto retarget from Watch probes are now available for manual hole-punch experiments.
  - `--make-invite` prints guided command templates for Watch, Share, and optional probe diagnostics.
  - Current generated invites are compact by default: `ss1e:` is encrypted with the shared access code and hides endpoint/session metadata; `ss1p:` is compact plaintext for explicit plaintext mode. Legacy verbose `nat_invite=screenshare-invite-v1;...` strings still parse.
  - Current UI bridge presents one sharer-owned room invite for LAN/VPN/reachable-NAT and an optional Watch-side My invite response for blocked NAT pairs. Share uses the friend response as `--share` and its own room invite as `--local-invite`.
  - Guided UI work adds create/copy/paste/extract buttons and compact status hints for the one-invite room flow.
  - NAT logs now summarize setup state with `nat_status` / `nat_hint` so reports can distinguish missing probes, rejected probes/media, retarget-without-feedback, and receiving states.
  - Remaining NAT work is deferred unless reports show direct hole punching is insufficient or diagnostics are still confusing.
- Receiver restart work:
  - This is not NAT-specific; the receiver should recover whenever a sender stops and starts again while Watch remains open.
  - The fix detects a video frame-id rewind with a newer sender QPC clock and resets decode, preview, audio playout, A/V sync, and receiver media queues.
  - Debug loopback validation showed `receiver_stream_restart`, one H.264 decoder restart, and continued decode across two sequential sender runs.
- UI live NAT status is merged:
  - The Qt UI can parse engine `nat_status` / `nat_hint` output while Share/Watch is running.
  - The Internet hint should switch from setup guidance to live states like probing, probe seen, connected, receiving, or rejected.
- NAT validation state:
  - User already validated the UI-guided invite flow after create/copy invite landed.
  - Later changes did not alter the NAT invite/probe/media mechanics, only receiver restart recovery and UI status display.
  - README has a two-computer UI checklist for future invite/Tailscale validation runs.
- Latest audio failure diagnosis:
  - Tailscale/transport was healthy; receiver got audio packets and playback was active.
  - The sender captured an 8-channel SteelSeries virtual output, but receiver Opus payloads were tiny silence-like packets, pointing to the wrong output/default device rather than packet loss.
  - Keep a small multichannel-to-stereo downmix safeguard, but the real UX fix is making the Share UI's output-device selection obvious.
- PR #60 fixed the freeze issue:
  - User confirmed it works and audio sounds fine too.
  - Reports showed the bad path was not LAN discovery/plaintext/encryption or native resolution alone.
  - The NVIDIA hardware H.264 MFT built an input queue and dropped frames; local tests showed software had zero `stream_dropped` at 640x360, 1280x720/60, and native 2560x1440/60.
  - `--share` now defaults to software encoding while `--stream-encoder hardware` remains available for experiments.
- Multi-viewer direction:
  - First slice is direct UDP fanout: one encoded video/audio stream is sent to multiple direct `HOST:PORT` targets using separate `UdpSender` instances.
  - Each target owns its own UDP socket, queue, and feedback path; sender telemetry aggregates counters and reports `udp_targets`, `udp_active_targets`, and `udp_failed_targets`.
  - Runtime send/feedback/flush errors on one target should mark only that target failed and keep the remaining viewers alive.
  - The Qt UI exposes direct multi-target sharing through Nearby multi-select and Manual comma/space-separated target lists.
  - NAT multi-viewer direction is one shared sharer room invite plus an optional watcher response invite list. Share binds the sharer invite's local port, learns watcher endpoints from valid NAT probes, sends outward to any pasted watcher response invite endpoints, and fans out the encoded stream through the same sender socket. Per-watcher sharer-local invite rows are intentionally not the main UI model.
  - v0.2.3 gives every signaling viewer a stable-ID send lane over the shared NAT-bound socket, with independent queue/pacing/drop/error state, GOP-aware recovery, fresh-feedback state, and host disconnect.
  - Global bitrate reduction now requires agreement from all fresh viewers; one degraded outlier cannot pull down healthy viewers.
  - `scripts/run-multiviewer-harness.ps1` covers healthy, loss/jitter, slow-consumer, late-join, leave/rejoin, and unreachable scenarios and writes per-viewer CSV/assertions.
- Live stream controls direction:
  - User reports native/2K looks sharp, but 1080p stretched to a 2K preview is still blurry after sharper scaling. Do not assume bitrate is the cause; after the revamped UI, investigate whether live resolution changes accidentally alter bitrate, encoder quality, chroma/subsampling behavior, scaler path, or preview upscale behavior.
  - After the revamped UI, add a live settings panel that can change parameters without restarting the room: Quality/Bitrate, FPS/adaptive FPS, resolution, encoder preference/preset, audio device, and audio mute.
  - Adaptive FPS should share the same typed runtime stream-settings control model as bitrate/resolution and reduce frame rate only under sustained pressure.
- Signaling direction:
  - First backend lives under `signaling-worker/`.
  - It is Cloudflare Worker + Durable Objects for live room membership, peer UDP candidates, heartbeat, cleanup, and the browseable active-room directory.
  - Durable Object room state replaced KV room storage after real reports showed asymmetric visibility (`Watch` saw `Share`, while `Share` stayed at zero targets) during rapid join/poll cycles.
  - It must not relay media or store plaintext room passwords/user-visible access codes.
  - For no-password public rooms, the Durable Object generates and stores a random room access key. Native clients use it as the hidden UDP access code so users get encrypted UDP media without seeing an access-code field. This is encryption, not private access control.
  - Optional room passwords are verified by the Worker over HTTPS with a salted verifier stored in the Durable Object, then the native app also mixes the typed password into the hidden room access key before UDP key derivation.
  - Native C++ diagnostic integration started with `backend/transport/SignalingClient.*` plus `--signal-health`, `--signal-join`, `--signal-peers`, `--signal-heartbeat`, and `--signal-leave`.
  - Live Share/Watch CLI integration is in progress: `--watch PORT --signal-room ROOM` publishes the watcher candidate and turns returned peers into NAT probe targets; `--share-room PORT --signal-room ROOM` publishes the sharer candidate and turns returned peers into UDP send targets. `--signal-server URL` overrides the built-in Worker.
  - Runtime live signaling uses authenticated heartbeat/peer reads plus WebSocket events; it re-announces only when local metadata changes. Share can start before Watch and wait for peers; Watch can add newly discovered room peers as NAT probe targets; Share can add newly discovered watcher candidates to the active sender socket.
  - The UI default Internet path now uses the built-in Worker `https://screenshare-signaling.bit-yeet.workers.dev`: Share has Room ID, friendly Name, optional Password, and Port, and copies a short `screenshare-room-v1;room=...` link; Watch can pick an active room from `GET /rooms` or paste the link. The engine receives the room access key from signaling during join and uses it as the hidden UDP access code, optionally mixed with the room password.
  - `GET /rooms` returns directory Durable Object-backed active-room summaries for browse/debug views, and verifies each listed room against its room Durable Object so stale entries do not stay visible in the API. `GET /rooms/:roomId/summary` returns one safe summary or `null`.
  - Live signaling publishes a `host` local candidate alongside the `srflx` STUN candidate when available, so same-PC and same-LAN room tests can use the direct local path instead of relying on router hairpin behavior.
  - Manual NAT invite fields still exist behind a fallback checkbox.
  - `ScreenShareUi` runs `windeployqt` through `cmake/RunWindeployQt.cmake`; the script always verifies/copies the current Qt DLLs/plugins and resolved MinGW runtime deps so the release UI does not keep stale mismatched Qt files.
  - Keep the UI runtime consistently UCRT (`C:/msys64/ucrt64/bin`); mixing `mingw64` and `ucrt64` Qt/ICU/libstdc++ DLLs causes Windows entry-point loader errors before the app starts.
  - Remaining signaling TODO is real multi-computer/multi-viewer validation across separate NATs.

- Receiver recovery follow-up: v2 presentation now catches typed DXGI device failures, releases resources on the window thread, drops frames during 250 ms backoff and allows three rebuilds per presenter lifetime. Fourth failure is terminal. Debug/Release policy/resource-recreation proofs pass with injected failures; actual driver removal and capture/encoder recovery remain open. Hardware/audio proof suites now pass 13 tests each; application suites pass 8 each. Desktop GPU proof requires `PresentationRecoveryTest --gpu`. Foundation committed as `4ca8318`; continue substantial logical commits.

- Capture/encoder recovery follow-up: original-item WGC resource reconstruction and shared hardware-session retirement are implemented and tested. Retired-device native frames drop without readback; two viewers resume software IDRs from a replacement device. Live WGC rebuild/closure passes three cycles per Debug/Release; proof suites 13/13 and application suites 8/8. Automatic capture recovery, bounded coordinator retries and repeated device generations remain pending, as does actual driver-removal testing. Continue with those integration boundaries before claiming full recovery or passing Gate A.

- Automatic proof recovery milestone: LiveCaptureSource now handles typed device loss with three rebuilds, 250 ms cancellable backoff, first-frame deadline and device generation counters. Every D3dVideoDevice owns retirement; old/cached frames from replacement devices are rejected, and two encoders resume IDRs across three owners. Debug/Release suites pass 14/14; automatic capture and live WebRTC desktop proofs pass. One Release LiveCaptureTest repeat run failed around second-cycle shutdown without exception text; standalone and cdb reruns passed. Investigate this intermittent failure next; do not mark lifecycle or Gate A complete. Production coordinator/callback-generation barriers and actual driver removal remain unverified.

- Larger build-delivery milestone: both content-addressed WebRTC SDKs exported with full file hashes, compiler builtins and dependency notices; CMake accepts relocated SDKs and validates ABI/compiler/MSVC/SDK identity. Fresh proof suites pass 14/14, fresh application suites 9/9 after adding the UI self-test. Native Release zip passes extracted CLI/self-test/Windows GUI startup with developer PATH removed and package-local plugins confirmed. Fixed MSVC runtime staging, native Qt plugin layout/Debug selection, and Qt application initialization in self-test. Twenty debugger plus twenty normal Release capture cycles passed, but the prior unexplained lifecycle failure remains open. See latest BUILD.md/CHECKPOINT-A.md sections. Next: remaining Gate A protocol fixtures, external baseline, lifecycle investigation, installer/distribution evidence. Normal application media remains legacy.

- Application COM lifetime milestone (2026-09-15): remaining retained events traced to COM OXID/apartment remoting. Scoped WindowsMediaRuntime in GUI/CLI/proof keeps MTA support alive across sessions and joined workers; create after QApplication/UI STA, destroy after sessions. Full Release 100 cycles passes (339.22 s, median handles 380 to 379); Debug rapid-close and fresh-owner 100 cycles each pass at 330 to 330. Debug/Release media 16/16, app 10/10, GUI startup and 8 stress-runner tests pass. API embedders must retain the same application owner. No global COM static; capture dispatch and system-module pin remain. Complete resource teardown, actual driver removal, production v2 coordinator, external gaming latency and distribution remain pending; normal media remains legacy. See latest refactor/CHECKPOINT-A.md.
