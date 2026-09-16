# Backend v2 — delivery milestones

The approved [PLAN.md](PLAN.md) remains authoritative. The exhaustive checklist and
historical evidence are preserved in [DETAIL-CHECKS.md](DETAIL-CHECKS.md). Individual
checks are implementation details, **not separate user turns**.

## Working agreement

Native code is organized into root-level `backend/` and `frontend/`; the room
service remains `signaling-worker/`. Backend targets must not depend on frontend
headers. Folder organization does not imply the legacy runtime has been replaced.

- Continue through an end-to-end deliverable, including integration, meaningful
  failure tests, documentation and logical commits.
- A helper, interface, isolated test or successful build is not a stopping point.
- Intermediate commits are checkpoints within a work batch; keep working afterward.
- Preserve every original security, latency, resource and cutover gate. Never mark
  hardware/remote tests passed using localhost evidence. Report unfinished scope.

## 1. Room-backed media session — complete for local integration

**Deliverable:** a reusable native session path and one-command headless scenario
that create/join real v2 rooms, negotiate actual media through authenticated room
messages, deliver paced frames/audio and stop cleanly.

The public RoomSession now owns admission, networking/signaling executors,
authenticated membership/signal routing, status and asynchronous shutdown.
NativeRoomRuntime composes capture, per-viewer sources, native peers, negotiation,
initial stream preferences and the shared restart/retirement budget.
WindowsRoomRuntimeFactory binds WGC capture and WASAPI audio, selects the capture
device before constructing hardware codecs, and retains device-retirement fallback.

The public headless scenario uses the production runtime with synthetic endpoints.
It validates four independent H.264/Opus viewers, authenticated restart, fresh
rejoin, admission cancellation, media-drain barriers, capture startup failure and
single-viewer delivery-failure isolation. The Windows variant also passes using
a generated WGC window and synthetic audio, without physical input. Application
and CLI builds link the same native adapters; no diagnostic runtime implementation
is needed by the backend.

Evidence and scope: [CHECKPOINT-B.md](CHECKPOINT-B.md) and
[HEADLESS-TESTING.md](HEADLESS-TESTING.md). This closes local composition/integration,
not latency, resource, NAT/TLS, service-cost or cutover acceptance. Normal UI/CLI
sessions remain legacy until milestone 2 adoption and the required acceptance gates.

- [x] Complete the integrated media-session deliverable and headless scenarios.

## 2. Complete user experience (B + D)

**Deliverable:** existing UI and CLI use the shared v2 backend, including capture/
audio selection, presentation, Auto/Manual/Gaming settings, saved nickname, live
directory, room policy and viewer diagnostics.

Include pending/applied/error settings, aggregate upload allocation, recovery,
profile persistence, room links, source changes and cancellation. Preserve appearance
and supported behavior. Do not change the default before required coverage exists.

- [ ] Complete UI/CLI adoption and settings/profile/presentation integration.

Integrated foundation: the public session accepts bounded host live-settings
commands and exposes per-peer applied/source-observed revisions and rejection.
The production runtime updates existing senders and gives joining peers the latest
preferences. Four-viewer headless coverage exercises resolution/FPS/bitrate changes,
restart/rejoin and invalid/unauthorized/stopped commands. UI/CLI adoption, capture
reconfiguration, aggregate upload allocation and presentation remain in this batch.

## 3. Gaming controls end to end (D)

**Deliverable:** authorized mouse/keyboard/gamepads over encrypted data channels,
with consent/revoke/confinement, generation/sequence checks, bounded queues and
watchdog neutralization. Headless input targets only a test-owned sink.

Validate input isolation and input-to-frame response under media pressure; preserve
installer-managed drivers and the three-pad/local-slot policy.

- [ ] Complete input integration and adversarial/headless control scenarios.

## 4. Stability, performance and service acceptance (B + C + E)

**Deliverable:** reproducible evidence against the original targets and a matched
legacy/v2 comparison, with failures resolved or explicitly blocking cutover.

Include impairment/slow-viewer/device recovery, 100 start/stops, two-hour four-viewer
soak, current capture handle-growth failures (+76/+10; bound 8), queue pressure,
hibernation, service caps/cost/headroom, real TLS/NAT and external gaming image/input
latency. See [COMPARISON.md](COMPARISON.md).

- [ ] Pass implementation-side stress and service/resource acceptance.
- [ ] Complete required real-machine/network/external latency measurements.

## 5. Cutover, removal and release readiness (E)

**Deliverable:** validated default v2 behavior and removal of obsolete UDP, polling,
adaptation, NAT invite and old runner code; preserve useful platform/security tests.

Include upgrade behavior, separate namespaces, dependency/runtime packaging,
installer/fresh-machine checks and documentation. Deployment/publishing remain
separate authorized actions.

- [ ] Complete safe cutover and legacy cleanup after acceptance.

## Foundation and evidence

Gate A passed its original integration/build criterion; resource/performance
acceptance remains open. Native Qt clients pass actual workerd protocol integration.
Neither fact means normal sessions have switched to v2.

- [CLOSEOUT-A.md](CLOSEOUT-A.md)
- [CHECKPOINT-B.md](CHECKPOINT-B.md)
- [CHECKPOINT-C.md](CHECKPOINT-C.md)
- [HEADLESS-TESTING.md](HEADLESS-TESTING.md)
- [DETAIL-CHECKS.md](DETAIL-CHECKS.md) — original checks, evidence and deferred work
