export interface Env {
  ROOMS: KVNamespace;
}

type CandidateType = "host" | "srflx";
type CandidateProtocol = "udp";

interface Candidate {
  type: CandidateType;
  ip: string;
  port: number;
  protocol: CandidateProtocol;
}

interface PeerMetadata {
  name?: string;
  platform?: string;
}

interface Peer {
  peerId: string;
  candidates: Candidate[];
  metadata?: PeerMetadata;
  lastSeen: number;
}

interface Room {
  peers: Peer[];
}

interface JoinRequest {
  peerId: unknown;
  candidates: unknown;
  metadata?: unknown;
}

interface PeerRequest {
  peerId: unknown;
}

const STALE_PEER_MS = 60_000;
const ROOM_TTL_SECONDS = 180;
const MAX_JSON_BYTES = 16 * 1024;
const MAX_CANDIDATES = 8;
const RATE_LIMIT_WINDOW_MS = 60_000;
const RATE_LIMIT_MAX_REQUESTS = 240;

const rateBuckets = new Map<string, { windowStart: number; count: number }>();

class HttpError extends Error {
  constructor(
    readonly status: number,
    readonly code: string,
    message: string,
  ) {
    super(message);
  }
}

function corsHeaders(): HeadersInit {
  return {
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type",
    "Access-Control-Max-Age": "86400",
    "Cache-Control": "no-store",
  };
}

function jsonResponse(data: unknown, status = 200): Response {
  return new Response(JSON.stringify(data), {
    status,
    headers: {
      ...corsHeaders(),
      "Content-Type": "application/json; charset=utf-8",
    },
  });
}

function normalizePath(pathname: string): string {
  return pathname.replace(/\/+$/, "") || "/";
}

function roomKey(roomId: string): string {
  return `room:${roomId}`;
}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function validateRoomId(roomId: string): string {
  const value = roomId.trim();
  if (!/^[A-Za-z0-9_-]{3,96}$/.test(value)) {
    throw new HttpError(400, "invalid_room_id", "Room IDs must be 3-96 characters: letters, numbers, underscore, or dash.");
  }
  return value;
}

function validatePeerId(peerId: unknown): string {
  if (typeof peerId !== "string") {
    throw new HttpError(400, "invalid_peer_id", "peerId must be a string.");
  }
  const value = peerId.trim();
  if (!/^[A-Za-z0-9_-]{1,96}$/.test(value)) {
    throw new HttpError(400, "invalid_peer_id", "peerId must be 1-96 characters: letters, numbers, underscore, or dash.");
  }
  return value;
}

function isValidIpv4(ip: string): boolean {
  const parts = ip.split(".");
  if (parts.length !== 4) {
    return false;
  }
  return parts.every((part) => {
    if (!/^\d{1,3}$/.test(part)) {
      return false;
    }
    const value = Number(part);
    return Number.isInteger(value) && value >= 0 && value <= 255 && String(value) === part.replace(/^0+(?=\d)/, "");
  });
}

function isLikelyIpv6(ip: string): boolean {
  return ip.includes(":") && /^[0-9A-Fa-f:.]{2,45}$/.test(ip);
}

function validateCandidate(candidate: unknown): Candidate {
  if (!isObject(candidate)) {
    throw new HttpError(400, "invalid_candidate", "Candidate must be an object.");
  }
  if (candidate.type !== "srflx" && candidate.type !== "host") {
    throw new HttpError(400, "invalid_candidate_type", "Only srflx/host UDP candidates are supported.");
  }
  if (candidate.protocol !== "udp") {
    throw new HttpError(400, "invalid_candidate_protocol", "Only UDP candidates are supported.");
  }
  if (typeof candidate.ip !== "string" || candidate.ip.length > 45) {
    throw new HttpError(400, "invalid_candidate_ip", "Candidate IP must be a valid IPv4 or IPv6 address.");
  }
  const ip = candidate.ip.trim();
  if (!isValidIpv4(ip) && !isLikelyIpv6(ip)) {
    throw new HttpError(400, "invalid_candidate_ip", "Candidate IP must be a valid IPv4 or IPv6 address.");
  }
  const port = candidate.port;
  if (typeof port !== "number" || !Number.isInteger(port) || port < 1 || port > 65535) {
    throw new HttpError(400, "invalid_candidate_port", "Candidate port must be 1-65535.");
  }
  return {
    type: candidate.type,
    ip,
    port,
    protocol: "udp",
  };
}

