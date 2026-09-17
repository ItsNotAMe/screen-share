# Gaming input transport and safety

The shared v2 input backend is implemented and exercised through actual encrypted
WebRTC channels with four simultaneous media viewers. **Stage 3 is not complete:**
controller control is integrated in the normal opt-in UI/CLI with explicit consent
and a lazy Windows sink; mouse/keyboard injection remains disabled pending source
mapping and confinement. See [CONTROLLERS.md](CONTROLLERS.md).

## Implemented contract

- `backend/input/v2/InputProtocol.h` owns portable events and explicit big-endian
  serialization. `SIN`, version 1, kind and connection-ID length precede 64-bit
  permission and sequence numbers, the negotiated connection ID, and an exact
  kind-specific payload. Messages are at most 162 bytes. Unknown versions/kinds,
  wrong lengths, invalid coordinates, key/button ranges and reserved gamepad bits
  are rejected. Pad slots are assigned by the host, never supplied over the wire.
- `InputService` owns one input thread per runtime. It sleeps when no grant needs
  processing; active processing is paced at 4 ms. Device callbacks do not execute
  in a renderer, media worker, HTTP callback or statistics loop. Sink methods must
  be bounded/nonblocking; this does not preempt a hung native driver.
- `RoomSession::Input()` exposes a retainable, thread-safe `input::Port`. It is null
  before native startup. `Request` asks for permission; only the host's explicit
  `Grant` authorizes it. Boolean results mean local acceptance, not remote consent
  or injection. `Read` exposes readiness, requested/granted capabilities,
  permission epoch, reason and applied/rejected/coalesced counts. Retained ports
  reject submissions after shutdown.
- `InputChannels` binds authenticated peer IDs to current negotiated connection
  IDs. Reliable/ordered control carries requests, permissions, release, keys,
  mouse buttons and wheel events. Unordered/no-retransmission input-state carries
  pointer, complete pad state and heartbeats. Telemetry keeps its own observer.
  The pre-existing diagnostic `options.channel` override retains exclusive channel
  ownership; it bypasses this service and must not be used by normal frontends.
- Connection changes/disconnection invalidate permission. Permission epochs are
  host-issued and monotonic across that runtime; old grants cannot inject.
  Reliable events have one sequence space; pointer, pad and heartbeat each have
  separate sequence high-water marks, so reordering across replaceable kinds
  cannot suppress unrelated input.
- The application bounds each peer's combined incoming/outgoing reliable queues
  to 64 messages (under 16 KiB). Overflow revokes rather than losing only key-up.
  SCTP gets one bounded batch only when its reliable buffer is empty. Persistent
  reliable backpressure for 100 ms revokes, even when the application queue is
  already empty. Failed reliable sends also revoke. Replaceable states are
  coalesced and dropped under transport pressure, never replayed as a backlog.
- Viewers resend complete pad state and a heartbeat every 100 ms while granted.
  A separate selected-device poller samples controllers at up to 250 Hz. Native channel draining
  uses the existing 5 ms signaling schedule; 250 Hz is a ceiling, not a promised
  measured delivery rate. Input application/watchdog operation is independent of
  that schedule.
- A 300 ms host watchdog invalidates the grant before draining late input.
  Revoke, disconnect, source invalidation and backend failure release held input
  on the input owner (normally within its next 4 ms tick). No automatic regrant.
  A failing partial backend grant is also released. Shutdown joins that owner.
- Mouse and keyboard ownership are exclusive per capability. At most three pad
  grants are allowed, reduced to `min(3, 4-localPads)`. `Configure` invalidates
  existing grants before changing this policy. The Windows sink also enumerates
  occupied XInput slots and verifies each created device's actual user index.
- Window-source runtimes refuse keyboard grants. A capture-switch request
  invalidates all input and restricts fresh grants to gamepads, including after a
  failed switch. Mouse/keyboard remain disabled until transactional replacement-
  source mapping is implemented. Invalid source validation does not
  change grants. There is no fallback to physical injection in tests.

## Remaining integrated delivery group

Controller portions of items 1, 3, 4 and 5 below are now implemented, including
UI/CLI factories and recording-device tests. Treat those as foundations; the next
group is mouse/keyboard geometry, confinement and full frontend integration.

Build the Windows device adapters and normal UI/CLI interaction together on this
port; do not introduce another transport, permission service or room workflow.

1. Implement a per-runtime Windows `Sink`, reusing the useful `RemoteInputInjector`,
   `VirtualGamepadBackend`, `ViewerGamepad` and XInput/PlayStation report primitives.
   Release must affect only that peer's held devices. Never install/repair drivers
   from the runtime; a missing driver disables gamepads alone. Detect local slots,
   backend failures and unplug, and neutralize before releasing allocations.
2. Bind input geometry to actual captured source identity and the active image
   rectangle, including viewer letterboxing. Reject obsolete displayed-source
   generations. Pin window identity, check foreground/occlusion/minimized state,
   preserve window-only mouse confinement and prohibit window-share keyboard.
   Extend capture switching with a mapping-ready transaction before allowing a
   fresh explicit grant; remove the current permanent switch inhibition only then.
3. Wire `QtRoomSession`/`RoomSessionWindow`, the existing `VideoFrameWidget` input
   events and CLI/preview paths to `Port`. Add first-use consent, capability choices,
   peer-identity-bound host grant/revoke, persistent indicators and AppShell panic
   revoke. Never translate admission, a request or a saved preference into consent.
   Focus loss/deactivation, device unplug and preview exit must release control.
4. Poll the selected viewer controller at up to 250 Hz on an appropriate input
   owner, submit changed state and use the service's complete-state keepalive.
   Keep high-frequency events off one-second status updates. Add test-owned sink
   injection throughout UI/CLI factories; tests must never fall back to SendInput.
5. Validate the complete interaction through real UI/CLI scripted controls and
   recorded sinks: multiple pads/local slots, focus/confinement, source changes,
   panic revoke, unplug, missing driver and backend failure. Retain the current
   real-channel/synthetic-response proof and add network-impairment scenarios.

Physical response latency, two-machine/NAT/TLS testing and production resource
acceptance remain Stage 4. One internal localhost sample is not a p95 result or an
external input-to-photon measurement. See HEADLESS-TESTING.md for exact evidence.
