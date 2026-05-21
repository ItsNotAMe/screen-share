# Signaling Notes

## Direction

- Signaling is allowed as a lightweight coordination layer.
- Media relay is not part of the current plan.
- The first backend is `signaling-worker/`, a Cloudflare Worker using Workers KV.
- The Worker stores short-lived room membership and peer UDP candidates only.
- The Worker cannot receive UDP and does not replace STUN.

## Security Model

- Do not send room keys, room passwords, access codes, or media encryption material to the Worker.
- Normal room UX should be encrypted by default with a hidden app-generated room key.
- A visible password is optional future UX, not required for encryption.
- The Worker room ID is a lookup name, not a secret.

## Native Client Flow

1. Open one UDP socket.
2. Use STUN from that socket to learn the public UDP endpoint.
3. Join the Worker room with peer ID and UDP candidate.
4. Poll peers/heartbeat while connecting.
5. Send UDP probes to returned peer candidates.
6. Use the same UDP socket for direct encrypted video/audio once probes/feedback work.

## Implementation Notes

- Native diagnostics use `src/transport/SignalingClient.*` and WinHTTP.
- Current CLI checks:
  - `--signal-health URL`
  - `--signal-join URL --signal-room ROOM --signal-peer-id PEER --signal-candidate IP:PORT`
  - `--signal-peers URL --signal-room ROOM --signal-peer-id PEER`
  - `--signal-heartbeat URL --signal-room ROOM --signal-peer-id PEER`
  - `--signal-leave URL --signal-room ROOM --signal-peer-id PEER`
- These are standalone diagnostics only; they do not yet start capture, preview, audio, STUN, or UDP probing.
- First live bridge:
  - `--watch PORT --signal-server URL --signal-room ROOM`
  - `--share-room PORT --signal-server URL --signal-room ROOM`
  - `--signal-stun HOST[:PORT]` overrides the default STUN server.
  - `--signal-setup-seconds S` controls startup peer discovery.
  - Watch room peers become NAT probe targets.
  - Share room peers become UDP send targets on one local room port.
- Current live bridge is setup-time discovery. Next work should add continuous room polling/heartbeat while waiting/running so sharer-first rooms and late/rejoining watchers work naturally.
- Live integration should still use HTTP polling; no WebSockets needed yet.
- Workers KV is acceptable for the first small implementation, but it is eventually consistent. If room state races become a real problem, move live room state to Durable Objects.
- Keep room TTLs short and delete empty rooms.
