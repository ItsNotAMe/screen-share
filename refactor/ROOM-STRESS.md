# Full-room lifecycle and continuous media testing

**Latest follow-up:** [RECEIVER-PACING.md](RECEIVER-PACING.md) identifies and fixes
audio capture overproduction that made A/V synchronization delay video. The
historical failed receive-rate runs below remain evidence of the original defect.
Current loopback validation also requires measured mean receiver buffering at or
below 100 ms; passing frame-rate recovery alone is insufficient.

The silent stress harness now keeps one native process alive across complete
room create/join/media/stop cycles. Each cycle has a host and four real H.264/Opus
viewers using the production RoomSession, networking/signaling executors, media
runtime and input port. Synthetic capture/audio and recording input sinks cannot
fall back to physical capture, playback or input injection.

```powershell
python scripts/test-room-lifecycle.py build/sdk-app-release build/webrtc/room-restarts-NEW
python scripts/test-room-lifecycle.py build/sdk-app-release build/webrtc/room-soak-NEW --cycles 1 --soak-seconds 7200
python scripts/test-room-lifecycle.py build/sdk-app-release build/webrtc/room-accounting-NEW --idle-seconds 60 --memory-accounting
python scripts/test-room-lifecycle.py build/sdk-app-release build/webrtc/room-slow-NEW --cycles 1 --soak-seconds 180 --slow-viewer --idle-seconds 30 --memory-accounting
```

Build `RoomLifecycleStress` first. The default is 100 restarts; soak duration is
explicit, bounded at 7200 seconds, and requires one cycle. A short run never counts
as the two-hour acceptance run. Output directories must be new.

## Contracts and evidence

- One persistent native process exposes accumulation across cycles. A fresh local
  production Worker fixture per cycle avoids changing admission quotas or adding
  privileged reset endpoints. This does not validate sustained service cost/rates.
- All four viewers must decode video and synthetic audio. Shutdown alternates
  host-first and viewer-first. Every runtime must be destroyed; video/audio
  callbacks must stop, and retained input ports must reject new requests.
  Each cycle also checks destruction of audio endpoints, capture sources/resources
  and 24 weakly observed dependencies, including all four presentation buffers.
  Weak references do not keep the tested objects alive. Synthetic capture resources
  use an owning wrapper, without copying pixels or collecting an unbounded history.
- Continuous runs check every five seconds that all viewers continue receiving
  valid video/audio and the host retains four healthy peers. They record capture
  handoff age separately from external latency. Each viewer reports its own video,
  audio and presentation counters; aggregate progress cannot conceal a stalled peer.
  Full decoder queue age and network impairment are not established here.
  Progress records also include the existing public sender/delivery snapshots
  (encode rate/time, bandwidth estimate, payload rate, loss/RTT, limiting reason,
  source drops and delivery replacement). Missing measurements remain null; reading
  this snapshot adds no signaling requests and changes no adaptation policy.
- Native stdout records cycle/progress/completion messages; resource records use
  a separate stderr stream. They must not be concatenated before parsing because
  separate pipe chunks can interleave within a resource JSON record. The first
  three-cycle trial exposed that harness bug; it is retained as failed evidence.
- The Python owner bounds process lifetime/logging and kills only its job/process
  tree. The service fixture adds progress and total deadlines. Logs are written
  during execution so an outer timeout preserves partial diagnostics. Binary,
  fixture and runner hashes accompany results. Missing/reordered samples, failed
  native exit, wrong binary identity and incomplete soak evidence fail closed.
- Restart runs of at least 20 cycles retain the original +8 median handle bound.
  Private memory and active-run resource trends are observations, not acceptance.
  The report explicitly leaves full soak acceptance false while memory, complete
  queue-age and impairment requirements are unverified.

The original public-session proof and the stress target share
`PublicRoomSessionFixture.h`; no second media implementation was introduced.

## Ownership, heap accounting and slow presentation

The stress target now consumes the actual UI/CLI `LatestRoomVideoFrame` buffer.
During `--slow-viewer`, viewer 0 takes one frame every 250 ms from active second
15 through 35; the other viewers keep consuming normally. The decoder callback
continues publishing into the one-frame buffer. Every sampled queue must satisfy
`received = consumed + replaced + pending`, with at most one pending frame.
Old frames must be replaced during impairment. Healthy viewers and recovered
viewers must sustain at least 70% of their own baseline rate; the impaired viewer
must fall below 50%. A maximum 36 presented frames/second for the fixed 30 fps source
rejects catch-up bursts, including the transition interval. This is presentation
impairment, not packet loss, congestion, decoder stalls or external latency proof.

