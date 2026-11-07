# Live pipeline

The modular room backend is the normal UI/CLI route. Capture publishes owned
frames, WebRTC handles media transport, and viewers present through the shared
native D3D surface. See [runtime ownership](../docs/architecture.md) and
[known limitations](../docs/known-limitations.md). Historical gate/checkpoint
notes no longer describe current routing. Do not remove production capture,
codec or input code merely because the legacy diagnostics also use it.

2026-11-07 software encoder workers: the WebRTC adapter requests at most two
slice workers through 1080p, or one when only one core is available. Larger
pictures retain automatic threading; hardware and low-level callers' default
policy remain. Set the count before MF format negotiation to size the pool,
then reapply afterward because negotiation resets the reported property. Setting
only afterward changes the query without reducing the pool/bitstream in the
tested inbox codec. Diagnostics record encoder-software-workers (zero means
automatic/unavailable). Rate/FPS restarts, resume and software fallback reuse the
configuration. See docs/performance.md for CPU, quality and timing evidence;
this exchanges encoding parallelism for lower CPU consumption, not uniformly
lower latency or identical pixels.

2026-11-07 host audio stalls: initial, established and manually selected System/
Microphone producers now expire after five seconds without fresh PCM, even when
the endpoint reports no error. Fresh queued PCM is consumed before expiry and
all-zero PCM counts as delivery; replacement filler silence cannot renew it.
Cancellation/retirement and pinned-target retries reuse the existing capture
owner and 250 ms..5 s backoff. Shared-source inner recovery and explicit-process
policy remain. Supplied-clock tests exercise startup/established/selected stalls,
normal silence, delayed owners, exact timeout/retry boundaries and teardown.
No physical device-stall or CPU/latency improvement claim.

2026-11-07 minimized restore: minimization now rearms CaptureSession's existing
first-frame watchdog after previously healthy output. The deadline keeps moving
while minimized; after restore a valid sample clears it, or missing output times
out (5 s in production). A switchable source releases failed capture and waits
for explicit replacement while its session stays alive; an ordinary source
fails. Tests cover both paths, long minimization, stale-pixel suppression,
successful restore and replacement after timeout. This is a watchdog fix,
separate from recovery-budget renewal; no physical WGC failure was injected.

2026-11-07 capture recovery: the three-rebuild allowance renews after 30
consecutive valid owned capture samples. Device loss, GPU frame skips,
minimization and source unavailability interrupt the healthy streak. Pending
rebuilds, empty polls and elapsed time cannot earn credit. Retirement, cumulative
generations, 250 ms cancellable backoff and terminal reconstruction/unstable-
capture handling remain. A new real-owner synthetic regression fails with the
old lifetime budget on the fourth loss and passes five healthy-separated losses;
exact 29/30-frame and interrupted/pending/terminal boundaries are application
checks. No physical driver-loss or capture handle-lifetime qualification.

2026-11-07 direct decoder crop: cropped software output copies visible rows from
the locked MF buffer directly into owned pixels, eliminating the full coded-frame
temporary vector/copy. A guard unlocks on validation/allocation failure. Uncropped
output and hardware textures retain their paths. Tests add uncropped 720p among
cropped/resized/retained frames. Six matched AB/BA pairs against 850fd02 give
four-decoder median CPU 6.952→6.104 ms/picture (−12.2%) and elapsed 4425→3708 ms
(−16.2%), with every four-decoder pair improving. Single-decoder CPU is noisy;
elapsed improves 16.4%. Full-frame checksums/byte counts match. See
docs/performance.md and its tracked raw runs for scope: decode/copy/crop/checksum,
not total app CPU, memory or end-to-end latency. Hardware handle lifetime remains
unqualified.

2026-11-07 presentation recovery: three rebuild attempts now renew after 30
successfully presented incoming frames, replacing the session-lifetime cap.
FramePresentationSession passes whether work contains incoming pixels; Update/
redraw success and skipped output cannot earn health credit. A device fault
clears the healthy count. Recovery totals stay cumulative until Clear, while
four rapid/short-recovery faults remain terminal with the existing 250 ms
drop-only backoff. Five isolated outages and exact 29/30-frame boundaries are
checked with supplied time; the shared session checks redraw/skip isolation.
The native Windows UI proof also covers four healthy-separated injected losses
with actual GPU resources. Physical driver loss/hangs remain unqualified.

2026-11-07 completed software-encode stalls: producing one valid packet after
more than 500 ms no longer marks that encoder failed. Drop late output, preserve
the live transform/latest pending frame and force the next fresh picture to an
IDR so it cannot depend on the missing reference. Cancellation suppresses late
callbacks and diagnostics. Actual codec errors and asynchronous hardware timeout
fallback retain their behavior. A real MF regression with three 550 ms input
mapping delays requires fresh independent-decodable keyframes, original timing/
mapping, exact pending replacement counts and no hidden transform restarts.
Release during a fourth stall and reinitialization are covered. This does not
preempt stuck COM/driver calls or qualify the physical game-lag report.

2026-11-06 software decoder storage: H264StreamDecoder keeps one caller-owned
CPU output buffer, resetting its length and creating fresh sample metadata on
each poll. Output vectors remain independent; stream change invalidates storage,
growth stays within the existing 128 MiB limit, and Stop frees it. MFT-provided
GPU samples retain their path. Real software tests retain frames across changing
pixels, 1080 crop, resize and Stop/restart. Six matched AB/BA pairs reduce elapsed
decode workload time by 6.5% (one decoder) and 10.4% (four); CPU medians improve
8.4%/7.5% but paired ranges include regressions. Warmed large-buffer allocations
fall from 1921/7684 to zero for 960 pictures per decoder. See docs/performance.md
for raw evidence, scope and retained scratch-memory tradeoff. No full-session
latency, total application CPU or hardware handle-lifetime claim.

