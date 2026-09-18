# Integrated media diagnostics

The Stage 2 diagnostics/integration group is implemented across the public native
snapshot, existing UI/CLI and encrypted peer telemetry. It adds no room-service
requests, adaptation controller or media queue. Ordinary legacy entry-point
migration remains in the adoption group; these checks exercise the shared v2
RoomApplication shell, RoomSessionWindow and production CLI session runner.

## Observations and ownership

- `StreamStatus.capture`: current host capture lifecycle, typed failure and source
  generation. Each peer includes delivered/replaced/rejected handoffs, consumer
  failure and maximum capture-to-consumer handoff age. This age is not image latency.
- `StreamStatus.codec`: nullable Windows host hardware-session observations,
  hardware-frame/software-fallback counters, quarantine and device retirement.
  These counters are shared across viewers and reset with the hardware session.
  They do not identify which viewer caused a fallback or establish GPU decoding.
- `PeerStreamStatus.recovery`: current connection state, typed lifecycle reason,
  restart revision, dispatch failure and operation failure. Retired peers disappear;
  this is a current snapshot, not a persistent failure history.
- Sender stats: allowlisted encoder implementation, optional mean encode time
  (cumulative encode time / encoded frames), retransmitted packets, NACK and PLI
  counters, alongside existing payload/FPS/network measurements. Missing values
  stay unknown. Pending encoder input age is explicitly unknown: no per-encoder
  pending-age observation is currently available through the public adapter.
- Receiver stats: allowlisted decoder implementation, dropped decoder frames and
  optional mean jitter-buffer residence (cumulative delay / emitted count). This
  is a lifetime average, not instantaneous buffering or end-to-end latency.
  `jitterBufferRecentMs` separately differences delay/emitted counters over the
  most recent fresh sampling interval. Initial, stale (three seconds), reset,
  different-stream, invalid and zero-emission intervals remain unknown. A measured
  zero is retained. This value can reveal recovery hidden by lifetime averaging;
  it still measures emitted-frame buffering, not the oldest queued frame or
  capture-to-display latency. Wire V3 adds four bounded bytes, with V1/V2 decoding
  retained. Pre-V3 receivers cannot decode V3 telemetry; use matching builds for
  diagnostics (media negotiation is unchanged).
  Windows GPU decode maps to `mf-h264-hardware`; startup/runtime software fallback
  maps to `mf-h264-software`. The current implementation determines this label.
- Local receive handoff: `gpuRetained` counts native decoded frames consumed by
  the frontend, and `gpuReadbacks` counts explicit cached CPU conversions requested
  through those frames. Qt displays both; final CLI presentation JSON includes
  both. These local counters are separate from sender hardware/scaling counters
  and are not transmitted as additional receiver telemetry fields.
- Receiver presentation: actual frontend renderer submissions, total drops,
  pending-frame count (0 or 1), and last outcome. Qt publishes on its existing
  one-second diagnostics timer; CLI uses its existing periodic report. No renderer
  means unknown, even when decoding works. Counters cover the renderer lifetime;
  ICE restart resets telemetry freshness/sequence, not renderer lifetime counters.

Presentation uses one mutex-protected replaceable snapshot with a three-second
local expiry. Signaling never calls into the frontend. The existing one-Hz decoder
sample incorporates a fresh presentation snapshot. All remote fields expire
together three seconds after the host's last accepted report. Fresh decoding with
an expired local presentation sample clears presentation instead of retaining it.
Receiver claims never authorize input, acknowledge a settings revision, prove
physical display, or drive a second bitrate controller.

UI/CLI share the same JSON vocabulary. UI measurement text refreshes at most once
per second; membership, settings, failure and receiver-staleness transitions update
immediately. This avoids rebuilding a table at frame/status cadence. Names remain
plain text; arbitrary codec strings, addresses, SDP and credentials are not emitted.

## Settings behavior

The desired revision, each viewer's successful revision/preferences, and the
source-observed revision remain separate. A WebRTC sender rejection preserves that
viewer's previously working source, RTP parameters and applied preferences. Other
viewers may apply successfully. UI/CLI count applied, pending and rejected viewers
as disjoint groups; partial application is explicit. Retry is the existing Apply
action, not an automatic loop. A zero video allocation is an intentional pause.

Changing Gaming/Quality preserves explicit resolution, FPS and bitrate selections.
Tests exercise actual controls, profile persistence and configuration parsing, and
the sender boundary confirms that manual settings disable automatic size/FPS
degradation without creating a minimum bitrate floor.

## Validation and limits

`StreamSettingsTest` injects sender rejection and unsupported topology at the real
WebRTC interface, exercises two independent source/settings instances, retries,
pauses/resumes and validates real stats objects. `NativeRoomRuntimeTests` covers
V1/V2/V3 framing, all truncations, invalid flags/ranges, canonical absence, expiry,
replay, generation changes and local presentation expiry. UI tests cover stale
serialization, actual renderer-to-host telemetry, presets and the real partial-
application label. CLI tests cover extended JSON and actual Windows presentation;
the no-renderer scenario explicitly requires null presentation. Four-viewer native
tests retain telemetry pause/resume, restart, rejoin and single-peer failure checks.

See HEADLESS-TESTING.md for final evidence. These tests use synthetic audio and no
physical input. Capture/presentation FPS inference, encoder pending age, input
timing and external capture-to-display measurement remain unavailable measurements,
not invented zeroes. Hardware decode/zero-copy work, physical device acceptance,
normal legacy entry-point parity, Stage 3 input and Stage 4 performance acceptance
are separate requirements and remain open.

## Sender recovery investigation

The shared UI/CLI/report includes `targetVideoBps`, `framesEncoded`,
`keyFramesEncoded` and `meanPacketSendDelayMs`. These use the same bounded,
once-per-second WebRTC stats request. The delay is a lifetime mean for packets
already sent, not the current oldest queued packet, a maximum or physical latency.
First/missing/stale values remain null; zero remains a measured zero.
The native stats test covers units, invalid data and expiry; report tests cover
stale suppression and fresh zeroes. Packet impairment artifacts include these
fields to distinguish sender assignment, encoded output and transport behavior.

The MF adapter now honors WebRTC's unavailable-FPS contract: a positive bitrate
still resumes video using InitEncode's maximum FPS. Finite targets are clamped
before integer conversion. Software tests cover zero, negative, NaN, infinity,
extreme and fractional FPS targets after suspension. This is independent of the
recorded congestion-recovery failure and must not be described as its proven fix.
