# Receiver media telemetry

The shared v2 runtime owns the existing `telemetry` SCTP data channel exclusively.
It remains unordered with zero retransmits inside the authenticated WebRTC peer
connection. Control/input callbacks receive only their own channels. No receiver
statistics pass through the room Worker, directory, database or service requests.

Each viewer requests local inbound-video WebRTC stats at most once per second,
with one request pending and one replaceable sample. Reports contain decoded
width/height, cumulative decoded frames, optional decoder FPS, decoder drops,
allowlisted codec implementation and mean jitter-buffer residence. A fresh frontend
renderer snapshot contributes submission/drop counters, queue size and last outcome.
See [DIAGNOSTICS.md](DIAGNOSTICS.md) for lifetimes and meaning. Multiple video
receivers or missing dimensions/counters yield unknown data. FPS remains unknown
when absent/nonfinite/out of range; measured zero is retained.

The host exposes `PeerStreamStatus.receiver`, and UI/CLI share its serialization:
`receiver.sampleState` is `fresh`, `stale` or `unknown`; `width`, `height`,
`framesDecoded`, `decodeFps` become JSON null when absent/stale. Host rows show
receiver dimensions separately from source dimensions. Inline details include
decode counts and FPS. These are receiver-reported measurements, not trusted
input authority, a settings-revision acknowledgement, remote display proof or
end-to-end latency. Fresh reports can correctly describe a stalled decoder.

## Wire and lifecycle contract

- Binary only, 26-byte V1 or 53-byte V2 header plus 1–128 connection-ID bytes;
  maximum 181 bytes. V1 is still decoded and emitted for decoder-only data.
- Magic `SVT`, version byte `1` or `2`, one-byte ID length, one-byte flags
  (bit 0: FPS; V2 bits 1/2/3: presentation, decoder drops, buffering).
- Network-order unsigned fields: sequence (64), width/height (16 each), decoded
  frames (32), FPS multiplied by 1000 (32). Absent FPS must encode zero.
- V2 appends presented/dropped (64 each), queued/outcome (8 each), decoder drops
  and mean jitter-buffer milliseconds (32 each), decoder enum (8), before the ID.
  Absent optional fields must encode zero. Counts must fit signed JSON integers;
  queue is 0–1, outcome 0–7, buffering 0–60000 ms, codec enum 0–2. No raw strings.
- Exact lengths/version/flags are checked before reading fields. Dimensions must
  be even, 2–3840 by 2–2160; FPS must be 0–240000; sequence must be nonzero.
- Reports bind to the current authenticated negotiation connection ID. Rebind
  clears sequence, measurement and receipt time. Older-generation packets and
  duplicate/out-of-order sequences cannot replace or refresh current data.
- The host inspects at most four binary messages per peer per second. Malformed
  messages consume that allowance but never consume an accepted sequence.
- Host freshness uses local monotonic receipt time, expiring at three seconds.
  No cross-machine clock subtraction or untrusted remote timestamps are used.
- Sending waits for an empty SCTP send buffer; only the latest sample is retained.
  Samples older than three seconds are not sent. Rejected sends are dropped,
  never retried every runtime tick. There is no application history/send queue.
- ICE negotiation changes replace the stats mailbox. Late stats callbacks hold
  only the retired mailbox, not peers/runtime/UI. Entry destruction unregisters
  its observer before releasing the peer. Callbacks remain on signaling and do
  not reenter WebRTC APIs from OnMessage/OnStateChange.

## Evidence and remaining work

Native runtime tests exercise roundtrip, every truncation, trailing bytes, invalid
versions/flags/dimensions/FPS, canonical absent values, replay, exact expiry,
generation changes and flood admission. Collector tests check real stats objects,
missing reports and mailbox isolation. UI/CLI scenarios verify decoded dimensions
after live settings changes. The four-viewer proof verifies fresh reports, expiry
while media continues, resumption and ICE restart; existing rejoin/retirement
coverage remains active.

Remote presentation/drop/buffering and codec observations are integrated and tested
through actual UI/CLI; sender network reasons are documented in NETWORK-DIAGNOSTICS.md.
Input timing, capture/presentation rate instrumentation, encoder pending age and
external latency remain unavailable. Receiver reports must not be used to claim
external latency or overall performance gains. New V2 reports are ignored by older
V1-only hosts; mixed-version telemetry is not an application compatibility promise.
