# Refactor backlog

## Deferred acceptance — user decision, 2026-09-19

The user accepts the measured backend for current personal use and authorizes
moving to cutover/frontend work. These items no longer block that transition.
They remain unresolved or unverified, not falsely marked passed. Do not reopen
them as mandatory acceptance work without a concrete reported regression or
an explicit request.

- Real multi-controller driver allocation/local-slot qualification; other Xbox
  and PlayStation hardware. Preserve the existing working input implementations.
- Laptop hardware decode Section-handle growth and hardware encoder deadline;
  retain the verified software compatibility option and existing fallback.
- Immediate capture-handle bound and deferred WGC/RPC cleanup; sustained resource
  qualification and four-viewer software CPU optimization.
- Physical mouse/keyboard confinement, HDR/adapters, unplug/driver-loss, audio
  quality and external capture/input/A-V timing measurements.
- Internet/NAT/interface-change coverage beyond the measured LAN and packet model.
- Provider billing/hibernation reconciliation and the stricter 50% free-tier
  headroom target. Existing cost controls remain implemented.

Evidence stays in STAGE-2-4-ACCEPTANCE.md, DETAIL-CHECKS.md and their linked reports.
The fair normal-load and matched impairment comparison is completed, not deferred.
Cleanup of the three test-created virtual devices requires administrator commands
in CONTROLLERS.md; that housekeeping is distinct from future driver qualification.

## External release preparation

Client cutover is complete for current use. Public service migration/publication,
fresh-machine installer qualification, remaining distribution notices/source
obligations and release-channel coordination remain separate future work.
The isolated v2 service and existing updater selection are unchanged. These
release tasks do not block the frontend redesign.

## Multi-viewer memory

Deferred at the user's request on 2026-09-18. Matching legacy's private-memory
footprint is not required for cutover. The latest reference fixture (one host and
four local receivers) measures approximately 522 MiB hardware / 924 MiB software
private memory, versus legacy's 398 / 500 MiB. This is not total GPU allocation,
whole-system memory, or evidence of sustained growth.

Later work may reduce per-peer codec resources and evaluate bounded shared
encoding for viewers with identical settings. Preserve independent congestion
adaptation and source/frame ownership. Any optimization still needs a fair
latency/throughput comparison. Do not replace this with an arbitrary lower-memory
requirement that delays the frontend refactor.

The original resource acceptance requirements are preserved in the evidence;
their remaining qualification is now deferred by the newer decision above.
Known failures must remain visible when revisiting hardware support.
