# Maintenance backlog

Only unfinished work belongs here. Completed milestones and historical evidence
remain in Git. The backend was accepted for personal use on 2026-09-19; deferred
qualification is not a prerequisite for routine frontend work. Do not restart a
broad acceptance campaign without a reported regression or an explicit request.

## Release process

The maintained build/publish scripts enforce a clean source commit, application
checks, package verification and signed updates. See [release instructions](../docs/release.md).

## Deferred qualification and known failures

Details and prior measured limits: [known limitations](../docs/known-limitations.md).

- [ ] Qualify fresh-machine installation; the owner confirmed an upgrade from 0.3.4
  on the second computer and the two-computer input/audio fixes for 1.0.0.
- [ ] Obtain Authenticode signing for future application/Setup binaries.

- [ ] Clean up the three test-created virtual devices, then qualify native
  multi-controller allocation/local-slot preservation and broader Xbox/PlayStation hardware.
- [ ] Investigate laptop hardware decode Section-handle growth and hardware encoder
  deadlines; retain the tested software compatibility option and fallback.
- [ ] Qualify capture-handle bounds, deferred WGC/RPC cleanup and sustained resources.
  Four-viewer software CPU and relative memory optimization remain deferred.
- [ ] Qualify physical mouse/keyboard confinement, HDR/adapters, unplug/driver loss,
  audio quality and external input/capture/A-V timing.
- [ ] Extend Internet/NAT/interface-change and real multi-colo service validation.
- [ ] Reconcile provider billing/hibernation and the 50% free-tier headroom target.

## Future work driven by reports

- [ ] Consider replacing the retired ViGEm runtime after validating signed-backend
  deployment costs and XInput-only game compatibility.
- [ ] Improve capture/codec/transport/presentation CPU and latency only with matched
  measurements; keep queues bounded and retain freshness/recovery tests.
- [ ] Revisit A/V drift correction and audio buffering if listening/diagnostics show
  audible catch-up or persistent sync problems.
- [ ] Keep title-bar, resize, fullscreen and native overlay behavior consistent
  across DPI/window states; improve actionable error text where reports show gaps.
- [ ] Consider UPnP/NAT-PMP only if actual direct-room failures justify it.
- [ ] Promote diagnostic options to normal controls only when users need them;
  keep diagnostic commands and richer overlays separate from the main flow.

Build inputs live in cmake/dependencies; durable maintenance docs live in docs/.
Historical legacy UDP-specific ideas do not imply new work on the retired transport.
