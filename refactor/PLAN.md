# Screen-sharing backend v2 — implementation handoff

Saved: 2026-09-14. Status: Gate A passed for native integration/build proof on 2026-09-15; Checkpoint B in progress. Full resource/field/release acceptance remains open. See CLOSEOUT-A.md.

Use [TODO.md](TODO.md) to track implementation, validation evidence and deferred work.

Use [COMPARISON.md](COMPARISON.md) for matched before/after measurements and cutover criteria. Passing component tests does not establish better gaming latency or image quality. Checkpoint B evidence is recorded in [CHECKPOINT-B.md](CHECKPOINT-B.md).

## 1. Objective, decisions and implementation rules

Replace the unstable streaming backend with a modular native WebRTC implementation. Preserve the native C++/Qt application and Cloudflare signaling service.

This document supersedes the earlier plans and contains the decisions needed for implementation.

### Product decisions

- **Windows 10/11 x64 first.** Keep portable interfaces around Windows implementations; defer actual Linux/macOS support.
- **Native `libwebrtc`**, using its full media stack. Do not substitute a data-channel-only library or send the existing custom video packets through WebRTC.
- **Direct peer-to-peer connections using ICE/STUN.** No TURN, SFU, media relay or paid service.
- Preserve display/window sharing, system/microphone/process audio, playback controls, mouse/keyboard control and gamepads.
- Preserve the current Qt screens and general appearance. Add the settings and diagnostics described below.
- Retire legacy manual UDP connections, manual NAT invites and LAN discovery after replacement validation.
- Keep a CLI for room-based diagnostics and automated testing, backed by the same engine as the UI.
- Default room capacity: **four viewers**, excluding the host. Allow increasing it with an upload/encoding warning.
- Four viewers is the primary validation target, not a protocol limit. Make the service abuse ceiling configurable; initially retain the existing 64-participant ceiling, allowing at most 63 viewers.
- No accounts in this implementation. Save a local nickname and preferences.
- Production deployment and release publishing are separate from implementing and validating the refactor.

### Rules for the implementing agent

- Implement in the ordered checkpoints in Section 5.
- Do not treat successful compilation or a localhost stream as proof that performance requirements are satisfied.
- Do not weaken latency, security or feature requirements to make a checkpoint pass.
- Do not add another custom bitrate controller around WebRTC.
- Do not leave mock implementations, hard-coded successful statuses or unverified performance claims.
- Tune numerical defaults only using recorded test evidence. Update the documentation when changing them.
- Keep components cohesive; do not recreate the existing large runtime file inside a new class.
- Preserve existing uncommitted work. At planning time, the checkout contains edits to CMake/runtime files and untracked adaptation/recovery modules and tests. Inspect the diff before implementation; do not reset or delete those changes indiscriminately.
- This handoff is saved as `refactor/PLAN.md`; keep its checkpoint/evidence checklist in `refactor/TODO.md`. Keep `agents/todo.md` synchronized with unfinished work when implementation begins.
- Update 2026-09-14: user authorized removal of unnecessary existing edits and creation of a dedicated branch. Legacy UDP adaptation/recovery edits were archived locally and removed; continue on `refactor/backend-v2`.
- When resuming with another model or effort level, read this document and the TODO first. Preserve the accepted decisions rather than reopening the architecture without evidence.

### Existing implementation facts that matter

- Streaming orchestration currently resides primarily in `ScreenShareRuntimeExecution.cpp`, roughly 6,600 nonblank lines.
- The live signaling loop normally performs heartbeat plus peer-list HTTP requests every two seconds. WebSocket notifications trigger additional refreshes.
- Room listings can verify directory entries against individual room objects.
- Current adaptation mixes sender, receiver and queue-pressure signals; historical reports describe seconds of stale sender backlog.
- Existing hardware encoding is not automatically trustworthy: earlier diagnostics found hardware input backlogs.
- Current decoded frame delivery and presentation include CPU byte buffers. A genuinely GPU-preserving path requires new frame ownership and presentation interfaces.
- Current documentation contains stale descriptions of password/key behavior. Treat inspected code and this plan as authoritative, then update conflicting documentation.

## 2. Native engine, settings and input implementation

### 2.1 Module boundaries

Organize the replacement into these responsibilities:

| Module | Owns | Must not own |
|---|---|---|
| Session coordinator | Lifecycle, operation IDs, configuration, viewer sessions, cancellation | Codec loops, HTTP parsing, presentation |
| Room client | Membership, signaling protocol, reconnection, directory subscription | Media adaptation, input injection |
| WebRTC engine | PeerConnections, tracks, codecs, transport statistics | Qt widgets, room authorization |
| Capture/audio adapters | Windows devices and timestamped source data | Signaling or congestion policy |
| Presentation adapter | GPU/CPU frame presentation and presentation metrics | Another jitter buffer or A/V controller |
| Input service | Permissions, device allocation, input validation and injection | Video encoding or room browsing |
| Diagnostics | Typed measurements, limiting reasons, local reports | Secrets or fabricated latency estimates |

Expose small interfaces such as `IVideoCapture`, `IAudioDevice`, `IMediaPeer`, `IVideoSink`, `IRoomTransport` and `IInputBackend`. Use concrete implementations behind them, not a generic plugin framework.

