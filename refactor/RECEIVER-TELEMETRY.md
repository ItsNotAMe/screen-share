# Receiver decoder telemetry

The shared v2 runtime owns the existing `telemetry` SCTP data channel exclusively.
It remains unordered with zero retransmits inside the authenticated WebRTC peer
connection. Control/input callbacks receive only their own channels. No receiver
statistics pass through the room Worker, directory, database or service requests.

Each viewer requests local inbound-video WebRTC stats at most once per second,
with one request pending and one replaceable sample. Reports contain decoded
width/height, cumulative decoded frames and optional decoder FPS. Multiple video
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

- Binary only, 26-byte header plus 1–128 connection-ID bytes; maximum 154 bytes.
- Magic `SVT`, version byte `1`, one-byte ID length, one-byte flags (bit 0: FPS).
- Network-order unsigned fields: sequence (64), width/height (16 each), decoded
  frames (32), FPS multiplied by 1000 (32). Absent FPS must encode zero.
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

Remaining telemetry includes remote presentation counters, input latency, network
limiting reasons and any additional metrics required by PLAN.md. These decoder
reports must not be used to claim external latency or overall performance gains.