`--idle-seconds` records separate post-stop samples (0..600 seconds). They never
replace immediate restart measurements. `--memory-accounting` performs read-only
Windows heap walks and virtual-memory classification at baseline, every ten cycles,
the final cycle, and every ten idle seconds including the final sample. It runs
after media shutdown, uses no heap compaction, and unlocks each heap before logging.
Unsupported/incomplete scans fail accounting validation; an outer process watchdog
still guards native calls that cannot be interrupted internally.

Heap busy/free bytes distinguish live allocations from heap space marked free.
VirtualQuery separately classifies private, mapped and image committed regions.
These are observations, not exhaustive allocation stacks: custom allocators and
image copy-on-write accounting can differ from process private bytes. Successful
weak-owner checks do not prove every internal library allocation was freed. Full
memory acceptance and the two-hour soak remain open; `soakAcceptanceComplete` stays
false. Schema 2 evidence now requires ownership, per-viewer progress and requested
idle/accounting records; historical schema 1 artifacts retain their earlier scope.
Heap free/region totals are not interchangeable with currently committed private
bytes: in these runs, heap free totals remain high even after VirtualQuery and
process counters show decommit. Do not subtract them to invent live process usage.

An initial diagnostic (`room-accounting-slow-release`) delayed the raw WebRTC sink
instead of presentation consumption. It produced a catch-up burst and extra memory,
but bypassed the app's existing bounded buffer. Its earlier harness pass is not
app slow-viewer evidence. The corrected scenario above reuses the real buffer;
no additional production queue or speculative memory cleanup was added.

## Verified runs — 2026-09-18

### Ownership/accounting follow-up

| Run | Result | Evidence under `build/webrtc/` |
| --- | --- | --- |
| Release, 100 rooms + 60 seconds idle | Ownership/accounting and handle bound pass; median handles 347 → 350 (+3) | `room-accounting-restarts-final-release/result.json` |
| Debug, 100 rooms + 60 seconds idle | Ownership/accounting and handle bound pass; median handles 347 → 349 (+2) | `room-accounting-restarts-final-debug/result.json` |
| Release, 180-second slow presentation alongside other media stress jobs | Failed final frame-rate recovery; retained as a failure, not acceptance | `room-accounting-presentation-release/result.json` |
| Release, isolated 180-second slow presentation | Also failed the unchanged final recovery threshold; competing tests cannot explain the entire issue | `room-accounting-presentation-isolated-release/result.json` |
| Debug, isolated 180-second slow presentation with sender snapshots | Passed the local isolation/recovery checks; this does not waive the Release failures or establish latency | `room-accounting-presentation-debug/result.json` |
| Release, isolated 180-second run with sender and remote receiver snapshots | Failed final rate recovery again; receiver buffering/drop growth recorded | `room-accounting-receiver-release/result.json` |

Every restart released all 24 observed dependencies and audio/capture endpoints;
peak shared capture resources was one (budget ten). Release live heap bytes were
1,617,687 at cycle 10 and 1,588,583 at cycle 100, while heap space marked free grew
105,446,608 → 115,417,312. Debug live heap bytes were 1,877,731 → 1,924,082 and free
heap space 88,823,168 → 117,335,776. This supports retained free heap space as a major
contributor to the variable process footprint, not a demonstrated growing set of
these application-owned objects. It is not exhaustive allocation attribution.

Immediate private-byte median growth remains visible: Release +16,302,080 and
Debug +99,954,688. Debug post-stop private bytes fell 127,209,472 → 19,066,880 over
60 seconds, whereas Release remained about 126.5 MB. Idle cleanup timing is not
guaranteed, and neither observation waives an immediate memory gate. No heap
compaction, allocator policy, bitrate floor or production queue was introduced.

Both configurations build and pass the original public-room input/media regression.
The Windows proof compile check exposed missing desktop/gamepad input objects in
its standalone link composition. Those existing production sources and `dwmapi`
are now linked into the proof support library; recording input remains explicitly
injected by the fixture. Both Windows proof configurations build again. No physical
input test is inferred from that compile check.
Fourteen Python lifecycle evidence tests cover missing counters/owners, invalid
timing, incomplete accounting, unbounded presentation, absent impairment and
recovery bursts. A failed concurrent-load run must not be silently replaced by an
isolated pass; controlled load/performance acceptance remains separate.

The isolated Release run retained the one-frame bound and continued audio/video,
but its final two samples included viewer rates below 70% of baseline (viewer 1:
20.39 and 20.38 fps). Initial recovery after the deliberate slowdown was faster;
later degradation must be investigated before a long soak can count as acceptance.
Sender/delivery and remote decoder snapshots have been added for that follow-up.
Do not lower the threshold, treat aggregate progress as sufficient, or attribute
the failure to resource contention without evidence.
The Debug run's baseline was about 29.3–29.7 fps, the impaired viewer about 3.93
fps, healthy viewers about 28.3 fps, and final recovery about 22.2–23.2 fps. It
passes the 70% threshold but still shows lower sustained rates. At active second
145 its source had delivered roughly 4,380 frames per viewer with zero source or
delivery drops, while presentation had received roughly 3,140–3,280. Sender
snapshots still reported 30 encoded fps, zero loss and 0 ms RTT. This narrows the
investigation beyond capture delivery; it does not identify the root cause.

