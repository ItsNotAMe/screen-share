# Checkpoint C evidence

## Native WebSocket transport - 2026-09-15

ScreenShareRoom shares the existing validators/cache and the new private Qt
RoomSocket across backend builds, independently of Widgets. The normal app still
uses v1. This is a transport prerequisite for B integration, not completion of B
or C. Admission, the v2 Worker and media connection-ID dispatch remain open.

RoomSocket lives on the networking event loop; tests run it on a separate QThread.
It accepts externally obtained membership data and attaches the token only through
Authorization. Remote origins require wss, with Qt certificate/hostname validation
retained. Userinfo, queries, fragments, arbitrary origin paths and token control
characters are rejected. Plaintext requires explicit diagnostic opt-in and numeric
loopback. Actual remote TLS/backend-plugin integration still needs validation.

Behavior and embedding contract:

- Handshake is not readiness: require an identity-bound authoritative snapshot.
  Independent room/directory revisions use the existing atomic cache. One gap
  sends one resync; later deltas are suppressed until the snapshot.
- Check schemas, room/self target, known membership and host/viewer direction.
  Viewer offers and unknown targets are rejected. These client checks do not
  replace server token, role, socket and target authorization.
- Callbacks carry increasing local transport generations; old socket callbacks
  are rejected. Queue value copies to consumers and discard older generations.
  These are not media connection IDs. The media adapter still needs connection/
  negotiation generation and candidate-budget checks. Never log signal values.
- Limit incoming frames/messages to 64 KiB for rooms and 256 KiB for directory,
  then apply stricter schema-specific limits. Cap outgoing Qt buffered bytes at
  256 KiB. Pressure drops/reconnects rather than adding a retry queue. Send returns
  typed results; native error strings and submitted credentials are not echoed.
- Exact automatic heartbeat pair: `v2:ping` / `v2:pong`. Send after 29–31 seconds;
  missing pong expires after 10 seconds. Connection/snapshot/resync deadlines are
  also 10 seconds. Resync cancels heartbeat waiting so a late pong cannot cancel
  its deadline. No periodic HTTP polling exists in this component.
- Reconnect bases are 1/2/4/8/16/30 seconds, jittered to 80–100%, capped at 30.
  An accepted snapshot resets backoff. Stop/room.closed cancel timers and clear
  membership configuration. Credentials are held in memory, not persisted.
- Callbacks must not throw, reenter or destroy RoomSocket. Stop/destroy it on
  networking before joining that thread. Application queue ownership remains an
  integration responsibility; this component does not create an invocation queue.

The live local QWebSocket server test covers headers, snapshot gating, normalized
profile writes, direction/target rejection, pushed signals/state, one-shot resync,
actual heartbeat timeout and authenticated reconnect, directory lifecycle, stop/
stale delivery and saturated writes. It requires no input, Widgets or deployed
service. CTest/headless runner enforce a 75-second process watchdog.

Final evidence: `build/webrtc/room-socket-final-sdk-app-{debug,release}.log`,
`room-socket-final-sdk-room-cli-release.log`, and
`room-socket-final-headless-release/result.json`. Initial runs without the write
saturation test remain under `room-socket-*` without `final`.
Final Debug and Release application suites passed 11/11 each; the fresh Release
CLI-only build passed 6/6. Combined Release headless media/room smoke passed
13/13, recording executable hashes, timing, watchdog results and JSON metrics.

Next: admission/membership lifecycle, local Cloudflare runtime tests, and mapping
validated room values into the media owner. These tests do not establish service
authorization, free-tier headroom, NAT success or gaming latency.