Windows and WebRTC headers must remain inside implementation modules. Portable public types contain standard C++ values and opaque, reference-counted resources.

### 2.2 Public API and lifecycle

Retain the `ScreenShareSession` facade concept and its start/stop/settings/control operations, but replace the internal runner.

Add a separate room-service interface for directory browsing and create/join operations. The UI and CLI must use it rather than constructing requests independently.

Public types must include:

- `LocalProfile`: nickname and saved preferences.
- `RoomPolicy`: name, visibility, password state and maximum viewers.
- `StreamPreferences`: preset plus independent resolution/FPS/bitrate settings.
- `ViewerStatus`: stable peer ID, nickname, connection state, requested/applied settings, actual measurements and limiting reasons.
- `OperationResult`: operation ID, success/failure and typed error.
- `SessionSnapshot`: immutable aggregate state.
- `VideoFrame`: owned frame resource, visible/coded dimensions, color metadata and timestamps.

Use explicit optional values for unavailable statistics. Unknown is not zero.

Session states:

`Idle → Starting → WaitingForViewers/Connecting → Running → Stopping → Stopped`

A fatal startup failure reaches `Failed`. Per-viewer connection failure does not fail the host session.

Implementation requirements:

- Serialize lifecycle/configuration changes on one control executor.
- Give every session and viewer connection a generation identifier.
- Reject callbacks/results from earlier generations.
- `Stop()` initiates cancellation and is safe to repeat.
- Complete shutdown releases input, closes media connections, stops capture/audio, detaches callbacks and joins owned threads.
- Never synchronously wait for the UI thread from a worker that the UI is joining.
- UI notifications are queued; high-frequency media/input do not traverse a one-second status loop.
- Update-only settings requests must not reset unrelated settings.

Use Qt Network and Qt WebSockets behind `IRoomTransport`, with one dedicated networking event loop. Require those Qt components for the CLI/backend transport too; Widgets remains UI-specific. Do not maintain separate WinHTTP and Qt signaling implementations.

### 2.3 WebRTC connection topology

Use one host-to-viewer PeerConnection per viewer. Do not create viewer-to-viewer media connections.

- Share capture frames and the source audio stream.
- Create a separate video-source adaptation wrapper, track, encoder and PeerConnection for each viewer.
- A viewer's source restrictions must never downscale the shared capture source for everyone.
- Host is always the offerer. Viewer answers and requests renegotiation/ICE restart through signaling when needed.
- Use Unified Plan, one outbound host video track and one outbound host audio track.
- Keep the initial implementation one-way for audio; do not introduce viewer voice chat.
- Bundle media/data on the PeerConnection.
- Enable ICE/STUN with local and server-reflexive candidates. Do not configure TURN credentials.
- Let WebRTC perform connectivity checks and consent verification; remove custom NAT probes and endpoint-retargeting logic.
- Trickle ICE candidates after setting the corresponding local description.
- Queue candidates arriving before the matching remote description, with a bounded queue.
- Associate SDP and candidates with a connection generation; discard old generations.
- Do not broadcast SDP, candidates or fingerprints to the entire room.

Set a 20-second initial connection deadline per viewer. On transient network loss, retain the peer briefly and initiate an ICE restart through the host. Limit restart attempts to three per minute per peer, with backoff. Failure must produce a specific direct-connect error and leave other viewers running.

Do not persist SDP or candidate histories in Durable Object storage. A connection interrupted during negotiation can negotiate again after reconnection.

### 2.4 Video pipeline and codec contract

Initial codecs: **H.264 video and Opus audio**.

Windows capture:

- Prefer Windows Graphics Capture.
- Retain DXGI fallback for display capture where appropriate.
- Never fall back from a failed selected-window capture to capturing the full desktop.
- Preserve cursor behavior, HDR-to-SDR conversion, visible dimensions and selected-source identity.
- Capture once at the required source quality; scale separately for viewer outputs.
- Treat source closure, minimization, resize and device loss as explicit states.

Implement Windows Media Foundation H.264 encoder/decoder adapters through WebRTC's codec factories. Reuse suitable low-level code only after removing its custom transport/runtime coupling.

Encoder requirements:

- Implement initialization, rate updates, keyframe requests, encoded callbacks and release against the pinned WebRTC API.
- `SetRates` must honor WebRTC's assigned rate; manual bitrate must not override it afterward.
- Report hardware/software implementation and adaptation capabilities accurately.
- Preserve capture timestamps through encoded output.
- Configure low-delay operation and zero B-frames where supported.
- Do not block the WebRTC signaling/network thread waiting for Media Foundation output.
- Own encoder operations on the encoder worker; process asynchronous Media Foundation events there.
- Keep at most one not-yet-submitted pending raw frame per encoder. Replace stale pending input before encoding.
- Maintain bounded tracking of submitted frames and their age.
- Do not discard arbitrary encoded fragments or dependent H.264 frames to reduce a queue.
- When reset/recovery invalidates references, request a new keyframe and allow WebRTC to manage transport recovery.
- Handle a zero rate as suspension rather than substituting a positive application floor.

Hardware selection:

- Probe the selected adapter for initialization, rate changes, keyframe generation and bounded output delay.
- Fail the probe if submitted frames remain stuck beyond 500 ms during active input.
- After a hardware error/stall, attempt software fallback for that viewer.
- Do not repeatedly oscillate between implementations. Quarantine the failing hardware implementation for that session.
- If software cannot support a manually fixed resolution, report the affected viewer's resource failure; do not silently change resolution.
- Under Auto settings, allow WebRTC's resource adaptation to reduce work.

