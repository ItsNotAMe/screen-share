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

## 1. Room-backed media session — active (B + C integration)

**Deliverable:** a reusable native session path and one-command headless scenario
that create/join real v2 rooms, negotiate actual media through authenticated room
messages, deliver paced frames/audio and stop cleanly.

Include owned networking, typed asynchronous operations/events, session/connection
generations, room-to-WebRTC adaptation, bounded candidate ordering, host/viewer
lifecycle, restart/rejoin and cleanup. Use shared production components. Extend
to four viewers with failure isolation; validate CLI-only and UI-capable builds.

Already available: codec/audio/capture peers, scheduled media owner, settings core,
isolated Worker/directory outbox, Qt transport/admission and native-to-workerd tests.
Actual authenticated four-viewer H.264/Opus negotiation now also passes against
local workerd, including slow-viewer isolation, twelve encrypted data channels,
ICE restart, kick/rejoin and room shutdown. The reusable RoomPeerNegotiation
adapter enforces candidate ordering and connection-generation barriers.
Negotiation completions and deadlines now run automatically on its signaling
thread, with cancellation-safe delayed callbacks and no recurring idle timer.
**Remaining:** embed this composition in the shared automatically scheduled session
facade, including roster-driven peer ownership and transport-failure recovery.
The diagnostic still owns orchestration; normal UI/CLI sessions remain legacy.

RoomNetwork now owns one dedicated Qt networking loop for admission and all room
sockets, with bounded commands/events and asynchronous cancellation. RoomPeerRoster
reconciles authenticated snapshot generations/revisions into peer lifecycle hooks.
The real-media proof uses both for creation, socket-loss retirement, reconnect,
kick/rejoin and close; it no longer pumps Qt events or manually attaches each peer.
Remaining facade work includes automatic cross-executor dispatch, asynchronous
capture-cleanup barriers, recovery budget integration and public session events.

The authenticated proof now uses RoomManagedPeer and HostPeerOwner for deferred
capture attachment, cleanup barriers and budgeted ICE restarts. BeginStop keeps
signaling responsive while capture callbacks drain; startup and shutdown waits
belong to the outer diagnostic, not signaling commands. Carry these integrated
owners into the normal facade and replace the remaining diagnostic dispatch.

RoomSessionCoordinator now automatically drains room events, dispatches them on
signaling, advances media lifecycle hooks and handles bounded asynchronous sends.
RoomSignalCodec provides shared SDP/ICE conversion. The four-viewer scenario no
longer pumps these operations, and recovery completes while the caller is idle.
MediaEngine now owns native worker/network threads, the factory, Opus setup,
independent peer creation, host track attachment and the three data-channel
policies. The authenticated scenario uses it with injected codecs/audio endpoints.
MediaPeer now owns each native peer's ICE lifecycle, negotiation, incoming video
sink, bounded/validated channels and idempotent teardown. Diagnostic observers
only collect frame/message evidence. RoomMediaSession now owns host/viewer
membership reconciliation, bounded authenticated signal routing, transport-loss
retirement and pending rejoin barriers. The scenario supplies native construction
hooks and uses the coordinator to advance these sessions automatically.
Next: expose owned admission/start/join/stop and session status through the normal
facade, including recovery and user-visible state. Public API adoption and complete
runtime ownership remain; the shared internal session is not yet UI/CLI cutover.
Do not add another event-pumping layer around the completed coordinator.

- [ ] Complete the integrated media-session deliverable and headless scenarios.

## 2. Complete user experience (B + D)

**Deliverable:** existing UI and CLI use the shared v2 backend, including capture/
audio selection, presentation, Auto/Manual/Gaming settings, saved nickname, live
directory, room policy and viewer diagnostics.

Include pending/applied/error settings, aggregate upload allocation, recovery,
profile persistence, room links, source changes and cancellation. Preserve appearance
and supported behavior. Do not change the default before required coverage exists.

- [ ] Complete UI/CLI adoption and settings/profile/presentation integration.

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
