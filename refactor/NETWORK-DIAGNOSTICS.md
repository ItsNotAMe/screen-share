# Per-viewer network and sender diagnostics

The existing one-Hz host GetStats request now supplies a typed
`SenderVideoObservation` in addition to transport upload. No extra stats request,
server call, bandwidth controller or adaptation policy was introduced.

The collector reports:

- Video RTP payload rate from outbound video byte deltas. This is separate from
  the video allocation/applied ceiling and total WebRTC transport rate; it is not
  physical-interface throughput. WebRTC's counter may include retransmitted payload.
- Encoded FPS from outbound video statistics, separate from source and decode FPS.
- RTT and estimated available outgoing bitrate from the selected ICE pair only.
  RTT is not image, input or end-to-end latency. Available bandwidth is an estimate,
  not an upload setting or a guaranteed available rate.
- Video loss fraction and jitter from the outbound stream's linked remote-inbound
  RTCP report. These are the last exposed RTCP measurements, not an application
  loss detector. No candidate addresses, IDs, SDP or raw encoder strings are exported.
- WebRTC's explicit quality-limitation reason: none, cpu, bandwidth, other, unknown.
  A low payload rate alone never implies congestion; a static desktop can be cheap.

## Validation and ownership

All measurements are optional. Missing, nonfinite or out-of-range numeric fields
remain unknown; zero stays zero. Unrecognized reasons stay unknown. Multiple
outbound video streams or selected transports are ambiguous and their respective
measurements stay unknown. Counters reset independently when stream IDs change,
decrease, disappear or timestamps fail to advance. Implausible rates above 1 Tbps
are rejected rather than risking overflow. The local sample expires at three
seconds and clears all sender measurements/reasons together.

Each negotiated connection ID gets a new mailbox, including ICE restarts. Late
callbacks can only update their retired mailbox. Collector callbacks retain no
peer/runtime/UI. The runtime copies typed values into its public snapshot.

Receiver report age is independently computed from local monotonic receipt time
in whole seconds. It remains available when decoded values expire. It is not a
cross-machine clock measurement or evidence of continued frame progress.

## UI and CLI

`PeerDiagnosticsWidget` owns the table, inline details and one nonmodal details
dialog. It accepts immutable room snapshots and owns no media, timers or requests.
The dialog is pinned to a peer ID, stays live as settings/measurements change,
and clears measurements when that peer leaves or the session ends. Duplicate
member names include their peer IDs; names never become authentication identities.
Changing the host capacity above four shows an upload/encoding-cost warning.

CLI peer status includes `sender` with videoPayloadBps, encodedFps, rttMs, jitterMs,
lossFraction, availableOutgoingBps and limitingReason. Unavailable numbers are
JSON null. `receiver.ageSeconds` is null before the first report and numeric
afterward. Both frontends share serialization and unknown/stale conventions.

Remaining scope: remote presentation/drop/buffering telemetry, codec/fallback
details, capture timing, gaming input measurements and externally measured
end-to-end latency. This work does not complete default-shell adoption or certify
hardware, network impairment, resource or performance acceptance.