Frame ownership:

- Native frames retain their GPU texture/device resources until all consumers release them.
- Use explicit synchronization and a single owner for D3D immediate-context operations; do not assume concurrent use is safe.
- The presenter consumes a GPU texture where compatible.
- Provide a CPU I420/NV12 fallback when GPU transfer is unavailable, and report that fallback.
- Preserve visible aperture/cropping independently from coded dimensions; 1920×1088 storage must not appear as a 1088-high picture or green strip.
- Use one replaceable pending frame at the application presentation boundary.
- Let WebRTC handle jitter buffering and media synchronization; do not reintroduce the old preview/audio gating loops.

### 2.5 Audio integration

Implement an Audio Device Module adapter around the existing Windows device capabilities. Feed/pull PCM through WebRTC's audio transport interfaces; do not encode Opus separately in the old audio pipeline. [WebRTC Audio Device Module](https://webrtc.googlesource.com/src/%2B/main/modules/audio_device/g3doc/audio_device_module.md)

- Preserve system output, microphone, selected output device and process-loopback selection.
- Convert to 48 kHz PCM and deliver 10 ms blocks.
- Preserve stereo system/process audio; downmix multichannel input consistently.
- Use event-driven WASAPI I/O.
- Keep application PCM queues bounded; start with a 30 ms capture handoff limit.
- Select supported low-period playback and record the actual device buffering. Do not assume requested periods were accepted.
- Disable AGC/noise suppression/echo cancellation for system/process audio.
- Apply microphone processing only to microphone input; never process the combined system-audio signal as microphone speech.
- Preserve mute/volume without stopping video.
- Missing or silent audio must not freeze video.
- Handle device changes explicitly; do not silently replace a failed selected process/device with another sensitive source.
- Use monotonic local timestamps and WebRTC's synchronization; never subtract unsynchronized clocks from different computers.

### 2.6 Auto/manual settings

Use explicit modes rather than interpreting zero as several different behaviors.

| Setting | Auto | Manual |
|---|---|---|
| Resolution | Adapt beneath the selected maximum | Keep the selected output canvas dimensions |
| FPS | Adapt according to preset, beneath the selected maximum | Target the selected rate; dropping stale frames remains allowed |
| Bitrate | WebRTC adapts beneath the calculated/user maximum | Selected value is the desired operating limit; congestion control may assign less |

For fixed output dimensions, preserve source aspect ratio using fit/letterboxing. Keep input coordinate mapping tied to the active image rectangle.

For native-size output, dimensions follow source resize. Native-size is still distinct from adaptive resolution.

Defaults for a fresh profile:

- Gaming preset.
- Auto resolution, maximum 1920×1080 fitted to source aspect ratio; do not upscale in Auto.
- FPS target 60.
- Auto bitrate.
- Derive the default Auto video maximum as
  `clamp(width × height × fps × 0.10, 2,000,000, 40,000,000)` bits/second, using the configured maximum output size.
- Start bandwidth estimation conservatively at no more than 3 Mbps or the configured maximum, whichever is lower.
- Do not enforce the calculated maximum as a minimum bitrate.
- Keep explicit user settings when switching presets. Presets change adaptation preferences, not manual selections.

These are initial tuning defaults, not claims about optimal quality.

Manual example: **1080p / 60 FPS / 12 Mbps**:

- Keep 1080p.
- Aim for 60 FPS.
- Let WebRTC assign less than 12 Mbps when necessary.
- Do not pad an unchanged desktop to consume 12 Mbps.
- Report compression/frame-drop limitations instead of accumulating delay.

WebRTC's bitrate setting is a maximum and remains subject to congestion constraints. [WebRTC encoding parameters](https://www.w3.org/TR/webrtc/)

Map adaptation deliberately:

- Auto resolution + manual FPS: prefer maintaining frame rate.
- Manual resolution + Auto FPS: prefer maintaining resolution.
- Both Auto: Gaming favors frame rate; Quality uses balanced/detail-oriented degradation.
- Both manual: disable automatic source resolution/FPS adaptation, while retaining transport congestion control and overload frame dropping.

Settings changes:

- Validate the complete proposed configuration first.
- Assign a settings revision and show pending/applied/error state.
- Apply without rebuilding the room or unrelated viewers.
- Prevalidate capability constraints before accepting a room-wide setting.
- On apply failure, report which viewer failed and retain/recover its last working configuration where possible. Do not claim an atomic cross-peer hardware change.
- Show a viewer as temporarily out of sync with the host settings until application/recovery succeeds.

Optional aggregate upload setting:

- Treat it as an application media budget, not an exact physical-interface shaper.
- Reserve 20% for transport/recovery overhead and an audio allowance before equally dividing the remaining video budget among active viewers.
- Apply each viewer's smaller individual limit.
- Recompute on membership/settings changes; do not add a new measured-bandwidth feedback loop.
- Display actual aggregate wire usage separately because retransmissions/probes can exceed estimates.

### 2.7 Gaming input and permission safety

Gaming must optimize **input delivery and returning video**, not just video settings.

Data channels:

