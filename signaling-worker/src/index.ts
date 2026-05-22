import { DurableObject } from "cloudflare:workers";

export interface Env {
  ROOMS: KVNamespace;
  ROOM_OBJECTS: DurableObjectNamespace<RoomObject>;
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

interface RoomSummary {
  roomId: string;
  peerCount: number;
  updatedAt: number;
  expiresAt: number;
  requiresRoomKey: true;
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
const ROOM_DIRECTORY_PREFIX = "active-room:";
const MAX_JSON_BYTES = 16 * 1024;
const MAX_CANDIDATES = 8;
const MAX_ROOM_LIST_LIMIT = 250;
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

function roomPeerPrefix(roomId: string): string {
  return `room:${roomId}:peer:`;
}

function roomPeerKey(roomId: string, peerId: string): string {
  return `${roomPeerPrefix(roomId)}${peerId}`;
}

function roomDirectoryKey(roomId: string): string {
  return `${ROOM_DIRECTORY_PREFIX}${roomId}`;
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

function normalizeStoredPeer(value: unknown): Peer | undefined {
  if (!isObject(value) || typeof value.peerId !== "string" || !Array.isArray(value.candidates)) {
    return undefined;
  }

  const candidates: Candidate[] = [];
  for (const candidate of value.candidates) {
    try {
      candidates.push(validateCandidate(candidate));
    } catch {
      return undefined;
    }
  }
  if (candidates.length === 0) {
    return undefined;
  }

  let metadata: PeerMetadata | undefined;
  try {
    metadata = validateMetadata(value.metadata);
  } catch {
    return undefined;
  }

  const lastSeen = typeof value.lastSeen === "number" && Number.isFinite(value.lastSeen) ? value.lastSeen : 0;
  return {
    peerId: value.peerId,
    candidates,
    metadata,
    lastSeen,
  };
}

async function loadRoom(env: Env, roomId: string, now = Date.now()): Promise<Room> {
  const listed = await env.ROOMS.list({ prefix: roomPeerPrefix(roomId), limit: 1000 });
  const peers: Peer[] = [];
  const deletes: Promise<void>[] = [];

  await Promise.all(listed.keys.map(async (key) => {
    const stored = await env.ROOMS.get<unknown>(key.name, "json");
    const peer = normalizeStoredPeer(stored);
    if (!peer || now - peer.lastSeen > STALE_PEER_MS) {
      deletes.push(env.ROOMS.delete(key.name));
      return;
    }
    peers.push(peer);
  }));

  if (deletes.length > 0) {
    await Promise.all(deletes);
  }

  return { peers };
}

async function savePeer(env: Env, roomId: string, peer: Peer): Promise<void> {
  await env.ROOMS.put(roomPeerKey(roomId, peer.peerId), JSON.stringify(peer), {
    expirationTtl: ROOM_TTL_SECONDS,
  });
}

async function deletePeer(env: Env, roomId: string, peerId: string): Promise<void> {
  await env.ROOMS.delete(roomPeerKey(roomId, peerId));
}

async function cleanupLegacyRoom(env: Env, roomId: string): Promise<void> {
  await env.ROOMS.delete(roomKey(roomId));
}

function otherPeers(room: Room, peerId: string): Peer[] {
  return room.peers.filter((peer) => peer.peerId !== peerId);
}

function roomSummary(roomId: string, peers: Peer[], now = Date.now()): RoomSummary | undefined {
  const activePeers = peers.filter((peer) => now - peer.lastSeen <= STALE_PEER_MS);
  if (activePeers.length === 0) {
    return undefined;
  }
  const updatedAt = Math.max(...activePeers.map((peer) => peer.lastSeen));
  return {
    roomId,
    peerCount: activePeers.length,
    updatedAt,
    expiresAt: updatedAt + ROOM_TTL_SECONDS * 1000,
    requiresRoomKey: true,
  };
}

function normalizeRoomSummary(value: unknown, now = Date.now()): RoomSummary | undefined {
  if (!isObject(value) ||
      typeof value.roomId !== "string" ||
      typeof value.peerCount !== "number" ||
      typeof value.updatedAt !== "number" ||
      typeof value.expiresAt !== "number" ||
      value.requiresRoomKey !== true) {
    return undefined;
  }
  try {
    validateRoomId(value.roomId);
  } catch {
    return undefined;
  }
  if (!Number.isInteger(value.peerCount) || value.peerCount < 1 ||
      !Number.isFinite(value.updatedAt) || !Number.isFinite(value.expiresAt) ||
      value.expiresAt <= now) {
    return undefined;
  }
  return {
    roomId: value.roomId,
    peerCount: value.peerCount,
    updatedAt: value.updatedAt,
    expiresAt: value.expiresAt,
    requiresRoomKey: true,
  };
}

async function syncRoomDirectory(env: Env, roomId: string, peers: Peer[], now = Date.now()): Promise<void> {
  const summary = roomSummary(roomId, peers, now);
  const key = roomDirectoryKey(roomId);
  if (!summary) {
    await env.ROOMS.delete(key);
    return;
  }
  await env.ROOMS.put(key, JSON.stringify(summary), {
    expirationTtl: ROOM_TTL_SECONDS,
  });
}

async function trySyncRoomDirectory(env: Env, roomId: string, peers: Peer[], now = Date.now()): Promise<void> {
  try {
    await syncRoomDirectory(env, roomId, peers, now);
  } catch (error) {
    console.error("Failed to sync room directory", error);
  }
}

async function handleRoomList(request: Request, env: Env): Promise<Response> {
  const url = new URL(request.url);
  const rawLimit = Number(url.searchParams.get("limit") ?? "100");
  const limit = Number.isInteger(rawLimit) && rawLimit > 0
    ? Math.min(rawLimit, MAX_ROOM_LIST_LIMIT)
    : 100;
  const now = Date.now();
  const listed = await env.ROOMS.list({ prefix: ROOM_DIRECTORY_PREFIX, limit });
  const rooms: RoomSummary[] = [];
  const deletes: Promise<void>[] = [];

  await Promise.all(listed.keys.map(async (key) => {
    const stored = await env.ROOMS.get<unknown>(key.name, "json");
    const summary = normalizeRoomSummary(stored, now);
    if (!summary) {
      deletes.push(env.ROOMS.delete(key.name));
      return;
    }
    rooms.push(summary);
  }));

  if (deletes.length > 0) {
    await Promise.all(deletes);
  }

  let visibleRooms = rooms;
  if (env.ROOM_OBJECTS) {
    const liveRooms = await Promise.all(rooms.map((room) => fetchLiveRoomSummary(env, room.roomId)));
    visibleRooms = liveRooms.filter((room): room is RoomSummary => room !== undefined);
  }

  visibleRooms.sort((a, b) => b.updatedAt - a.updatedAt || a.roomId.localeCompare(b.roomId));
  return jsonResponse({ ok: true, rooms: visibleRooms });
}

async function fetchLiveRoomSummary(env: Env, roomId: string): Promise<RoomSummary | undefined> {
  const id = env.ROOM_OBJECTS.idFromName(roomId);
  const stub = env.ROOM_OBJECTS.get(id);
  const response = await stub.fetch(new Request(`https://rooms.local/rooms/${encodeURIComponent(roomId)}/summary`));
  if (!response.ok) {
    return undefined;
  }
  const data = await response.json<unknown>();
  if (!isObject(data)) {
    return undefined;
  }
  return normalizeRoomSummary(data.room);
}

async function handleRoomSummary(env: Env, roomId: string): Promise<Response> {
  const room = await loadRoom(env, roomId);
  await trySyncRoomDirectory(env, roomId, room.peers);
  return jsonResponse({ ok: true, room: roomSummary(roomId, room.peers) ?? null });
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
  const peer = { peerId, candidates, metadata, lastSeen: now };

  const room = await loadRoom(env, roomId, now);
  const nextPeers = [...otherPeers(room, peerId), peer];
  await savePeer(env, roomId, peer);
  await cleanupLegacyRoom(env, roomId);
  await trySyncRoomDirectory(env, roomId, nextPeers, now);

  return jsonResponse({ ok: true, peers: otherPeers(room, peerId) });
}

async function handlePeers(request: Request, env: Env, roomId: string): Promise<Response> {
  const url = new URL(request.url);
  const peerId = validatePeerId(url.searchParams.get("peerId"));
  const room = await loadRoom(env, roomId);
  await trySyncRoomDirectory(env, roomId, room.peers);

  return jsonResponse({ ok: true, peers: otherPeers(room, peerId) });
}

async function handleHeartbeat(request: Request, env: Env, roomId: string): Promise<Response> {
  const body = await readJson<PeerRequest>(request);
  const peerId = validatePeerId(body.peerId);
  const now = Date.now();
  const peer = normalizeStoredPeer(await env.ROOMS.get<unknown>(roomPeerKey(roomId, peerId), "json"));
  if (!peer) {
    await deletePeer(env, roomId, peerId);
    throw new HttpError(404, "peer_not_found", "Peer is not in this room.");
  }
  peer.lastSeen = now;
  await savePeer(env, roomId, peer);
  await trySyncRoomDirectory(env, roomId, (await loadRoom(env, roomId, now)).peers, now);

  return jsonResponse({ ok: true });
}

async function handleLeave(request: Request, env: Env, roomId: string): Promise<Response> {
  const body = await readJson<PeerRequest>(request);
  const peerId = validatePeerId(body.peerId);
  await deletePeer(env, roomId, peerId);
  await cleanupLegacyRoom(env, roomId);
  await trySyncRoomDirectory(env, roomId, (await loadRoom(env, roomId)).peers);

  return jsonResponse({ ok: true });
}

export class RoomObject extends DurableObject<Env> {
  private peers: Map<string, Peer> | undefined;

  async fetch(request: Request): Promise<Response> {
    try {
      return await this.route(request);
    } catch (error) {
      if (error instanceof HttpError) {
        return jsonResponse({ ok: false, error: error.code, message: error.message }, error.status);
      }
      console.error("Unhandled room object error", error);
      return jsonResponse({ ok: false, error: "internal_error", message: "Internal server error." }, 500);
    }
  }

  private async route(request: Request): Promise<Response> {
    const url = new URL(request.url);
    const pathname = normalizePath(url.pathname);
    const match = pathname.match(/^\/rooms\/([^/]+)\/(join|peers|heartbeat|leave|summary)$/);
    if (!match) {
      throw new HttpError(404, "not_found", "Endpoint not found.");
    }

    const roomId = validateRoomId(decodeURIComponent(match[1]));
    const action = match[2];
    if (action === "join" && request.method === "POST") {
      return this.handleJoin(request, roomId);
    }
    if (action === "peers" && request.method === "GET") {
      return this.handlePeers(request, roomId);
    }
    if (action === "heartbeat" && request.method === "POST") {
      return this.handleHeartbeat(request, roomId);
    }
    if (action === "leave" && request.method === "POST") {
      return this.handleLeave(request, roomId);
    }
    if (action === "summary" && request.method === "GET") {
      return this.handleSummary(roomId);
    }

    throw new HttpError(405, "method_not_allowed", "Method not allowed for this endpoint.");
  }

  private async loadPeers(): Promise<Map<string, Peer>> {
    if (this.peers) {
      return this.peers;
    }

    const stored = await this.ctx.storage.get<Peer[]>("peers");
    const peers = new Map<string, Peer>();
    if (Array.isArray(stored)) {
      for (const value of stored) {
        const peer = normalizeStoredPeer(value);
        if (peer) {
          peers.set(peer.peerId, peer);
        }
      }
    }
    this.peers = peers;
    return peers;
  }

  private async savePeers(roomId: string, peers: Map<string, Peer>, now = Date.now()): Promise<void> {
    if (peers.size === 0) {
      await this.ctx.storage.delete("peers");
      await trySyncRoomDirectory(this.env, roomId, [], now);
      return;
    }
    await this.ctx.storage.put("peers", [...peers.values()]);
    await trySyncRoomDirectory(this.env, roomId, [...peers.values()], now);
  }

  private cleanupPeers(peers: Map<string, Peer>, now = Date.now()): boolean {
    let changed = false;
    for (const [peerId, peer] of peers) {
      if (now - peer.lastSeen > STALE_PEER_MS) {
        peers.delete(peerId);
        changed = true;
      }
    }
    return changed;
  }

  private async handleJoin(request: Request, roomId: string): Promise<Response> {
    const body = await readJson<JoinRequest>(request);
    const peerId = validatePeerId(body.peerId);
    const candidates = validateCandidates(body.candidates);
    const metadata = validateMetadata(body.metadata);
    const now = Date.now();
    const peers = await this.loadPeers();
    this.cleanupPeers(peers, now);

    peers.set(peerId, { peerId, candidates, metadata, lastSeen: now });
    await this.savePeers(roomId, peers, now);

    return jsonResponse({ ok: true, peers: otherPeers({ peers: [...peers.values()] }, peerId) });
  }

  private async handlePeers(request: Request, roomId: string): Promise<Response> {
    const url = new URL(request.url);
    const peerId = validatePeerId(url.searchParams.get("peerId"));
    const peers = await this.loadPeers();
    const changed = this.cleanupPeers(peers);
    if (changed) {
      await this.savePeers(roomId, peers);
    }

    return jsonResponse({ ok: true, peers: otherPeers({ peers: [...peers.values()] }, peerId) });
  }

  private async handleHeartbeat(request: Request, roomId: string): Promise<Response> {
    const body = await readJson<PeerRequest>(request);
    const peerId = validatePeerId(body.peerId);
    const now = Date.now();
    const peers = await this.loadPeers();
    this.cleanupPeers(peers, now);
    const peer = peers.get(peerId);
    if (!peer) {
      await this.savePeers(roomId, peers, now);
      throw new HttpError(404, "peer_not_found", "Peer is not in this room.");
    }

    peer.lastSeen = now;
    await this.savePeers(roomId, peers, now);

    return jsonResponse({ ok: true });
  }

  private async handleLeave(request: Request, roomId: string): Promise<Response> {
    const body = await readJson<PeerRequest>(request);
    const peerId = validatePeerId(body.peerId);
    const peers = await this.loadPeers();
    peers.delete(peerId);
    await this.savePeers(roomId, peers);

    return jsonResponse({ ok: true });
  }

  private async handleSummary(roomId: string): Promise<Response> {
    const now = Date.now();
    const peers = await this.loadPeers();
    const changed = this.cleanupPeers(peers, now);
    const summary = roomSummary(roomId, [...peers.values()], now);
    if (changed || !summary) {
      await this.savePeers(roomId, peers, now);
    } else {
      await trySyncRoomDirectory(this.env, roomId, [...peers.values()], now);
    }

    return jsonResponse({ ok: true, room: summary ?? null });
  }
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

  if (pathname === "/rooms" && request.method === "GET") {
    return handleRoomList(request, env);
  }

  const match = pathname.match(/^\/rooms\/([^/]+)\/(join|peers|heartbeat|leave|summary)$/);
  if (!match) {
    throw new HttpError(404, "not_found", "Endpoint not found.");
  }

  const roomId = validateRoomId(decodeURIComponent(match[1]));
  const action = match[2];

  if (env.ROOM_OBJECTS) {
    const id = env.ROOM_OBJECTS.idFromName(roomId);
    return env.ROOM_OBJECTS.get(id).fetch(request);
  }

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
  if (action === "summary" && request.method === "GET") {
    return handleRoomSummary(env, roomId);
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
