# Runtime ownership

The UI and CLI share `backend/api/RoomSession.h`. Commands enqueue bounded work;
completion and status snapshots return asynchronously. Frontend code must not
block on capture, codec, driver, signaling or network operations.

- `backend/room/qt/RoomNetwork` owns the dedicated room-network event loop.
  Admission and sockets use authenticated membership and same-origin validation.
- The signaling executor owns native WebRTC operations. Peer/session teardown
  cancels stale generations and joins owned workers before releasing resources.
- Capture publishes owned frames with source/generation metadata. Consumers retain
  resources explicitly; live rendering uses bounded latest-frame handoffs.
- Input grants bind to authenticated peers and permission epochs. Old polling
  owners cannot submit into or revoke newer grants. Focus loss pauses UI input
  and releases held states without cancelling host permissions; source changes,
  disconnect, explicit revoke and transport failure retain release safeguards.
- Native D3D presentation is a child surface, not ordinary Qt widget painting.
  Overlays clip its visible region without resizing/stopping the stream.

Legacy UDP transport and runners are diagnostic comparison targets only. Shared
Windows capture, codec, rendering and input implementations remain production
code and must not be deleted merely because they predate the modular backend.

See [room protocol](room-protocol.md), [codec ownership](../backend/media/webrtc/README.md),
[testing](testing.md) and [known limitations](known-limitations.md).