| Channel | Delivery | Payload |
|---|---|---|
| Control | Reliable, ordered | Grants/revokes, mouse/key transitions, wheel events, restart requests |
| Input state | Unordered, `maxRetransmits=0` | Pointer motion and complete gamepad state |
| Telemetry | Unordered, `maxRetransmits=0` | Replaceable receiver statistics |

- Keep control messages small; do not send large reports/files on these channels.
- Input processing runs separately from rendering, encoding, signaling HTTP and statistics.
- Coalesce pointer motion to the newest state, initially capped at 250 updates/second.
- Poll gamepads at up to 250 Hz and transmit changed state; send a state keepalive at least every 100 ms while granted.
- Respect WebRTC channel backpressure. Drop replaceable states rather than queueing them.
- Bound reliable input queueing to 64 messages/16 KiB. On sustained overflow, neutralize/revoke the affected input session rather than replaying a backlog.
- Every input message includes protocol version, connection generation, permission generation and sequence number.
- Validate lengths, enums, ranges and finite coordinates before injection.
- Serialize fields explicitly; never transmit raw packed C++ structs.

Safety:

- Keep mouse and keyboard ownership exclusive.
- Preserve at most three remote virtual gamepads, fewer when local controllers occupy slots.
- Use the existing 300 ms input watchdog.
- Disconnect, revoke, watchdog expiry, unplug or backend error neutralizes held input immediately.
- After watchdog expiry, invalidate the old input generation; require a fresh host-issued generation before accepting more events. This prevents late reliable key presses reviving stale input.
- Preserve first-use consent, persistent control indicators and panic revoke.
- Preserve window-only mouse confinement and disabled keyboard injection for a single-window share.
- On capture-source changes, neutralize input and rebuild coordinate mapping before resuming.
- Preserve current PlayStation/XInput support.
- Controller drivers remain installer-managed. Normal startup never installs or repairs them.

## 3. Room protocol, persistence and security

### 3.1 Server responsibilities and API

Implement v2 using separate room/directory Durable Object namespaces. Keep TypeScript strict mode and add Worker integration tests.

Public API:

| Endpoint | Purpose |
|---|---|
| `POST /v2/rooms` | Create room and return host membership |
| `POST /v2/rooms/:id/join` | Verify admission/password and return viewer membership |
| `GET /v2/rooms/:id/events` | Authenticated room WebSocket and resume |
| `GET /v2/directory/events` | Public directory snapshot plus changes |
| `GET /v2/rooms` | Snapshot for explicit refresh/diagnostics |
| `GET /v2/health` | Operational health/version |

Create/join returns opaque room/peer IDs, a server-issued membership token, role and protocol version. No UDP room access key.

Use authorization headers for native HTTP/WebSocket connections. Never put membership tokens or passwords in URLs.

Do not automatically retry an ambiguous create/join response as if it were definitely unsuccessful. Unconfirmed memberships expire after 30 seconds; a fresh user attempt may create a new provisional membership. Publish a room only after its host attaches.

Normal mutations occur through the authenticated room socket:

- `profile.update`
- `room.update`
- `peer.disconnect`
- `peer.leave`
- `signal.offer`
- `signal.answer`
- `signal.candidate`
- `signal.restart_request`
- `state.resync`

### 3.2 Message structure and ordering

Use a shared JSON protocol specification and client/server fixture tests.

Envelope fields:

- `v`: protocol version, initially `2`.
- `type`: known message discriminator.
- `requestId`: command identifier where an acknowledgement is required.
- `roomId`: room identity where applicable.
- `revision`: only for room/directory state snapshots and changes.
- `connectionId`: generation for SDP/ICE messages.
- `toPeerId`: only for targeted messages.
- `payload`: validated type-specific data.

The server supplies sender identity from the authenticated connection. Ignore/reject client attempts to set another sender.

Important ordering rules:

- Room state and directory state have separate monotonically increasing revisions.
- Directed SDP/ICE messages do not consume the room-state revision; otherwise receivers would see false gaps.
- Ignore old/duplicate state revisions.
- A gap triggers one `state.resync`; do not repeatedly refetch until the snapshot arrives.
- Reconnect sends an authoritative snapshot; no persistent event history is needed.
- Configuration commands carry an expected revision. Return a typed conflict rather than overwriting a newer edit.
- Use idempotent set-value operations; deduplicate repeated command IDs within the active connection.
- SDP/ICE retries are scoped to the current connection generation.

Keep room metadata/commands to 16 KiB, SDP/signaling frames to 64 KiB and directory snapshots to 256 KiB. Limit each connection generation to 64 candidates and bounded pending candidate storage. Measure byte lengths, not only JavaScript string length.

### 3.3 Hibernation and liveness

Use `acceptWebSocket`, socket attachments and `setWebSocketAutoResponse` with a fixed ping/pong pair.

