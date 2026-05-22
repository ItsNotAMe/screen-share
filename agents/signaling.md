# Signaling Notes

## Direction

- Signaling is allowed as a lightweight coordination layer.
- Media relay is not part of the current plan.
- The first backend is `signaling-worker/`, a Cloudflare Worker using per-room Durable Objects for live room state and a small directory Durable Object for the browseable active-room list.
- The Worker stores short-lived room membership and peer UDP candidates in the room Durable Object only.
- Active-room directory entries contain safe summaries: room ID, peer count, timestamps, and whether an extra room secret is required.
- Safe room metadata now includes optional friendly room name and a `passwordProtected` lock flag. Locked rooms store a salted password verifier in the Durable Object; plaintext passwords are sent only over HTTPS during join and are not stored.
- The Worker cannot receive UDP and does not replace STUN.

## Security Model

- Do not store room passwords or user-visible access codes in the Worker.
- Normal no-password room UX gets a random room access key from the Durable Object and uses it as a hidden UDP access code. This is public-room encryption, not private room access control.
- A visible room password is optional. The Worker verifies it before returning the room access key, and the native app also mixes it locally into the hidden room access key before UDP key derivation.
- The Worker room ID is a lookup name, not a secret.

## Native Client Flow

1. Open one UDP socket.
2. Use STUN from that socket to learn the public UDP endpoint.
3. Join the Worker room with peer ID and UDP candidate.
4. Open the room event WebSocket for peer join/update notifications.
5. Keep a slower HTTP `join` heartbeat/fallback while connecting.
6. Send UDP probes to returned or newly announced peer candidates.
7. Use the same UDP socket for direct encrypted video/audio once probes/feedback work.

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
  - `--signal-setup-seconds S` is legacy compatibility; startup now does one join and runtime events handle late peers.
  - Watch room peers become NAT probe targets.
  - Share room peers become UDP send targets on one local room port.
- Runtime live signaling opens `GET /rooms/:roomId/events?peerId=...` as a WebSocket for room updates and keeps periodic `join` calls only as heartbeat/fallback.
- Runtime live signaling sends `leave` on normal shutdown/Stop. Background polling/leave uses a short timeout so the Qt UI's graceful Stop window can finish cleanup before force-kill. The Worker removes the current peer from the room Durable Object on leave; if a process crashes or the network path is gone, stale-peer cleanup remains the fallback.
- `GET /rooms` reads the active-room directory Durable Object and verifies listed rooms against their room Durable Objects for browse/debug UI. It is not the signaling source of truth and does not return peer candidates or passwords.
- `GET /rooms/:roomId/summary` returns one safe summary or `null`; it is useful for pruning stale directory entries.
- `POST /rooms/:roomId/join` can receive `room.name`, `room.passwordProtected`, and `room.password`; the Worker must store only a salted verifier and reject wrong passwords before adding the peer.
- Share no longer requires Watch to be present during startup; it can wait for room peers and add them while running.
- If Share starts before Watch, the sender must create its UDP sender lazily when the first Worker peer candidate arrives. Do not tie UDP sender creation only to initial stream encoder startup.
- Watch can add newly discovered room peers as NAT probe targets while running.
- The Qt UI's default Internet path now launches the live signaling bridge directly against the built-in Worker URL `https://screenshare-signaling.bit-yeet.workers.dev`, copies short `screenshare-room-v1;room=...` links, and can also join from the active room list. The Worker sees room IDs/candidates and distributes a random room access key after optional password verification.
- Room links should not set the watcher's UDP listen port. Each side publishes its own chosen UDP port through Worker candidates, which also keeps same-PC tests from binding both processes to the same port.
- Live room joins publish both `srflx` STUN/public candidates and a `host` local candidate when STUN reveals a usable local address. This keeps same-PC and same-LAN tests from depending on NAT hairpin support.
- Room event WebSockets replace the old every-few-seconds peer polling path; HTTP remains the fallback and room-state source of truth.
- Workers KV was too eventually consistent for rapid room join/poll cycles. Live room state now uses Durable Objects to avoid asymmetric "Watch sees Share, Share sees nobody" reports.
- Keep room TTLs short, delete empty rooms, and keep the directory Durable Object pruned.