The next investigation should compare each viewer's `sender.delivered`,
`sender.encodedFps`, `receiver.framesDecoded`/`decoderDrops`/`jitterBufferMeanMs`
and presentation `received`/`replaced` over time. Snapshot samples can be older
than the local presentation counters; compare trends, not exact equality across
different sampling clocks. Only presentation accounting uses an atomic snapshot
from the same buffer. `limitingReason` uses the public `VideoLimitReason` enum;
null remains unmeasured. These reports avoid credentials, SDP and addresses.

The final receiver-instrumented Release artifact is
`room-accounting-receiver-release/result.json`. At active second 165, remote
reports show 619–764 dropped frames and cumulative mean jitter-buffer delay of
1,398–1,436 ms across all four viewers. Source/delivery drops, NACK/PLI counts,
retransmissions and reported packet loss are zero at that sample; encoded rate is
29–30 fps and mean encode time about 0.35 ms. Presentation retains at most one
pending frame. These are internal receiver statistics, not external image latency
or proof of an MF decoder defect. The next group must diagnose receive buffering,
timing and synchronization before the two-hour/low-latency acceptance run. A
passing final rate ratio alone cannot override this buffering evidence.

### Earlier harness runs

| Run | Result | Evidence under `build/webrtc/` |
| --- | --- | --- |
| Release complete rooms, 100 cycles | Passed; median handles 344 → 349 (+5) | `room-stress-final-restarts-release/result.json` |
| Debug complete rooms, 100 cycles | Passed; median handles 343 → 348 (+5) | `room-stress-restarts-debug/result.json` |
| Release four-viewer continuous media, 180 seconds | Passed functional/progress checks; active median handles −6, private bytes +4,005,888 | `room-stress-soak-release/result.json` |
| Debug four-viewer continuous media, 30 seconds | Passed short-run functional/progress checks | `room-stress-final-soak-debug/result.json` |

Release/Debug builds and the original public-session input/media CTest both pass
after fixture extraction. Five lifecycle evidence tests and the existing seventeen
capture evidence/watchdog tests pass. The later log-durability change was exercised
by the Debug 30-second run; earlier artifacts retain their own fixture hashes.

Restart memory does **not** pass acceptance: final Release/Debug median private-byte
growth was 103,481,344 / 91,807,744 bytes, while their final individual samples fell
to 35,110,912 / 26,099,712 bytes. This variability requires lifetime/allocation
accounting; selecting only the low final value would hide it.

## Longer capture investigation

The production capture-owner proof now supports up to 1000 cycles, while ordinary
direct-capture modes retain their 100-cycle limit. A 500-cycle Release run completed
without native failure, but **failed the unchanged +8 handle bound**: median handles
300 → 526 (+226), peak 558, final 523. Private bytes grew from median 62,570,496 to
77,459,456 (+14,888,960), with a peak of 79,089,664.

Artifact: `room-stress-memory-500/result.json`, 486.97 seconds. Handles were roughly
stable for the first 175 cycles, then grew approximately one per cycle before
some later releases. Thus the earlier 100-cycle passes did not close the resource
problem. Memory did not grow linearly over this run, but allocation ownership is
still unproven. Keep the MTA lease, dispatcher and module-pin mitigations intact.

Debugger evidence: `room-stress-htrace-corrected.log` snapshots cycles 50–55 and
reports six outstanding new handles, across the cycle worker thread IDs. Its
allocation stacks are empty, so it does not identify the allocator. The earlier
`room-stress-htrace.log` used hexadecimal cycle constants unintentionally and
never took the intended snapshots; it is not allocation evidence. A subsequent
global CreateEvent breakpoint attempt (`room-stress-event-callers-console.log`)
exceeded the native first-cycle deadline and produced no useful correlation.
Neither diagnostic run is an acceptance pass. Further tracing must avoid adding
breakpoint overhead before the target interval; do not weaken production deadlines.

Follow-up: [CAPTURE-HANDLES.md](CAPTURE-HANDLES.md) identifies the reproduced
retained handles as WGC/RPC ALPC ports and records a repeatable typed trace,
rejected apartment/thread-reuse hypotheses and separate post-stop observations.
The explicit RPC cleanup experiment also fails the unchanged immediate bound;
later OS releases cannot replace restart samples. No new production fix is claimed.

Remaining: complete capture resource acceptance and full allocation/queue-age
acceptance, then run the full two-hour four-viewer soak and network/decoder
impairment tests. Physical media/input, TLS/NAT, external latency, service cost,
matched legacy comparison and default cutover remain separate open gates.