function validateCandidates(candidates: unknown): Candidate[] {
  if (!Array.isArray(candidates) || candidates.length === 0 || candidates.length > MAX_CANDIDATES) {
    throw new HttpError(400, "invalid_candidates", `candidates must contain 1-${MAX_CANDIDATES} UDP candidates.`);
  }
  const unique = new Map<string, Candidate>();
  for (const candidate of candidates) {
    const validated = validateCandidate(candidate);
    unique.set(`${validated.protocol}:${validated.ip}:${validated.port}`, validated);
  }
  return [...unique.values()];
}

function validateMetadata(metadata: unknown): PeerMetadata | undefined {
  if (metadata === undefined) {
    return undefined;
  }
  if (!isObject(metadata)) {
    throw new HttpError(400, "invalid_metadata", "metadata must be an object.");
  }

  const clean: PeerMetadata = {};
  if (metadata.name !== undefined) {
    if (typeof metadata.name !== "string" || metadata.name.length > 80) {
      throw new HttpError(400, "invalid_metadata", "metadata.name must be a short string.");
    }
    clean.name = metadata.name.trim();
  }
  if (metadata.platform !== undefined) {
    if (typeof metadata.platform !== "string" || metadata.platform.length > 80) {
      throw new HttpError(400, "invalid_metadata", "metadata.platform must be a short string.");
    }
    clean.platform = metadata.platform.trim();
  }
  return Object.keys(clean).length > 0 ? clean : undefined;
}

async function readJson<T>(request: Request): Promise<T> {
  const contentType = request.headers.get("Content-Type") ?? "";
  if (!contentType.toLowerCase().includes("application/json")) {
    throw new HttpError(415, "unsupported_media_type", "Use Content-Type: application/json.");
  }
  const contentLength = Number(request.headers.get("Content-Length") ?? "0");
  if (Number.isFinite(contentLength) && contentLength > MAX_JSON_BYTES) {
    throw new HttpError(413, "body_too_large", "Request body is too large.");
  }

  const text = await request.text();
  if (text.length > MAX_JSON_BYTES) {
    throw new HttpError(413, "body_too_large", "Request body is too large.");
  }
  try {
    return JSON.parse(text) as T;
  } catch {
    throw new HttpError(400, "invalid_json", "Request body must be valid JSON.");
  }
}

async function loadRoom(env: Env, roomId: string): Promise<Room> {
  const stored = await env.ROOMS.get<Room>(roomKey(roomId), "json");
  if (!stored || !Array.isArray(stored.peers)) {
    return { peers: [] };
  }
  return {
    peers: stored.peers.filter((peer) => typeof peer.peerId === "string" && Array.isArray(peer.candidates)),
  };
}

async function saveRoom(env: Env, roomId: string, room: Room): Promise<void> {
  if (room.peers.length === 0) {
    await env.ROOMS.delete(roomKey(roomId));
    return;
  }
  await env.ROOMS.put(roomKey(roomId), JSON.stringify(room), {
    expirationTtl: ROOM_TTL_SECONDS,
  });
}

function cleanupPeers(room: Room, now = Date.now()): Room {
  return {
    peers: room.peers.filter((peer) => now - peer.lastSeen <= STALE_PEER_MS),
  };
}

function upsertPeer(room: Room, peer: Peer): Room {
  const peers = room.peers.filter((existing) => existing.peerId !== peer.peerId);
  peers.push(peer);
  return { peers };
}

function otherPeers(room: Room, peerId: string): Peer[] {
  return room.peers.filter((peer) => peer.peerId !== peerId);
}

