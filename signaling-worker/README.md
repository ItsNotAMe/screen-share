# ScreenShare Signaling Worker

This is a minimal Cloudflare Worker signaling backend for ScreenShare.

It does **not** relay video, audio, or input. Its only job is to help native
C++ clients find each other's UDP candidates for direct peer-to-peer sessions.

## Architecture

ScreenShare keeps the media path native and direct:

```text
C++ client -> STUN -> public UDP candidate
C++ client -> Worker HTTPS room API -> peer candidate exchange
C++ client <-> C++ client direct UDP probes
C++ client <-> C++ client encrypted UDP video/audio
```

The Worker stores short-lived room membership in Workers KV through the `ROOMS`
binding. Room entries expire quickly and stale peers are removed after 60
seconds.

## Why Signaling Is Needed

STUN tells one client what public UDP endpoint its NAT created, but it does not
tell the other client where to send probes. Signaling provides that missing
coordination step:

- who is in the room
- what public UDP candidates they learned from STUN
- whether a peer is still alive

## NAT Hole Punching Flow

The intended native client flow is:

1. Client opens **one UDP socket**.
2. Client discovers its public UDP endpoint using STUN from that same socket.
3. Client joins a Worker room over HTTPS with its peer ID and UDP candidate.
4. Worker returns other peers in the room.
5. Client sends UDP probes to each returned peer candidate.
6. Peers simultaneously probe each other.
7. NAT hole punching succeeds when both NATs allow the peer packets.
8. Direct UDP P2P begins.
9. Screen-sharing traffic bypasses the Worker entirely.

STUN is still required. The Cloudflare Worker cannot receive UDP packets and
cannot perform the STUN step for the client.

## Security Model

The Worker should never receive the media encryption key, room password, or any
raw user secret.

The product direction is:

- Rooms are encrypted by default.
- The native app generates a hidden room key automatically.
- Normal users should not see an "access code" field.
- A visible room password can be added later as an optional extra lock.
- The room ID used with this Worker is only a lookup name, not the encryption
  secret.

The Worker validates request shape and candidate values, but it does not decide
who can decrypt media. UDP media encryption remains end-to-end in the C++ app.

## API

All responses are JSON. CORS is enabled for simple tooling and diagnostics.

### `GET /health`

Returns:

```json
{ "ok": true }
```

### `POST /rooms/:roomId/join`

Upserts this peer, cleans stale peers, and returns every other peer in the room.

```json
{
  "peerId": "alice",
  "candidates": [
    {
      "type": "srflx",
      "ip": "1.2.3.4",
      "port": 62000,
      "protocol": "udp"
    }
  ],
  "metadata": {
    "name": "Alice",
    "platform": "windows"
  }
}
```

Response:

```json
{
  "ok": true,
  "peers": []
}
```

### `GET /rooms/:roomId/peers?peerId=alice`

Returns all peers except `alice` after stale-peer cleanup.

### `POST /rooms/:roomId/heartbeat`

Updates `lastSeen` for a joined peer.

```json
{ "peerId": "alice" }
```

### `POST /rooms/:roomId/leave`

Removes a peer from the room. Empty rooms are deleted from KV.

```json
{ "peerId": "alice" }
```

## Deploy

Install dependencies:

```powershell
npm install
```

From the repository root, you can also install Node.js LTS and these npm dependencies with:

```powershell
.\scripts\install-dev-deps.ps1 -WorkerOnly
```

Create the KV namespace:

```powershell
npx wrangler kv namespace create ROOMS
```

Copy the generated namespace ID into `wrangler.toml`:

```toml
[[kv_namespaces]]
binding = "ROOMS"
id = "your-namespace-id"
```

Deploy:

```powershell
npx wrangler deploy
```

For local development:

```powershell
npx wrangler dev
```

## Example Curl

```bash
curl https://YOUR_WORKER.workers.dev/health
```

```bash
curl -X POST https://YOUR_WORKER.workers.dev/rooms/test-room/join \
  -H "Content-Type: application/json" \
  -d '{
    "peerId": "alice",
    "candidates": [
      { "type": "srflx", "ip": "1.2.3.4", "port": 62000, "protocol": "udp" }
    ],
    "metadata": { "name": "Alice", "platform": "windows" }
  }'
```

```bash
curl "https://YOUR_WORKER.workers.dev/rooms/test-room/peers?peerId=alice"
```

## Notes

- No WebSockets are needed for the first version; simple HTTP polling is enough.
- Workers KV is eventually consistent. It is acceptable for this first small
  signaling layer, but Durable Objects would be a better fit if we later need
  strongly consistent room state.
- The Worker is intentionally low bandwidth: short JSON requests, short room TTL,
  and heartbeat cleanup.
