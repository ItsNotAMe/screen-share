# Full-room lifecycle and continuous media testing

The silent stress harness now keeps one native process alive across complete
room create/join/media/stop cycles. Each cycle has a host and four real H.264/Opus
viewers using the production RoomSession, networking/signaling executors, media
runtime and input port. Synthetic capture/audio and recording input sinks cannot
fall back to physical capture, playback or input injection.

```powershell
python scripts/test-room-lifecycle.py build/sdk-app-release build/webrtc/room-restarts-NEW
python scripts/test-room-lifecycle.py build/sdk-app-release build/webrtc/room-soak-NEW --cycles 1 --soak-seconds 7200
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
- Continuous runs check every five seconds that all viewers continue receiving
  valid video/audio and the host retains four healthy peers. They record capture
  handoff age separately from external latency. Full decoder/presentation queue
  age, slow-viewer isolation and network impairment are not established here.
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

## Verified runs — 2026-09-18

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

Remaining: identify/fix or account for this delayed capture handle retention,
explain restart memory variability, then run the full two-hour four-viewer soak
and impairment tests. Physical media/input, TLS/NAT, external latency, service cost,
matched legacy comparison and default cutover remain separate open gates.
