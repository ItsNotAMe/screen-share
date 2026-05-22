# ScreenShare Signaling Worker

This is a minimal Cloudflare Worker signaling backend for ScreenShare.

It does **not** relay video, audio, or input. Its only job is to help native
C++ clients find each other's UDP candidates for direct peer-to-peer sessions.

## Architecture

ScreenShare keeps the media path native and direct:

```text
C++ client -> STUN -> public UDP candidate
C++ client -> Worker HTTPS/WebSocket room API -> peer candidate exchange
C++ client <-> C++ client direct UDP probes
C++ client <-> C++ client encrypted UDP video/audio
```

The Worker stores live room membership and safe room metadata in per-room Durable Objects, and stores
the browseable active-room list in a small directory Durable Object. Each room has one ordered state
holder, which avoids the eventual-consistency and last-writer-wins races that KV has during rapid
join/poll cycles. Peer entries are still stale-cleaned after 60 seconds.

KV is retained only for legacy/fallback storage. It should not contain peer UDP
candidates, plaintext room passwords, access codes, or user-visible secrets.

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
5. Client opens the room event WebSocket for peer join/update notifications.
6. Client sends UDP probes to each returned or newly announced peer candidate.
7. Peers simultaneously probe each other.
8. NAT hole punching succeeds when both NATs allow the peer packets.
9. Direct UDP P2P begins.
10. Screen-sharing traffic bypasses the Worker entirely.

STUN is still required. The Cloudflare Worker cannot receive UDP packets and
cannot perform the STUN step for the client.

## Security Model

For no-password public rooms, the Durable Object creates and stores the random
UDP room access key that native clients use for automatic encryption. For locked
rooms, clients send the room password over HTTPS during join; the Durable Object
stores a salted PBKDF2 verifier, rejects wrong passwords before returning the
room access key, and never stores the plaintext password. Native clients also
mix the password into the UDP key locally.

The product direction is:

- Rooms are encrypted by default.
- The Durable Object generates a random automatic UDP encryption key for no-password public rooms.
- Normal users should not see an "access code" field.
- A visible room password is an optional extra lock. It is sent to the Worker only over HTTPS for verification and is not stored as plaintext.
- The room ID used with this Worker is public room metadata, not a private access-control secret.

The Worker validates request shape and candidate values. No-password rooms are
public rooms: any client that joins the room can receive the room access key.
Optional passwords are the private-room layer; the Worker advertises only whether
a password is required, and rejects incorrect passwords before adding the peer to
the room.

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
    },
    {
      "type": "host",
      "ip": "192.168.1.23",
      "port": 5000,
      "protocol": "udp"
    }
  ],
  "metadata": {
    "name": "Alice",
    "platform": "windows"
  },
  "room": {
    "name": "Movie night",
    "password": "optional-room-password",
    "passwordProtected": true
  }
}
```

Response:

```json
{
  "ok": true,
  "roomAccessKey": "random-public-room-access-key",
  "roomName": "Movie night",
  "passwordProtected": true,
  "peers": []
}
```

### `GET /rooms/:roomId/peers?peerId=alice`

Returns the room access key, safe room metadata, and all peers except `alice` after stale-peer cleanup.
For locked rooms, send `X-ScreenShare-Room-Password` with the percent-encoded password; wrong or
missing passwords are rejected.

### `GET /rooms/:roomId/events?peerId=alice`

Upgrades to a WebSocket for room change notifications. For locked rooms, add
`X-ScreenShare-Room-Password` with the percent-encoded password; wrong or missing passwords are
rejected before the socket opens.

The first message is a `hello` event with the currently known peers. Later messages are
`peer_joined`, `peer_updated`, or `peer_left`. Native clients still keep a slower HTTP `join`
heartbeat/fallback, but they no longer need to poll every few seconds to discover late peers.

```json
{
  "ok": true,
  "type": "peer_joined",
  "roomId": "test-room",
  "peer": {
    "peerId": "bob",
    "candidates": [
      { "type": "srflx", "ip": "5.6.7.8", "port": 62001, "protocol": "udp" }
    ],
    "metadata": { "name": "Bob", "platform": "windows" },
    "lastSeen": 1760000000000
  }
}
```

### `GET /rooms?limit=100`

Returns browseable active-room summaries from the directory Durable Object after
verifying each listed room against its room Durable Object. This is a
directory/debug view, not the source of truth for signaling. Locked rooms are
listed as locked, but the password is never included.

```json
{
  "ok": true,
  "rooms": [
    {
      "roomId": "test-room",
      "name": "Movie night",
      "peerCount": 2,
      "updatedAt": 1760000000000,
      "expiresAt": 1760000180000,
      "requiresRoomKey": true,
      "passwordProtected": true
    }
  ]
}
```

### `GET /rooms/:roomId/summary`

Returns one safe active-room summary, or `null` if the room has no live peers.
This endpoint does not return peer candidates or user passwords.

```json
{
  "ok": true,
  "room": null
}
```

### `POST /rooms/:roomId/heartbeat`

Updates `lastSeen` for a joined peer.

```json
{ "peerId": "alice" }
```

### `POST /rooms/:roomId/leave`

Removes a peer from the room. Empty rooms clear their Durable Object room state.

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

Copy the generated namespace ID into `wrangler.toml`. The Durable Object binding
and migration are already in `wrangler.toml`:

```toml
[[kv_namespaces]]
binding = "ROOMS"
id = "your-namespace-id"

[[durable_objects.bindings]]
name = "ROOM_OBJECTS"
class_name = "RoomObject"

[[durable_objects.bindings]]
name = "ROOM_DIRECTORY"
class_name = "RoomDirectoryObject"

[[migrations]]
tag = "v1"
new_sqlite_classes = ["RoomObject"]

[[migrations]]
tag = "v2"
new_sqlite_classes = ["RoomDirectoryObject"]
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
      { "type": "srflx", "ip": "1.2.3.4", "port": 62000, "protocol": "udp" },
      { "type": "host", "ip": "192.168.1.23", "port": 5000, "protocol": "udp" }
    ],
    "metadata": { "name": "Alice", "platform": "windows" }
  }'
```

```bash
curl "https://YOUR_WORKER.workers.dev/rooms/test-room/peers?peerId=alice"
```

```bash
curl "https://YOUR_WORKER.workers.dev/rooms"
```

## Notes

- Room events use WebSockets so peers do not need short-interval HTTP polling.
- Workers KV is eventually consistent. Live room membership uses a Durable
  Object because signaling needs immediate peer visibility.
- The native room-list view uses a directory Durable Object for immediate active
  room summaries. Summaries may contain a friendly room name and lock flag, but
  never passwords or peer candidates.
- The Worker is intentionally low bandwidth: short JSON requests, short room TTL,
  and heartbeat cleanup.
