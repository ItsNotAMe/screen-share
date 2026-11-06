# Maintenance backlog

Only unfinished work belongs here. Completed milestones and historical evidence
remain in Git. The backend was accepted for personal use on 2026-09-19.
Validation and qualification tasks are excluded at the owner's request. The
laptop is no longer a test target; investigate its reported issues in code.

## Release process

The maintained build/publish scripts enforce a clean source commit, application
checks, package verification and signed updates. See [release instructions](../docs/release.md).

## Code investigations and release preparation

Details and prior measured limits: [known limitations](../docs/known-limitations.md).

- [ ] Obtain Authenticode signing for future application/Setup binaries.
- [ ] Investigate hardware decoder Section-handle lifetime and hardware encoder
  output deadlines in code; preserve the software compatibility option and fallback.
- [ ] Investigate capture resource ownership and deferred WGC/RPC cleanup in code.
- [ ] Reduce four-viewer software CPU and memory usage.

## Future work driven by reports

- [ ] Consider replacing the retired ViGEm runtime; account for signed-backend
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