function applyRateLimit(request: Request): void {
  const now = Date.now();
  const ip = request.headers.get("CF-Connecting-IP") ?? request.headers.get("X-Forwarded-For") ?? "unknown";
  const bucket = rateBuckets.get(ip);
  if (!bucket || now - bucket.windowStart >= RATE_LIMIT_WINDOW_MS) {
    rateBuckets.set(ip, { windowStart: now, count: 1 });
  } else {
    bucket.count += 1;
    if (bucket.count > RATE_LIMIT_MAX_REQUESTS) {
      throw new HttpError(429, "rate_limited", "Too many signaling requests.");
    }
  }

  if (rateBuckets.size > 10_000) {
    for (const [key, value] of rateBuckets) {
      if (now - value.windowStart >= RATE_LIMIT_WINDOW_MS) {
        rateBuckets.delete(key);
      }
    }
  }
}

async function handleJoin(request: Request, env: Env, roomId: string): Promise<Response> {
  const body = await readJson<JoinRequest>(request);
  const peerId = validatePeerId(body.peerId);
  const candidates = validateCandidates(body.candidates);
  const metadata = validateMetadata(body.metadata);
  const now = Date.now();

  let room = cleanupPeers(await loadRoom(env, roomId), now);
  room = upsertPeer(room, { peerId, candidates, metadata, lastSeen: now });
  await saveRoom(env, roomId, room);

  return jsonResponse({ ok: true, peers: otherPeers(room, peerId) });
}

async function handlePeers(request: Request, env: Env, roomId: string): Promise<Response> {
  const url = new URL(request.url);
  const peerId = validatePeerId(url.searchParams.get("peerId"));
  const room = cleanupPeers(await loadRoom(env, roomId));
  await saveRoom(env, roomId, room);

  return jsonResponse({ ok: true, peers: otherPeers(room, peerId) });
}

async function handleHeartbeat(request: Request, env: Env, roomId: string): Promise<Response> {
  const body = await readJson<PeerRequest>(request);
  const peerId = validatePeerId(body.peerId);
  const now = Date.now();
  const room = cleanupPeers(await loadRoom(env, roomId), now);
  const peer = room.peers.find((existing) => existing.peerId === peerId);
  if (!peer) {
    await saveRoom(env, roomId, room);
    throw new HttpError(404, "peer_not_found", "Peer is not in this room.");
  }
  peer.lastSeen = now;
  await saveRoom(env, roomId, room);

  return jsonResponse({ ok: true });
}

async function handleLeave(request: Request, env: Env, roomId: string): Promise<Response> {
  const body = await readJson<PeerRequest>(request);
  const peerId = validatePeerId(body.peerId);
  const room = cleanupPeers(await loadRoom(env, roomId));
  const nextRoom = { peers: room.peers.filter((peer) => peer.peerId !== peerId) };
  await saveRoom(env, roomId, nextRoom);

  return jsonResponse({ ok: true });
}

async function routeRequest(request: Request, env: Env): Promise<Response> {
  if (request.method === "OPTIONS") {
    return new Response(null, { status: 204, headers: corsHeaders() });
  }

  applyRateLimit(request);

  const url = new URL(request.url);
  const pathname = normalizePath(url.pathname);

  if (pathname === "/health" && request.method === "GET") {
    return jsonResponse({ ok: true });
  }

  const match = pathname.match(/^\/rooms\/([^/]+)\/(join|peers|heartbeat|leave)$/);
  if (!match) {
    throw new HttpError(404, "not_found", "Endpoint not found.");
  }

  const roomId = validateRoomId(decodeURIComponent(match[1]));
  const action = match[2];

  if (action === "join" && request.method === "POST") {
    return handleJoin(request, env, roomId);
  }
  if (action === "peers" && request.method === "GET") {
    return handlePeers(request, env, roomId);
  }
  if (action === "heartbeat" && request.method === "POST") {
    return handleHeartbeat(request, env, roomId);
  }
  if (action === "leave" && request.method === "POST") {
    return handleLeave(request, env, roomId);
  }

  throw new HttpError(405, "method_not_allowed", "Method not allowed for this endpoint.");
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    try {
      return await routeRequest(request, env);
    } catch (error) {
      if (error instanceof HttpError) {
        return jsonResponse({ ok: false, error: error.code, message: error.message }, error.status);
      }
      console.error("Unhandled signaling error", error);
      return jsonResponse({ ok: false, error: "internal_error", message: "Internal server error." }, 500);
    }
  },
};
