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
- These are standalone diagnostics only; they do not start capture, preview, audio, STUN, or UDP probing.
- Live CLI bridge:
  - `--watch PORT --signal-room ROOM`
  - `--share-room PORT --signal-room ROOM`
  - `--signal-server URL` overrides the built-in Worker for diagnostics or alternate deployments.
  - `--signal-stun HOST[:PORT]` overrides the default STUN server.
  - `--signal-setup-seconds S` controls startup peer discovery.
  - Watch room peers become NAT probe targets.
  - Share room peers become UDP send targets on one local room port.
- Runtime live signaling uses periodic `join` calls as both heartbeat and peer refresh.
- Runtime live signaling sends `leave` on normal shutdown/Stop. Background polling/leave uses a short timeout so the Qt UI's graceful Stop window can finish cleanup before force-kill. The Worker deletes the room key when the last peer leaves; if a process crashes or the network path is gone, the short KV TTL remains the fallback cleanup.
- Share no longer requires Watch to be present during startup; it can wait for room peers and add them while running.
- If Share starts before Watch, the sender must create its UDP sender lazily when the first Worker peer candidate arrives. Do not tie UDP sender creation only to initial stream encoder startup.
- Watch can add newly discovered room peers as NAT probe targets while running.
- The Qt UI's default Internet path now launches the live signaling bridge directly against the built-in Worker URL `https://screenshare-signaling.bit-yeet.workers.dev` and exchanges a small `screenshare-room-v1` link that contains only the room ID. This link is not a secret.
- Room links should not set the watcher's UDP listen port. Each side publishes its own chosen UDP port through Worker candidates, which also keeps same-PC tests from binding both processes to the same port.
- Live room joins publish both `srflx` STUN/public candidates and a `host` local candidate when STUN reveals a usable local address. This keeps same-PC and same-LAN tests from depending on NAT hairpin support.
- Live integration should still use HTTP polling; no WebSockets needed yet.
- Workers KV is acceptable for the first small implementation, but it is eventually consistent. If room state races become a real problem, move live room state to Durable Objects.
- Keep room TTLs short and delete empty rooms.
