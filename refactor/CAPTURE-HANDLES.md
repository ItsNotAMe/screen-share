# Delayed capture handle investigation

The longer production-owner test fails the existing immediate post-cycle handle
bound. The surviving handles have now been identified as Windows RPC **ALPC
ports** created by WGC's `CapturableItemStatics::TryCreateForWindow`. This is a
different allocation path from the earlier `OXIDEntry::Initialize` event problem.
Keep the application MTA lease, owned dispatcher and GraphicsCapture module pin.
No new production lifetime change is justified by the experiments below.

## Verification — 2026-09-18

| Run | Outcome | Evidence under `build/webrtc/` |
| --- | --- | --- |
| Reusable Debug typed trace, cycles 30–35 | Complete; five retained ALPC ports correlated by handle, opening thread and successful RPC return stack; one uncorrelated file handle also reported | `room-stress-port-trace-final/result.json` |
| Release production owner, explicit RPC cleanup, 500 cycles + 180 s idle | Native exit 0; immediate handles 304 → 558 (+254), peak 561: **bound fails**. Idle samples decline 558 → 385, without changing that failure | `room-stress-rpc-cleanup-500/result.json` |
| Release direct capture on one reused thread, 100 resize/rebuild/readback cycles | Native exit 0; handles 334 → 425 (+91), final 427: **bound fails**. Merely reusing the capture thread does not solve the reproduced growth | `room-stress-fixed-thread-100/result.json` |
| Release production owner, unchanged RPC policy, 100 cycles + 360 s idle | Native exit 0; immediate handles 304 → 396 (+92), final 398: **bound fails**. Idle handles reach 295 at 300 s and 289 at 360 s, below warm-up | `room-stress-default-idle-100/result.json` |

The 500-cycle experiment plateaus near 559 handles for more than 200 cycles.
Private bytes peak at 86,597,632 and fall to 37,761,024 during idle. These observations
support delayed runtime cleanup rather than indefinitely linear retention, but are
not a pass of the unchanged immediate +8 bound or full memory acceptance. The
experiment's `rpcIdleCleanupExperiment` field and native `CAPTURE_OPTIONS` both
record the enabled policy; do not treat it as ordinary application behavior.
The normal-policy run also releases the reproduced excess during idle, without
calling the cleanup API. This accounts for delayed RPC retention in this
reproduction; it does not prove every driver/runtime version behaves identically.
That run's private bytes fall from 72,425,472 immediately after the last cycle to
18,231,296 after 360 seconds; the warm-up median was 58,195,968. This is capture
process evidence, not an explanation of the separate full-room memory variability.
No extra MTA thread, persistent capture thread or RPC policy change is warranted.

Release/Debug proof builds and all three Python CTests pass (21 capture
evidence/watchdog tests, six trace tests and five room-evidence tests). The trace
runner was exercised against the actual Debug proof, including process-tree
cleanup. The long runs are silent, use only generated windows and do not inject
physical input. No application backend code was changed in this investigation.

## Allocation evidence

Artifacts are under `build/webrtc/`:

- `room-stress-handle-types.log`: cycles 30–35 leave five new ALPC ports plus one
  file handle. Type inspection rules out treating every retained handle as an event.
- `room-stress-port-callers.log`: all five surviving ports (`a44`, `908`, `ed0`,
  `7b0`, `fe8`) match successful `NtAlpcConnectPort` returns through
  `RPCRT4!LRPC_CASSOCIATION::AlpcConnect`, COM negotiation and WGC's
  `CapturableItemStatics::TryCreateForWindow`, called by the production adapter.
- `room-stress-anchor-ports.log`: a test-only persistent explicit WinRT MTA
  apartment still leaves five ports in the same interval. That experiment was
  reverted. Do not add an extra runtime thread/apartment as a speculative fix.

The earlier CreateEvent-return matches were inconclusive: handle numbers can be
reused after close. Allocation correlation requires the live object's type and
the opening thread, not just matching a hexadecimal number. `!htrace` reports
surviving handles here but its allocation stacks are empty; explicit port-return
breakpoints supply the missing call stacks. Debugger timing is diagnostic evidence,
not representative performance or a resource acceptance pass.

## Repeat the trace

Build `WindowsCaptureLifecycleTest` in the Debug proof configuration, then run:

```powershell
python scripts/trace-capture-handles.py build/sdk-proof-debug/WindowsCaptureLifecycleTest.exe build/webrtc/capture-port-trace-NEW
```

The script launches only the named proof with its matching PDB; it never attaches
to an existing process. The default snapshots are cycles 30 and 35. It disables
allocation breakpoints until the first snapshot, uses explicit decimal cycle
constants, filters to the capture owner and protects the return breakpoint against
other threads. A process-job watchdog bounds the entire debugger/proof tree and
console log. Microsoft symbols are cached in `build/symbols`. Use `--cdb` for a
different x64 Windows SDK debugger installation.

`result.json` records executable/PDB/command hashes, typed outstanding handles and
port-return stack correlations. The proof is deliberately terminated at the final
snapshot: `traceComplete` means the diagnostic completed; `acceptanceRun` is always
false. A completed trace with unmatched handles still needs investigation.

## Observe cleanup without hiding the failure

```powershell
python scripts/stress-live-capture.py build/sdk-proof-release/WindowsCaptureLifecycleTest.exe build/webrtc/capture-idle-NEW --production-owner --cycles 500 --idle-seconds 360 --max-handle-growth 8 --timeout 1200
```

After all capture owners, test windows and retained frames are destroyed, the
process remains alive for the requested bounded observation. `CAPTURE_IDLE`
samples are stored separately from `LIFECYCLE` samples. The original median
comparison (cycles 6–10 versus the last five cycles) is unchanged, even when idle
handles later return to baseline. Missing, malformed or truncated idle observations
fail the run; null counters and nonfinite or insufficient elapsed times cannot pass.

`--rpc-idle-cleanup` is an explicit **test-only experiment**. It enables the
documented process-wide [RPC idle cleanup API](https://learn.microsoft.com/en-us/windows/win32/api/rpcdce/nf-rpcdce-rpcmgmtenableidlecleanup)
before capture. Windows may also enable this cleanup automatically after resource
thresholds are reached. The API enables periodic cleanup, not immediate release
after each capture. The application does not call it, and this option must not be
silently added to acceptance commands. The evidence records the experimental mode.

Remaining acceptance: immediate handle stability, restart memory accounting,
two-hour continuous four-viewer media, impairment, physical devices, remote
TLS/NAT, latency and the matched legacy comparison remain open.
Continue with full-room memory/soak and impairment work rather than repeating
these allocation traces. Keep the immediate capture failure visible; closing or
revising that gate requires separate evidence and an explicit acceptance decision.