- Client application ping every 30 seconds, with small scheduling jitter.
- Missing pong for 10 seconds triggers connection recovery.
- Reconnect delays: approximately 1, 2, 4, 8, 16 and 30 seconds, with jitter; maximum 30 seconds.
- Store peer ID, role, socket generation and connection timestamps in the attachment.
- On wake, reconstruct live bindings from attached sockets plus durable state.
- Use `getWebSocketAutoResponseTimestamp()` for heartbeat freshness. Do not require a JavaScript handler/storage write per ping.
- Every 30 seconds while a room exists, an alarm checks membership deadlines and pending cleanup.
- Membership expires after 90 seconds without valid activity, with cleanup on the next alarm.
- Socket replacement invalidates/closes the previous socket. An old socket's close callback must not remove its replacement.
- Explicit leave/kick acts immediately.
- An unexpected host disconnect marks the room reconnecting and blocks new joins. Existing media can continue while reconnecting.
- Host expiry closes the room; there is no automatic host election.
- Tokens remain valid for the active membership and reconnect grace, then are invalidated. Do not persist them in local profile settings.

Cloudflare exposes automatic responses and their timestamps specifically for hibernating sockets. [Durable Object state API](https://developers.cloudflare.com/durable-objects/api/state/)

### 3.4 Directory consistency without listing fanout

Directory entries contain only safe summaries:

- Room ID/name.
- Viewer count and viewer limit.
- Password-required flag.
- Joinability/status.
- Summary version and lease expiry.

Do not include member rosters, addresses, SDP, passwords, tokens or media keys.

- Publish after host attachment and on meaningful summary changes.
- Renew the directory lease every 60 seconds while the host membership remains live.
- Lease duration: 180 seconds.
- Renewal alone must not broadcast a visible room change.
- Directory cleanup alarm runs every 60 seconds while entries exist.
- No requests to individual rooms when serving a list.
- Push deletions immediately for normal room closure.
- Exclude unlisted rooms entirely.
- Close the client directory subscription when the browsing screen is hidden.
- Explicit refresh may use the snapshot endpoint; no automatic HTTP polling fallback.

Handle cross-object failures:

- Persist a pending directory update/version in the room transaction.
- Send it after commit and retry from the room alarm if it fails.
- Directory accepts only newer summary versions for an existing room.
- Room IDs are random and never reused.
- Closure cleanup must retain enough pending state to retry directory removal before deleting the room's remaining state.
- Leases are the final fallback against ghost rooms.
- Directory reservation/admission failures fail closed; remove the existing fail-open behavior around global room limits.

### 3.5 Profile and room customization

Local profile:

- Save nickname, preset and explicit stream preferences with QSettings.
- Default nickname: `Guest-` plus a random four-character suffix.
- Never derive the nickname from Windows account or computer names.
- No persistent account identity or server profile database.
- Nicknames: normalize to NFC, trim surrounding whitespace, require 1–32 Unicode code points and at most 128 UTF-8 bytes.
- Reject control characters and bidirectional override/isolate controls.
- Render as plain text.
- Duplicates are allowed; append a short peer-ID suffix in the UI when ambiguous.
- Nicknames never determine permissions or ownership.

Room policy:

- Public by default; unlisted available.
- Room name, visibility and viewer limit editable live.
- Password configured at creation; defer live password rotation.
- Lowering the viewer limit below current occupancy does not kick existing viewers; block new admissions until below the limit.
- Duplicate names and room names are permitted.
- Copied v2 links contain only a versioned room identifier, never passwords/tokens.

### 3.6 Authorization and abuse protection

- Generate peer IDs and 256-bit random membership tokens server-side.
- Store token hashes, not raw tokens; compare digests safely.
- Check membership, role, socket generation and target permissions on every command.
- Only the host edits room policy or disconnects other viewers.
- Only host/viewer pairs exchange signaling.
- Continue using salted, versioned PBKDF2 password verification through WebCrypto; initially preserve the existing 100,000-iteration implementation, subject to verification in the deployed runtime. Do not reduce its work factor.
- Send passwords only in HTTPS bodies and never echo them.
- Preserve closed-by-default CORS and certificate validation.
- Use the existing authoritative rate-limiter concept for connection/create/join admission. Do not perform a cross-object rate-limiter request for every media/control/room message.
- Apply bounded per-socket command/candidate rates within the room object.
- Start with the existing 240 HTTP requests/minute/IP ceiling, add tighter create/password-attempt buckets, and test legitimate shared-NAT usage.
- Retain the global 500-room safety cap as configurable deployment policy.
- Do not expose peer-IP-based banning as durable identity protection.
- Kicking revokes the current token and closes the corresponding host connection. Without accounts, rejoining under a new identity cannot be permanently prevented.
- The signaling service remains trusted for membership/fingerprint exchange; do not claim protection against a compromised signaling service.

## 4. UI, diagnostics and build delivery

### 4.1 Viewer information

Each viewer row must show:

- Nickname and stable disambiguation.
- Connecting/live/reconnecting/failed state.
- Actual resolution and FPS.
- Actual video bitrate.
- Health/limiting reason.
- Control permissions and disconnect action.

Provide a details popup with:

- Host requested settings and applied settings revision.
- Actual capture/encode/decode/presentation rates.
- Encoder name and hardware/software state.
- Encode time and pending input age.
- RTT, loss, jitter and available bandwidth when exposed.
- Retransmission/recovery counts.
- Decode/presentation drops and buffering statistics.
- CPU-copy fallback state.
- Receiver telemetry age.
- Reason quality is limited: network, host encoding, receiver decoding/rendering, host budget or manual configuration.

Refresh native WebRTC statistics once per second. Send receiver-only telemetry directly over the telemetry data channel once per second. Never send these updates through Cloudflare.

Mark remote statistics stale after three seconds without an update. Do not label an unchanged desktop as a stalled stream merely because few new images appear.

### 4.2 Diagnostics and measurement

Keep structured local reports with session/peer correlation IDs, configuration, implementation versions and measured counters.

- Separate capture rate from encoded/presented FPS.
- Label bitrate as video payload versus total wire rate.
- Derive rates from counter deltas using monotonic local time.
- Reset baselines on connection/stream generation changes.
- Report RTT as RTT, not “latency.”
- End-to-end capture-to-display and input-to-visible-response require dedicated measurement.
- Redact passwords, tokens, SDP, ICE credentials and addresses.
- Preserve signed updater behavior and report-location behavior.

### 4.3 Toolchain and reproducibility

Replace default MinGW presets with Windows x64 CMake/Ninja presets using clang-cl and the MSVC ABI.

- Build WebRTC from official source using its GN/Ninja workflow.
- During the integration checkpoint, resolve and record one exact upstream commit and its toolchain/dependency revisions.
- Record all GN arguments, architecture, debug/release mode and runtime-library choices.
- Do not leave a floating `main`, “latest” download or unverified third-party WebRTC binary in the build.
- Match ABI, CRT, standard-library, RTTI/exception requirements and debug/release settings across application, WebRTC and Qt.
- Qt must use an MSVC-compatible build; do not link existing MinGW Qt binaries.
- Export a narrow CMake imported target for the WebRTC artifact.
- Cache artifacts by source revision, GN arguments and compiler/runtime identity.
- Allow an explicitly provided local artifact directory for offline development.
- Keep application builds separate from expensive dependency builds.
- Include dependency notices and update portable/installer DLL staging.
- Preserve installer-only controller-driver provisioning and update-signature verification.

WebRTC documents Windows x64 and Clang support; verify actual build requirements against the pinned source rather than guessing GN flags. [Supported platforms and compilers](https://webrtc.googlesource.com/src/%2B/HEAD/g3doc/supported-platforms-and-compilers.md)

## 5. Ordered checkpoints, tests and completion criteria

### Checkpoint A — baseline and build proof

Deliver:

- Saved architecture/protocol documents.
- Current-backend performance baseline with settings, hardware and network recorded.
- Reproducible pinned WebRTC debug/release artifacts.
- Qt/CLI executable using the new toolchain.
- Two local PeerConnections exchanging H.264, Opus and encrypted data channels.
- Proof of rate updates, forced keyframes, encoder reset and CPU/GPU frame ownership.

**Gate:** do not migrate the whole application until native codec/audio integration and build reproducibility work. If an API lacks a required capability, document the exact source/API finding and revise the design; do not silently omit the capability.

### Checkpoint B — native media engine

Deliver:

- Typed session lifecycle.
- Windows capture/audio/codec/presentation adapters.
- Auto/manual setting semantics.
- Independent viewer source wrappers and encoders.
- Stop/restart/source-change/device-loss behavior.
- A local diagnostic harness with synthetic video/audio sources.

Tests:

- Manual resolution remains fixed under bandwidth reduction.
- Manual bitrate never bypasses congestion control.
- Auto resolution adaptation for one viewer does not alter another viewer's source restrictions.
- Encoder failure affects only its viewer.
- No stale callback reaches a later session.
- Queues stay bounded during slow encoding/rendering.
- Visible/coded dimensions and source aspect ratios remain correct.
- No audio/video reciprocal waiting deadlock.

### Checkpoint C — room service v2

Deliver:

- New Worker namespaces and native room client.
- Authenticated create/join/resume.
- Targeted SDP/ICE negotiation.
- Directory snapshot/deltas.
- Hibernation-aware liveness and retryable directory updates.
- Profile and room settings.
- Shared protocol fixtures and Worker tests.

Use local Cloudflare runtime tests, not only mocked TypeScript classes. Add scripts for Worker typechecking and tests; include both in the validation checklist.

Tests must cover:

- Simultaneous joins and capacity enforcement.
- Wrong passwords and expired/replayed membership credentials.
- Host-only command enforcement.
- Socket replacement followed by old-socket closure.
- Hibernation followed by ping freshness checks.
- Duplicate/gapped revisions.
- Directory update failure and retry.
- Host crash, normal leave and unlisted-room behavior.
- Malformed/oversized JSON, SDP and candidate floods.
- No per-room checks during listing.

### Checkpoint D — UI and gaming controls

Deliver:

- New backend behind both UI and CLI.
- Auto/Manual settings and Gaming/Quality presets.
- Live room browser and saved nickname.
- Per-viewer summary/details.
- Mouse/keyboard/gamepad delivery and permissions.
- Existing consent, revoke, confinement and installer behavior preserved.

Tests:

- Input remains responsive during high video bitrate, retransmissions and keyframe bursts.
- Pointer/controller state does not build a backlog.
- Reliable key/button transitions cannot revive input after watchdog expiry.
- Three remote pads remain independent and local slots stay reserved.
- Source changes and lost focus release/confine input correctly.
- Missing driver disables gamepad grants without breaking sharing.
- Settings changes show pending/applied/error states accurately.

### Checkpoint E — impairment tests, real machines and cutover

Extend the existing multi-viewer harness for the new CLI. Test reproducible scenarios with fixed random seeds where applicable:

| Scenario | Initial test conditions |
|---|---|
| Healthy | 1080p60, ample bandwidth, one and four viewers |
| Bandwidth collapse | 20 Mbps → 4 Mbps → 20 Mbps on one viewer path |
| Loss/jitter | 2% and 5% loss, up to 50 ms added jitter |
| Reordering | Reordered/duplicated packets without injected loss |
| Slow receiver | Delayed decode/presentation on one viewer |
| Host pressure | Encoder exhaustion and hardware failure |
| Lifecycle | Late join, kick, leave/rejoin, host restart |
| Network | Interface change, ICE restart, direct UDP blocked |
| Input | Lost state, delayed reliable events, revoke during congestion |

Use WebRTC's supported simulated-network facilities for deterministic media tests. Use actual two-machine LAN/Internet tests for Windows devices, NAT behavior and end-to-end latency; fake or loopback tests do not replace these.

Acceptance requirements:

- Healthy reference LAN, 1080p60 Gaming: **p95 capture-to-display below 80 ms**.
- Same environment: **p95 input-to-visible-response below 120 ms**, using a deterministic host response scene. Arbitrary game-engine delay is additional.
- Quality preset: p95 capture-to-display below 250 ms.
- Steady A/V skew within ±50 ms.
- Record p50/p95/p99 and sample counts, not only averages.
- Measure end-to-end latency with an external camera/clock method; report local pipeline timing separately.
- After a capacity reduction, stale frame age must not continue increasing. At sustainable reduced settings, aim to settle within three seconds.
- After bandwidth recovery, resume upward adaptation without repeated resolution/encoder restart oscillation.
- One impaired viewer must not independently force healthy viewers down, except where a documented shared host resource or aggregate budget is limiting.
- Two-hour four-viewer soak: no deadlock, sustained memory growth or accumulating queues.
- Repeat start/stop at least 100 times; no retained sessions, sockets, devices or callbacks.
- Normal room/list changes visible within two seconds on a healthy signaling connection.
- Healthy sessions produce **zero periodic HTTP membership/list polls**.
- Directory listing performs **zero per-room verification requests**.
- Heartbeats perform no per-peer durable writes.

These performance numbers are acceptance targets, not results already achieved.

### Free-tier validation

Create a usage report for ten rooms, each with one host/four viewers active for eight hours, plus ten directory subscribers.

Count:

- Worker HTTP requests/upgrades.
- Durable Object messages and cross-object calls.
- Alarm invocations.
- Storage reads/writes.
- Active duration.
- Directory broadcasts.

Confirm the implementation comfortably fits the applicable free-tier allowances for that scenario, leaving at least 50% headroom in each measured daily allowance. Include account-wide usage caveats; do not assume WebSockets make all operations free. [Cloudflare pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/)

### Cutover and cleanup

- Validate a prerelease against separate v2 namespaces.
- Do not migrate active v1 rooms or their shared media keys; users recreate rooms.
- Keep existing updater compatibility so older installations can upgrade.
- After coordinated cutover, old protocol endpoints return a typed upgrade-required response.
- Remove old custom UDP transport/crypto, polling, adaptation, NAT-invite and runtime-control code that no longer serves a supported path.
- Remove unused KV fallback bindings and obsolete tests only after equivalent new behavior is validated.
- Keep useful platform adapters, security tests, controller tests and updater tests.
- Update README, usage/build instructions and stale repository memory.
- Finish with exact validation results and outstanding hardware/network limitations. Do not mark unrun manual checks as passed.

### Deferred TODO

Record, without implementing in this refactor:

- Host approval queue for newcomers.
- Accounts and profiles across devices.
- Additional personalization.
- Actual Linux/macOS ports.
- Encoding reuse/simulcast/SVC optimization for larger rooms.
- Maintained replacement for the retired virtual-controller runtime.
- Live room-password rotation.
- Additional codecs after H.264/Opus performance is established.
- Optional relay support only if the direct-only/free requirement changes.
## Capture-owner implementation note (Gate A evidence, 2026-09-15)

WGC requires capture-item message delivery even with a free-threaded frame pool.
Keep the Windows capture owner thread and its COM apartment alive through frame,
session and owned-dispatcher cleanup. `WindowsCaptureDispatcher` now creates a
current-thread queue only when necessary, borrows existing caller queues, pumps
bounded message batches and preserves WM_QUIT. Owned queues complete asynchronous
shutdown before COM uninitialization. Do not move these calls to the UI thread;
capture methods and destruction must stay on their dedicated owner thread.

For vanished/replaced sources, Stop pumps the actual Closed notification with a
250 ms deadline before revoking the handler. This is not a fixed active-capture
delay or a hard deadline on native RPC/driver calls; retain process watchdogs.
The GraphicsCapture.dll pin remains until separate evidence permits removal.

The application must also retain WindowsMediaRuntime across capture sessions
and joined worker restarts. Create it after the UI STA/QApplication, check its
HRESULT, and destroy all sessions before releasing it. Do not use static
process-shutdown ownership. This balanced MTA usage lease eliminates the
reproduced linear COM remoting event growth: 100-cycle full and rapid-close
handle bounds pass. Complete resource teardown, external latency and production
session integration remain open. Use CHECKPOINT-A.md and BUILD.md for evidence
and the embedding contract before declaring cutover ready.

## Headless live-session testing requirement — 2026-09-15

User requirement for subsequent implementation: provide an easy, documented,
single-command way to exercise the program in realistic real-time sessions
without manual mouse/keyboard interaction. Build this alongside production
session integration so future changes can be validated autonomously.

- Drive the production session API and media/control paths from a CLI scenario
  runner; avoid a separate mock engine that can pass while the application fails.
- Provide a fully headless mode with paced synthetic video/audio, local host and
  viewer processes, and an offscreen receiver. Cover start/stop, reconnect,
  settings changes, fixed/auto bitrate, multiple viewers and recovery.
- Add an unattended Windows hardware mode using a generated capture target for
  WGC/encoder/presentation coverage. Explicitly report when an interactive desktop
  or GPU is required; a synthetic headless pass does not prove WGC coverage.
- Exercise gaming control with scripted protocol events and a test-owned input
  sink/target; require no physical input and do not inject into unrelated apps.
  Test grant/revoke, stale-event rejection and input-to-frame response timing.
- Support deterministic scenario configuration, timeouts, process cleanup and
  nonzero failure exits. Save machine-readable results plus diagnostic logs:
  frame delivery, queue age, latency distributions, bitrate/FPS, drops, recovery
  and resource growth. Redact credentials and sensitive signaling data.
- Include repeatable constrained-network scenarios and document which timing
  measurements are internal estimates. Keep actual display/input latency and
  external-network acceptance separate from headless checks.

Reuse existing proof/stress tooling where practical. Document one quick smoke
command and one longer regression command with prerequisites and artifact paths.
Implementation resumed on 2026-09-15. The first shared capture-session and
headless media runner are implemented; see HEADLESS-TESTING.md for commands,
coverage and remaining production-facade/input/network integration.

## Stream-settings implementation note — 2026-09-15

The production settings core maps Auto/Manual preferences to WebRTC RTP limits
and per-viewer VideoAdapter requests. Fixed canvases fit/letterbox instead of
cropping; native mode follows input; Auto does not upscale. The implementation
accepts even dimensions up to 3840x2160, FPS 1–240 and explicit bitrate limits
1,000–100,000,000 bps. Auto calculation retains the original 2–40 Mbps clamp.
These are validation bounds, not claims that every machine supports every mode.

Call ViewerStreamSettings on the serialized coordinator, one owner per viewer
generation. Success means RTP parameters accepted and source revision queued;
source observedRevision is not remote presentation acknowledgement. Keep the
initial bitrate calculation for peer startup wiring; do not reset bandwidth
estimation on every settings change. Aggregate upload allocation, startup wiring,
full UI/settings transactions and actual congestion acceptance remain pending.

Only call VideoAdapter::OnOutputFormatRequest when source limits change: the
pinned implementation resets its framerate controller on that call. Calling it
per frame prevents FPS reduction; the headless regression now catches this.
GPU resizing now uses per-viewer NV12 plane scaling with owned output textures and
a four-submission bound per capture device. Unsupported scaling is quarantined and
uses the counted CPU fallback. Matching dimensions retain native frames. Source
path and active-image geometry are exposed through the shared API/UI/CLI; see
GPU-SCALING.md. Input mapping and end-to-end latency acceptance remain open.

## Gate A closeout decision — 2026-09-15

At the user's request, close A against its original Section 5 native-codec/audio
and reproducible-build gate. Available legacy baseline evidence and a current
extracted-package smoke audit are recorded in CLOSEOUT-A.md. Production session
teardown stays in B/E, actual hardware/external latency acceptance in E, and
remaining distribution/fresh-machine installer work in release preparation.
Nothing unrun is marked passed and the original performance targets remain.

The closeout resource recheck FAILED: all 100 full cycles and 20 rapid-close
cycles completed without crash/timeout, but median handle growth was +76 and
+10 respectively (bound 8). This supersedes any earlier claim that current
resource acceptance is resolved. Preserve existing MTA/dispatcher/module-pin
mitigations; investigate the discrepancy under B before production cutover.
Gate A passing authorizes wider implementation, not deployment or release.

## Peer recovery policy implementation note - 2026-09-15

The portable per-connection policy uses the specified 20-second initial deadline
and three restart attempts in a rolling minute. Initial backoff defaults are
500 ms, 1 second and 2 seconds according to recent attempts; duplicate failure
notifications cannot extend the wait. A reconnect preserves the recent-attempt
budget. Each restart has a 20-second attempt deadline before rescheduling within
that budget. These backoff values are initial implementation defaults, not tuned
performance claims. A closed/failed instance cannot be revived; a replacement
connection needs a fresh generation and instance.

The four-peer proof now exercises an explicit host-offered restart and reports
elapsed negotiation/media-check times. Automatic production action dispatch,
authenticated restart requests and actual outage/interface-change validation
remain required. The local proof does not establish NAT or outage recovery time.

## Follow-on UI design phase — requested 2026-09-16

After completing the current implementation, acceptance and cutover milestones,
refactor the UI's appearance, feel and usability. This is milestone 6 in TODO.md:
review the complete create/join/share/watch journey, establish consistent visual
and interaction patterns, improve feedback/accessibility and verify visual and
end-to-end behavior. Preserve latency, security and backend/frontend boundaries.
Necessary UI integration continues during the current refactor as already planned;
the later design phase neither blocks that work nor begins before the current
milestones are complete.