2026-11-06 controller lag: a 300 ms input gap now neutralizes through Sink::Pause
without dropping granted capabilities or native controller allocation. The host
rotates the permission epoch with the existing Permission message, clears pre-gap
queues and resumes only on fresh input; heartbeat alone leaves the sink paused.
Client cache clearing and epoch-bound poller recreation prevent stale replay.
Fresh queued input is processed before timing out a delayed owner iteration.
Concurrent revoke wins over late Pause completion. Pause failure falls back to
revocation. DesktopSink separates identity health from stale captured geometry:
staleness releases held keys/discards input, while fresh geometry permits new
input again; source invalidation still revokes. Host UI keeps icons checked and
shows Input paused; diagnostic JSON includes paused. Physical game/driver lag
is not reproduced by the injected owner, reader, sink and runtime-gap proofs.

2026-11-06 capture failure isolation: a switchable source may abandon failed
native state on its capture owner and wait for an explicit replacement while
the room/distribution/peers stay alive. Read errors, rebuild failure, exhausted
recovery and first-frame timeout preserve their failure reason in SourceUnavailable.
Abandonment advances the source generation and clears the retry policy only for
the next explicit source; it never reopens the failed device. Fresh output clears
the failure. Consumer failures, ordinary unwrapped sources and initial Start
failure remain terminal. Runtime disables grants/releases held input on entering
SourceUnavailable and restores grant availability after capture resumes, without
implicit regrant. Host copy distinguishes capture failure from window closure.
Injected four-viewer media and two-viewer UI tests require audio continuity,
explicit video replacement and unchanged room/connection identity. Physical
driver removal/hang and deferred native cleanup remain unqualified.

2026-11-06 decoder recovery: the three-software-rebuild budget now renews after
30 validated output frames instead of exhausting across the entire configuration.
Failures reset the healthy count; accepted input without output cannot renew it.
Retention-pressure drops count as successful decoding. The keyframe gate, 250 ms
backoff and session hardware quarantine remain. Deterministic output-validation
faults cover five healthy-separated recoveries and repeated 29-frame unstable
recoveries that still exhaust the budget. DecoderAdapterTests is now a regular
application CTest; generated GPU checks remain explicit. This does not establish
a fix for the laptop's Section-handle growth or hardware output deadlines.

2026-11-06 direct NV12 planes: the software adapter now calls EncodeNv12 with
bounded plane views, copying directly into the owned MF input sample. The
temporary per-frame packing vector is gone. Input geometry/strides/minimal last
rows are validated before consuming frame state; I420/hardware paths remain.
Matched 1080p software-encoder timing against 05582cc (six AB/BA pairs, 480
measured frames after 30 warmup frames) reduces elapsed time 8.9–17.7%, including
four-GPU-input encoders 8.486→7.571 ms/input batch (10.8%). CPU-time results are
mixed/noisy and do not establish general CPU savings. Outputs retain matching
byte counts/hashes. See docs/performance.md and its tracked raw evidence. Timing
excludes capture/network/decode/presentation; no full-session scorecard was rerun.

2026-11-06 NV12 software input: MfVideoEncoder consumes CPU NV12 and shared GPU
NV12 readback without the former NV12-to-I420-to-NV12 round trip. Readback uses
the first requested format; I420-only callers retain direct conversion and do
not eagerly retain a second buffer. Requesting both formats derives the second
from owned CPU pixels without another GPU readback. Device retirement rejects
new maps, including cached ones. CPU preview materialization uses NV12 too.
The real four-software-encoder test decodes 32 padded/changing NV12 inputs with
zero ToI420 calls (32 before); generated GPU input uses eight shared readbacks
and zero I420 conversions. NV12 packing/MF input copies remain; this is operation
count evidence, not a new total CPU/memory/latency scorecard.

2026-11-06 shared scaling: D3dVideoFrameBuffer retains at most four successful
scaled outputs keyed by all six canvas/image geometry values, under its existing
mutex. Shared capture resources let matching viewer tracks reuse immutable GPU
pixels and their lazy CPU readback. Tracks, adaptation, mapping/preset metadata
and encoding stay independent. Failed/busy scales are not cached; retirement or
scaler quarantine bypasses cached output. Eviction does not invalidate consumer
references. Do not add a cross-frame cache or overwrite published textures.
The explicit generated-texture StreamSettingsTests --gpu comparison reduced
four-viewer 1080p-to-720p scales/readbacks from 96/96 to 24/24 over 24 frames.
This is operation-count evidence, not a new full-session CPU/memory scorecard.

2026-09-25 multi-viewer report: a stalled connection was still processing capture
frames (including shared-device scaling). NativeRoomRuntime now gates per-viewer
delivery until negotiation, connection and initial settings are ready; disconnect,
failure, video pause and shutdown close the gate. The real-peer synthetic isolation
test fails with the old delivery behavior and passes with the gate. This is a
candidate fix for the reported FPS drop, not a reproduction of the user's WAN/GPU
slowdown. Reports retain up to 63 retired connection snapshots (no SDP/addresses),
candidate counts, negotiated state, OS version and source GPU-drop counters.
The host/viewer UI exposes terminal connection failures instead of waiting forever.

2026-09-26 diagnostics expansion: see docs/diagnostics.md for bounded startup,
failure and measurement histories, correlation IDs and redaction. Production
Windows peers get default STUN discovery when unconfigured. Decoder low-latency
ICodecAPI support is optional on older implementations; supported-but-failing
properties retain errors. Full ICE+DTLS state controls connection readiness.
ICE restart success also reconciles lifecycle when the aggregate transport stays
connected and emits no new connected callback. Windows 7 and the reported WAN
pair still need verification on those machines.
