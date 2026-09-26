# Room diagnostic reports

Save reports on the host and affected viewer after a failed attempt, preferably
while the room is still open. Include the healthy viewer when comparing FPS.
Both endpoints should use the same test build. Reports identify the executable
by SHA-256, not only the displayed version. A peer's `correlation` matches the
other endpoint's `selfCorrelation` within the same room without exporting IDs.

`schema` remains 1; `diagnosticsSchema: 2` adds:

- OS/kernel/architecture, Qt build/runtime, executable fingerprint, GPU PCI
  vendor/model IDs, adapter memory and driver version when available.
- Room admission, socket and runtime events; per-peer negotiation stage,
  SDP operation error codes, ICE gathering/connection and full transport state,
  STUN/TURN error codes, candidate counts/types, restart and terminal failures.
- Transport samples during startup as well as streaming: candidate-pair states,
  connectivity requests/responses, selected route type/protocol/address family,
  DTLS state, RTT, bandwidth estimate, packet loss and send/receive counters.
- Audio concealment/jitter and video received/decoded/encoded/dropped frames,
  decode/encode time, resolution, FPS, keyframes, feedback and freezes where
  the native transport supplies them. Missing fields mean unavailable, not zero.
- Codec initialization, hardware/software fallback, optional low-latency support,
  capture/device errors and native HRESULTs tagged with the failed operation.
- Per-peer delivery gate, capture scaling/readback/drop counters, allocated and
  applied bitrate, receiver observations and presentation counters.
- Process CPU, private/working-set memory, handle count and maximum runtime tick
  gap per sampling interval; renderer failures, recoveries and presentation time.

Event histories retain startup events and the latest events (128 records by
default), plus a sticky first failure. One-second measurement histories retain
the latest 60 samples. Each history includes a UTC start anchor, monotonic elapsed
time and omitted-record count. Cross-machine clocks may differ; use correlations
and event sequences as well as wall time. Up to 63 retired peer snapshots survive
removal. Export size is capped at 32 MiB and failed writes preserve the old file.
Immutable cached histories avoid rebuilding large report objects on every tick;
the CLI serializes the complete report only when saving it.

Reports use explicit field allowlists. They omit SDP, ICE credentials, raw
candidate addresses, server URLs, device instance IDs, nicknames and room IDs.
Transport implementation labels are restricted to known values. GPU PCI IDs
identify models, not individual devices. Reports still contain technical system
metadata and timing information.

This evidence separates signaling failure, NAT connectivity failure, secure
transport failure, missing media, decoder failure and presentation failure. It
cannot prove remote firewall rules, replace a crash dump when the process dies
before export, or establish full Windows 7 support from a working UI.

The production Windows runtime supplies public STUN discovery when no explicit
ICE servers are configured. Explicit settings and relay-only policy are retained;
injected test endpoints do not acquire public-network dependencies. STUN does not
provide TURN relay service, so restrictive NAT pairs can still fail. The decoder
tolerates a missing optional low-latency interface/property; actual supported
property failures remain visible and fatal to that decoder attempt.
