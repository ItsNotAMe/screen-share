import { DurableObject } from "cloudflare:workers";

export interface Env {
  ROOMS: KVNamespace;
  ROOM_OBJECTS: DurableObjectNamespace<RoomObject>;
  ROOM_DIRECTORY?: DurableObjectNamespace<RoomDirectoryObject>;
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
  // Server-issued secret proving ownership of this peerId. Never returned to
  // other peers; required to read candidates/key or mutate this peer's state.
  token: string;
}

// A peer as exposed to other members: no token.
interface PublicPeer {
  peerId: string;
  candidates: Candidate[];
  metadata?: PeerMetadata;
  lastSeen: number;
}

interface Room {
  peers: Peer[];
  roomAccessKey?: string;
  info?: RoomInfo;
  passwordVerifier?: PasswordVerifier;
}

interface RoomInfo {
  name?: string;
  passwordProtected: boolean;
}

interface RoomInfoUpdate {
  name?: string;
  passwordProtected?: boolean;
  password?: string;
}

interface PasswordVerifier {
  algorithm: "pbkdf2-sha256";
  iterations: number;
  salt: string;
  hash: string;
}

interface RoomSummary {
  roomId: string;
  name?: string;
  peerCount: number;
  updatedAt: number;
  expiresAt: number;
  requiresRoomKey: boolean;
  passwordProtected: boolean;
}

interface JoinRequest {
  peerId: unknown;
  candidates: unknown;
  metadata?: unknown;
  room?: unknown;
  host?: unknown;
}

interface PeerRequest {
  peerId: unknown;
}

interface DirectorySyncRequest {
  roomId?: unknown;
  room?: unknown;
}

interface RoomEventSocketAttachment {
  peerId?: string;
}

const STALE_PEER_MS = 60_000;
const ROOM_TTL_SECONDS = 180;
const ROOM_DIRECTORY_PREFIX = "active-room:";
const ROOM_DIRECTORY_OBJECT_NAME = "global";
const ROOM_DIRECTORY_STORAGE_PREFIX = "room:";
const MAX_JSON_BYTES = 16 * 1024;
const MAX_CANDIDATES = 8;
const MAX_ROOM_LIST_LIMIT = 250;
// Cap on distinct peers a single room will hold. All peers are persisted as
// one Durable Object value (~128 KiB limit), so an unbounded peer count both
// exhausts storage and can break the room; this also bounds the per-request
// fanout of peer lists. Generous relative to any real screen-share room.
const MAX_PEERS_PER_ROOM = 64;
const RATE_LIMIT_WINDOW_MS = 60_000;
const RATE_LIMIT_MAX_REQUESTS = 240;
const ROOM_ACCESS_KEY_BYTES = 32;
const ROOM_PASSWORD_SALT_BYTES = 16;
const ROOM_PASSWORD_HASH_BYTES = 32;
const ROOM_PASSWORD_HASH_ITERATIONS = 100_000;
const ROOM_PASSWORD_HEADER = "X-ScreenShare-Room-Password";
const PEER_TOKEN_HEADER = "X-ScreenShare-Peer-Token";
const PEER_TOKEN_BYTES = 32;

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

function roomAccessKeyKey(roomId: string): string {
  return `room:${roomId}:access-key`;
}

function roomInfoKey(roomId: string): string {
  return `room:${roomId}:info`;
}

function roomPasswordVerifierKey(roomId: string): string {
  return `room:${roomId}:password-verifier`;
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

function roomDirectoryStorageKey(roomId: string): string {
  return `${ROOM_DIRECTORY_STORAGE_PREFIX}${roomId}`;
}

function publicPeerForEvent(peer: Peer): PublicPeer {
  return {
    peerId: peer.peerId,
    candidates: peer.candidates,
    metadata: peer.metadata,
    lastSeen: peer.lastSeen,
  };
}

function generatePeerToken(): string {
  const bytes = new Uint8Array(PEER_TOKEN_BYTES);
  crypto.getRandomValues(bytes);
  return base64UrlEncode(bytes);
}

function peerTokenFromRequest(request: Request): string | undefined {
  const token = request.headers.get(PEER_TOKEN_HEADER);
  return token !== null && token.length > 0 ? token : undefined;
}

// Constant-time comparison of a presented token against a peer's stored token.
function peerTokenMatches(stored: string, presented: string | undefined): boolean {
  return presented !== undefined && timingSafeEqual(stored, presented);
}

function peerAnnouncementKey(peer: Peer): string {
  return JSON.stringify({
    peerId: peer.peerId,
    candidates: peer.candidates,
    metadata: peer.metadata,
  });
}

function randomRoomAccessKey(): string {
  const bytes = new Uint8Array(ROOM_ACCESS_KEY_BYTES);
  crypto.getRandomValues(bytes);
  let binary = "";
  for (const byte of bytes) {
    binary += String.fromCharCode(byte);
  }
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

function validRoomAccessKey(value: unknown): value is string {
  return typeof value === "string" && /^[A-Za-z0-9_-]{32,128}$/.test(value);
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

// Decode a %-encoded room id path segment, turning malformed encodings into a
// clean 400 instead of an uncaught URIError surfacing as a generic 500.
function decodeRoomIdSegment(segment: string): string {
  let decoded: string;
  try {
    decoded = decodeURIComponent(segment);
  } catch {
    throw new HttpError(400, "invalid_room_id", "Room id is not valid percent-encoding.");
  }
  return validateRoomId(decoded);
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

function isPrivateAddress(ip: string): boolean {
  if (isValidIpv4(ip)) {
    const [a, b] = ip.split(".").map(Number);
    return (
      a === 10 ||
      a === 127 ||
      (a === 169 && b === 254) ||         // link-local
      (a === 172 && b >= 16 && b <= 31) ||
      (a === 192 && b === 168) ||
      (a === 100 && b >= 64 && b <= 127)  // CGNAT
    );
  }
  const lower = ip.toLowerCase();
  return (
    lower === "::1" ||
    lower.startsWith("fc") || lower.startsWith("fd") || // ULA
    lower.startsWith("fe8") || lower.startsWith("fe9") ||
    lower.startsWith("fea") || lower.startsWith("feb")   // link-local
  );
}

function sameIpFamily(a: string, b: string): boolean {
  return isValidIpv4(a) === isValidIpv4(b);
}

// Drop candidates that could turn honest peers into a reflected-DDoS source:
// a srflx (public) candidate must match the caller's own connecting IP, and a
// host candidate must be a private/local address. Filtering (rather than
// rejecting the join) avoids hard-failing peers whose STUN egress differs from
// their TLS egress; they simply lose the offending candidate.
function filterReflectionCandidates(candidates: Candidate[], connectingIp: string): Candidate[] {
  return candidates.filter((candidate) => {
    if (candidate.type === "host") {
      return isPrivateAddress(candidate.ip);
    }
    if (candidate.type === "srflx") {
      if (!connectingIp || !sameIpFamily(candidate.ip, connectingIp)) {
        return true;
      }
      return candidate.ip === connectingIp;
    }
    return true;
  });
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

// Reject C0 control characters. Peer/room display strings are echoed verbatim
// to every other peer and to the native client, so keep them to plain text.
function rejectControlChars(value: string, field: string): string {
  for (const ch of value) {
    if (ch < " ") {
      throw new HttpError(400, "invalid_metadata", `${field} must not contain control characters.`);
    }
  }
  return value;
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
    clean.name = rejectControlChars(metadata.name.trim(), "metadata.name");
  }
  if (metadata.platform !== undefined) {
    if (typeof metadata.platform !== "string" || metadata.platform.length > 80) {
      throw new HttpError(400, "invalid_metadata", "metadata.platform must be a short string.");
    }
    clean.platform = rejectControlChars(metadata.platform.trim(), "metadata.platform");
  }
  return Object.keys(clean).length > 0 ? clean : undefined;
}

function validateHostFlag(host: unknown): boolean {
  if (host === undefined) {
    return false;
  }
  if (typeof host !== "boolean") {
    throw new HttpError(400, "invalid_host", "host must be a boolean.");
  }
  return host;
}

function base64UrlEncode(bytes: Uint8Array): string {
  let binary = "";
  for (const byte of bytes) {
    binary += String.fromCharCode(byte);
  }
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

function base64UrlDecode(text: string): Uint8Array | undefined {
  if (!/^[A-Za-z0-9_-]+$/.test(text)) {
    return undefined;
  }
  const padded = text.replace(/-/g, "+").replace(/_/g, "/").padEnd(Math.ceil(text.length / 4) * 4, "=");
  try {
    const binary = atob(padded);
    const bytes = new Uint8Array(binary.length);
    for (let index = 0; index < binary.length; ++index) {
      bytes[index] = binary.charCodeAt(index);
    }
    return bytes;
  } catch {
    return undefined;
  }
}

function timingSafeEqual(lhs: string, rhs: string): boolean {
  const lhsBytes = new TextEncoder().encode(lhs);
  const rhsBytes = new TextEncoder().encode(rhs);
  let diff = lhsBytes.length ^ rhsBytes.length;
  const length = Math.max(lhsBytes.length, rhsBytes.length);
  for (let index = 0; index < length; ++index) {
    diff |= (lhsBytes[index] ?? 0) ^ (rhsBytes[index] ?? 0);
  }
  return diff === 0;
}

function validateRoomPassword(password: unknown): string | undefined {
  if (password === undefined) {
    return undefined;
  }
  if (typeof password !== "string" || password.length < 1 || password.length > 128) {
    throw new HttpError(400, "invalid_room_password", "room.password must be 1-128 characters.");
  }
  for (const ch of password) {
    if (ch < " ") {
      throw new HttpError(400, "invalid_room_password", "room.password must not contain control characters.");
    }
  }
  return password;
}

function roomPasswordFromRequest(request: Request, _url: URL): string | undefined {
  // The room password is only accepted via the X-ScreenShare-Room-Password
  // header. It must never be read from the query string: query strings are
  // routinely captured in Cloudflare/proxy/server access logs even over HTTPS.
  const headerPassword = request.headers.get(ROOM_PASSWORD_HEADER);
  if (headerPassword !== null) {
    try {
      return validateRoomPassword(decodeURIComponent(headerPassword));
    } catch {
      throw new HttpError(400, "invalid_room_password", "Room password header is invalid.");
    }
  }
  return undefined;
}

function validateRoomInfo(info: unknown): RoomInfoUpdate | undefined {
  if (info === undefined) {
    return undefined;
  }
  if (!isObject(info)) {
    throw new HttpError(400, "invalid_room_info", "room must be an object.");
  }

  const clean: RoomInfoUpdate = {};
  if (info.name !== undefined) {
    if (typeof info.name !== "string" || info.name.length > 80) {
      throw new HttpError(400, "invalid_room_name", "room.name must be a short string.");
    }
    const name = info.name.trim();
    for (const ch of name) {
      if (ch < " ") {
        throw new HttpError(400, "invalid_room_name", "room.name must not contain control characters.");
      }
    }
    if (name.length > 0) {
      clean.name = name;
    }
  }
  const password = validateRoomPassword(info.password);
  if (password !== undefined) {
    clean.password = password;
  }
  if (info.passwordProtected !== undefined) {
    if (typeof info.passwordProtected !== "boolean") {
      throw new HttpError(400, "invalid_room_password", "room.passwordProtected must be a boolean.");
    }
    clean.passwordProtected = info.passwordProtected;
  }
  return Object.keys(clean).length > 0 ? clean : undefined;
}

function normalizePasswordVerifier(value: unknown): PasswordVerifier | undefined {
  if (!isObject(value) ||
      value.algorithm !== "pbkdf2-sha256" ||
      typeof value.iterations !== "number" ||
      typeof value.salt !== "string" ||
      typeof value.hash !== "string") {
    return undefined;
  }
  if (!Number.isInteger(value.iterations) || value.iterations < 10_000 || value.iterations > 1_000_000) {
    return undefined;
  }
  const salt = base64UrlDecode(value.salt);
  const hash = base64UrlDecode(value.hash);
  if (!salt || salt.length < 8 || !hash || hash.length !== ROOM_PASSWORD_HASH_BYTES) {
    return undefined;
  }
  return {
    algorithm: "pbkdf2-sha256",
    iterations: value.iterations,
    salt: value.salt,
    hash: value.hash,
  };
}

function normalizeRoomInfo(value: unknown): RoomInfo | undefined {
  if (!isObject(value)) {
    return undefined;
  }
  let name: string | undefined;
  if (value.name !== undefined) {
    if (typeof value.name !== "string" || value.name.length > 80) {
      return undefined;
    }
    name = value.name.trim();
    if (name.length === 0) {
      name = undefined;
    }
  }
  return {
    name,
    passwordProtected: value.passwordProtected === true,
  };
}

function mergeRoomInfo(current: RoomInfo | undefined, update: Partial<RoomInfo> | undefined): RoomInfo | undefined {
  if (!current && !update) {
    return undefined;
  }
  const merged: RoomInfo = {
    name: update?.name ?? current?.name,
    passwordProtected: current?.passwordProtected === true || update?.passwordProtected === true,
  };
  return merged.name || merged.passwordProtected ? merged : undefined;
}

async function hashRoomPassword(password: string, verifier: Pick<PasswordVerifier, "iterations" | "salt">): Promise<string> {
  const salt = base64UrlDecode(verifier.salt);
  if (!salt) {
    throw new HttpError(500, "invalid_room_password_verifier", "Room password verifier is invalid.");
  }
  const key = await crypto.subtle.importKey(
    "raw",
    new TextEncoder().encode(password),
    "PBKDF2",
    false,
    ["deriveBits"],
  );
  const bits = await crypto.subtle.deriveBits(
    {
      name: "PBKDF2",
      hash: "SHA-256",
      salt,
      iterations: verifier.iterations,
    },
    key,
    ROOM_PASSWORD_HASH_BYTES * 8,
  );
  return base64UrlEncode(new Uint8Array(bits));
}

async function createPasswordVerifier(password: string): Promise<PasswordVerifier> {
  const salt = new Uint8Array(ROOM_PASSWORD_SALT_BYTES);
  crypto.getRandomValues(salt);
  const verifier = {
    iterations: ROOM_PASSWORD_HASH_ITERATIONS,
    salt: base64UrlEncode(salt),
  };
  return {
    algorithm: "pbkdf2-sha256",
    ...verifier,
    hash: await hashRoomPassword(password, verifier),
  };
}

async function verifyPassword(verifier: PasswordVerifier, password: string): Promise<boolean> {
  const hash = await hashRoomPassword(password, verifier);
  return timingSafeEqual(hash, verifier.hash);
}

interface AppliedRoomSecurity {
  infoUpdate?: Partial<RoomInfo>;
  passwordVerifier?: PasswordVerifier;
}

async function applyJoinRoomSecurity(room: Room, update: RoomInfoUpdate | undefined): Promise<AppliedRoomSecurity> {
  const existingVerifier = room.passwordVerifier;
  const currentlyLocked = existingVerifier !== undefined || room.info?.passwordProtected === true;
  const requestedLock = update?.passwordProtected === true || update?.password !== undefined;
  let passwordVerifier = existingVerifier;

  if (currentlyLocked) {
    if (!update?.password) {
      throw new HttpError(401, "room_password_required", "Room requires a password.");
    }
    if (existingVerifier) {
      if (!await verifyPassword(existingVerifier, update.password)) {
        throw new HttpError(403, "invalid_room_password", "Room password is incorrect.");
      }
    } else {
      passwordVerifier = await createPasswordVerifier(update.password);
    }
  } else if (requestedLock && update?.password) {
    if (room.peers.length === 0) {
      passwordVerifier = await createPasswordVerifier(update.password);
    } else {
      throw new HttpError(409, "room_already_open", "An active open room cannot be locked. Choose a new room ID.");
    }
  } else if (requestedLock) {
    throw new HttpError(400, "room_password_required", "Locked rooms require a password.");
  }

  const infoUpdate: Partial<RoomInfo> = {};
  if (update?.name !== undefined) {
    infoUpdate.name = update.name;
  }
  if (passwordVerifier !== undefined) {
    infoUpdate.passwordProtected = true;
  }
  return {
    infoUpdate: Object.keys(infoUpdate).length > 0 ? infoUpdate : undefined,
    passwordVerifier,
  };
}

async function requireRoomPassword(room: Room, password: string | undefined): Promise<void> {
  if (!room.passwordVerifier && room.info?.passwordProtected !== true) {
    return;
  }
  if (!password) {
    throw new HttpError(401, "room_password_required", "Room requires a password.");
  }
  if (!room.passwordVerifier) {
    throw new HttpError(403, "invalid_room_password", "Room password verifier is unavailable.");
  }
  if (!await verifyPassword(room.passwordVerifier, password)) {
    throw new HttpError(403, "invalid_room_password", "Room password is incorrect.");
  }
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
  // A peer persisted before this field existed has no token; an empty string
  // never matches a presented token (timing-safe compare), so such a peer must
  // re-join to obtain one rather than being silently authable.
  const token = typeof value.token === "string" ? value.token : "";
  return {
    peerId: value.peerId,
    candidates,
    metadata,
    lastSeen,
    token,
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

  const storedRoomAccessKey = await env.ROOMS.get(roomAccessKeyKey(roomId));
  const roomAccessKey = validRoomAccessKey(storedRoomAccessKey) ? storedRoomAccessKey : undefined;
  const info = normalizeRoomInfo(await env.ROOMS.get<unknown>(roomInfoKey(roomId), "json"));
  const passwordVerifier = normalizePasswordVerifier(await env.ROOMS.get<unknown>(roomPasswordVerifierKey(roomId), "json"));
  return { peers, roomAccessKey, info, passwordVerifier };
}

async function ensureRoomAccessKey(env: Env, roomId: string, room: Room): Promise<string> {
  if (validRoomAccessKey(room.roomAccessKey)) {
    await env.ROOMS.put(roomAccessKeyKey(roomId), room.roomAccessKey, {
      expirationTtl: ROOM_TTL_SECONDS,
    });
    return room.roomAccessKey;
  }
  const roomAccessKey = randomRoomAccessKey();
  room.roomAccessKey = roomAccessKey;
  await env.ROOMS.put(roomAccessKeyKey(roomId), roomAccessKey, {
    expirationTtl: ROOM_TTL_SECONDS,
  });
  return roomAccessKey;
}

async function savePeer(env: Env, roomId: string, peer: Peer): Promise<void> {
  await env.ROOMS.put(roomPeerKey(roomId, peer.peerId), JSON.stringify(peer), {
    expirationTtl: ROOM_TTL_SECONDS,
  });
}

async function saveRoomInfo(env: Env, roomId: string, info: RoomInfo | undefined): Promise<void> {
  if (!info) {
    await env.ROOMS.delete(roomInfoKey(roomId));
    return;
  }
  await env.ROOMS.put(roomInfoKey(roomId), JSON.stringify(info), {
    expirationTtl: ROOM_TTL_SECONDS,
  });
}

async function deletePeer(env: Env, roomId: string, peerId: string): Promise<void> {
  await env.ROOMS.delete(roomPeerKey(roomId, peerId));
}

async function cleanupLegacyRoom(env: Env, roomId: string): Promise<void> {
  await env.ROOMS.delete(roomKey(roomId));
}

function otherPeers(room: Room, peerId: string): PublicPeer[] {
  return room.peers
    .filter((peer) => peer.peerId !== peerId)
    .map(publicPeerForEvent);
}

function roomSummary(roomId: string, peers: Peer[], info?: RoomInfo, now = Date.now()): RoomSummary | undefined {
  const activePeers = peers.filter((peer) => now - peer.lastSeen <= STALE_PEER_MS);
  if (activePeers.length === 0) {
    return undefined;
  }
  const updatedAt = Math.max(...activePeers.map((peer) => peer.lastSeen));
  const passwordProtected = info?.passwordProtected === true;
  return {
    roomId,
    name: info?.name,
    peerCount: activePeers.length,
    updatedAt,
    expiresAt: updatedAt + ROOM_TTL_SECONDS * 1000,
    requiresRoomKey: passwordProtected,
    passwordProtected,
  };
}

function normalizeRoomSummary(value: unknown, now = Date.now()): RoomSummary | undefined {
  if (!isObject(value) ||
      typeof value.roomId !== "string" ||
      typeof value.peerCount !== "number" ||
      typeof value.updatedAt !== "number" ||
      typeof value.expiresAt !== "number" ||
      typeof value.requiresRoomKey !== "boolean") {
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
    name: typeof value.name === "string" && value.name.trim().length > 0 ? value.name.trim().slice(0, 80) : undefined,
    peerCount: value.peerCount,
    updatedAt: value.updatedAt,
    expiresAt: value.expiresAt,
    requiresRoomKey: value.passwordProtected === true || value.requiresRoomKey === true,
    passwordProtected: value.passwordProtected === true || value.requiresRoomKey === true,
  };
}

async function saveRoomPasswordVerifier(
  env: Env,
  roomId: string,
  passwordVerifier: PasswordVerifier | undefined,
): Promise<void> {
  if (!passwordVerifier) {
    await env.ROOMS.delete(roomPasswordVerifierKey(roomId));
    return;
  }
  await env.ROOMS.put(roomPasswordVerifierKey(roomId), JSON.stringify(passwordVerifier), {
    expirationTtl: ROOM_TTL_SECONDS,
  });
}

async function syncRoomDirectoryObject(
  env: Env,
  roomId: string,
  summary: RoomSummary | undefined,
): Promise<boolean> {
  if (!env.ROOM_DIRECTORY) {
    return false;
  }

  const id = env.ROOM_DIRECTORY.idFromName(ROOM_DIRECTORY_OBJECT_NAME);
  const response = await env.ROOM_DIRECTORY.get(id).fetch(new Request("https://directory.local/directory/sync", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ roomId, room: summary ?? null }),
  }));
  if (!response.ok) {
    throw new Error(`Room directory sync failed with HTTP ${response.status}`);
  }
  return true;
}

async function syncRoomDirectory(
  env: Env,
  roomId: string,
  peers: Peer[],
  info?: RoomInfo,
  now = Date.now(),
): Promise<void> {
  const summary = roomSummary(roomId, peers, info, now);
  if (await syncRoomDirectoryObject(env, roomId, summary)) {
    return;
  }

  const key = roomDirectoryKey(roomId);
  if (!summary) {
    await env.ROOMS.delete(key);
    return;
  }
  await env.ROOMS.put(key, JSON.stringify(summary), {
    expirationTtl: ROOM_TTL_SECONDS,
  });
}

async function trySyncRoomDirectory(
  env: Env,
  roomId: string,
  peers: Peer[],
  info?: RoomInfo,
  now = Date.now(),
): Promise<void> {
  try {
    await syncRoomDirectory(env, roomId, peers, info, now);
  } catch (error) {
    console.error("Failed to sync room directory", error);
  }
}

async function fetchKvRoomDirectory(env: Env, limit: number, now: number): Promise<RoomSummary[]> {
  const listed = await env.ROOMS.list({ prefix: ROOM_DIRECTORY_PREFIX, limit });
  const rooms: RoomSummary[] = [];
  const deletes: Promise<void>[] = [];

  await Promise.all(listed.keys.map(async (key) => {
    try {
      const storedText = await env.ROOMS.get(key.name);
      const stored = storedText === null ? undefined : JSON.parse(storedText);
      const summary = normalizeRoomSummary(stored, now);
      if (!summary) {
        deletes.push(env.ROOMS.delete(key.name));
        return;
      }
      rooms.push(summary);
    } catch (error) {
      console.error("Failed to read active room directory entry", key.name, error);
      deletes.push(env.ROOMS.delete(key.name));
    }
  }));

  if (deletes.length > 0) {
    await Promise.allSettled(deletes);
  }
  return rooms;
}

async function fetchDurableRoomDirectory(env: Env, limit: number, now: number): Promise<RoomSummary[]> {
  if (!env.ROOM_DIRECTORY) {
    return [];
  }

  const id = env.ROOM_DIRECTORY.idFromName(ROOM_DIRECTORY_OBJECT_NAME);
  const response = await env.ROOM_DIRECTORY.get(id).fetch(
    new Request(`https://directory.local/directory/rooms?limit=${limit}`),
  );
  if (!response.ok) {
    throw new Error(`Room directory list failed with HTTP ${response.status}`);
  }

  const data = await response.json<unknown>();
  if (!isObject(data) || data.ok !== true || !Array.isArray(data.rooms)) {
    throw new Error("Room directory returned an invalid response.");
  }

  return data.rooms
    .map((room) => normalizeRoomSummary(room, now))
    .filter((room): room is RoomSummary => room !== undefined);
}

async function handleRoomList(request: Request, env: Env): Promise<Response> {
  try {
    const url = new URL(request.url);
    const rawLimit = Number(url.searchParams.get("limit") ?? "100");
    const limit = Number.isInteger(rawLimit) && rawLimit > 0
      ? Math.min(rawLimit, MAX_ROOM_LIST_LIMIT)
      : 100;
    const now = Date.now();
    const rooms = env.ROOM_DIRECTORY
      ? await fetchDurableRoomDirectory(env, limit, now)
      : await fetchKvRoomDirectory(env, limit, now);
    let visibleRooms = rooms;
    if (env.ROOM_OBJECTS) {
      const liveRooms = await Promise.all(rooms.map(async (room) => {
        try {
          return await fetchLiveRoomSummary(env, room.roomId);
        } catch (error) {
          console.error("Failed to fetch live room summary", room.roomId, error);
          await trySyncRoomDirectory(env, room.roomId, [], undefined);
          return undefined;
        }
      }));
      visibleRooms = liveRooms.filter((room): room is RoomSummary => room !== undefined);
    }

    visibleRooms.sort((a, b) => b.updatedAt - a.updatedAt || a.roomId.localeCompare(b.roomId));
    return jsonResponse({ ok: true, rooms: visibleRooms });
  } catch (error) {
    console.error("Room directory unavailable", error);
    return jsonResponse({ ok: true, rooms: [] });
  }
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

// Legacy per-room KV handlers removed: all room traffic is served by the
// authenticated RoomObject Durable Object path (see routeRequest).

export class RoomDirectoryObject extends DurableObject<Env> {
  async fetch(request: Request): Promise<Response> {
    try {
      return await this.route(request);
    } catch (error) {
      if (error instanceof HttpError) {
        return jsonResponse({ ok: false, error: error.code, message: error.message }, error.status);
      }
      console.error("Unhandled room directory error", error);
      return jsonResponse({ ok: false, error: "internal_error", message: "Internal server error." }, 500);
    }
  }

  private async route(request: Request): Promise<Response> {
    const url = new URL(request.url);
    const pathname = normalizePath(url.pathname);

    if (pathname === "/directory/rooms" && request.method === "GET") {
      return this.handleList(url);
    }
    if (pathname === "/directory/sync" && request.method === "POST") {
      return this.handleSync(request);
    }

    throw new HttpError(404, "not_found", "Endpoint not found.");
  }

  private async handleList(url: URL): Promise<Response> {
    const rawLimit = Number(url.searchParams.get("limit") ?? "100");
    const limit = Number.isInteger(rawLimit) && rawLimit > 0
      ? Math.min(rawLimit, MAX_ROOM_LIST_LIMIT)
      : 100;
    const now = Date.now();
    const entries = await this.ctx.storage.list<unknown>({
      prefix: ROOM_DIRECTORY_STORAGE_PREFIX,
      limit: MAX_ROOM_LIST_LIMIT,
    });
    const rooms: RoomSummary[] = [];
    const deletes: string[] = [];

    for (const [key, value] of entries) {
      const summary = normalizeRoomSummary(value, now);
      if (!summary) {
        deletes.push(key);
        continue;
      }
      rooms.push(summary);
    }

    if (deletes.length > 0) {
      await this.ctx.storage.delete(deletes);
    }

    rooms.sort((a, b) => b.updatedAt - a.updatedAt || a.roomId.localeCompare(b.roomId));
    return jsonResponse({ ok: true, rooms: rooms.slice(0, limit) });
  }

  private async handleSync(request: Request): Promise<Response> {
    const body = await readJson<DirectorySyncRequest>(request);
    const roomId = validateRoomId(typeof body.roomId === "string" ? body.roomId : "");
    const key = roomDirectoryStorageKey(roomId);
    if (body.room === null || body.room === undefined) {
      await this.ctx.storage.delete(key);
      return jsonResponse({ ok: true });
    }

    const summary = normalizeRoomSummary(body.room);
    if (!summary || summary.roomId !== roomId) {
      await this.ctx.storage.delete(key);
      return jsonResponse({ ok: true });
    }

    await this.ctx.storage.put(key, summary);
    return jsonResponse({ ok: true });
  }
}

export class RoomObject extends DurableObject<Env> {
  private peers: Map<string, Peer> | undefined;
  private roomAccessKey: string | undefined;
  private roomInfo: RoomInfo | undefined;
  private roomPasswordVerifier: PasswordVerifier | undefined;
  private hostPeerId: string | undefined;

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
    const match = pathname.match(/^\/rooms\/([^/]+)\/(join|peers|heartbeat|leave|summary|events)$/);
    if (!match) {
      throw new HttpError(404, "not_found", "Endpoint not found.");
    }

    const roomId = decodeRoomIdSegment(match[1]);
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
    if (action === "events" && request.method === "GET") {
      return this.handleEvents(request, roomId);
    }

    throw new HttpError(405, "method_not_allowed", "Method not allowed for this endpoint.");
  }

  webSocketMessage(webSocket: WebSocket, message: string | ArrayBuffer): void {
    if (typeof message === "string" && message === "ping") {
      webSocket.send("pong");
    }
  }

  webSocketClose(_webSocket: WebSocket): void {
    // The hibernation API removes closed sockets from ctx.getWebSockets().
  }

  webSocketError(_webSocket: WebSocket, error: unknown): void {
    console.error("Room event WebSocket error", error);
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

  private async ensureRoomAccessKey(): Promise<string> {
    if (validRoomAccessKey(this.roomAccessKey)) {
      return this.roomAccessKey;
    }
    const stored = await this.ctx.storage.get<string>("roomAccessKey");
    if (validRoomAccessKey(stored)) {
      this.roomAccessKey = stored;
      return stored;
    }
    const roomAccessKey = randomRoomAccessKey();
    this.roomAccessKey = roomAccessKey;
    await this.ctx.storage.put("roomAccessKey", roomAccessKey);
    return roomAccessKey;
  }

  private async loadRoomInfo(): Promise<RoomInfo | undefined> {
    if (this.roomInfo !== undefined) {
      return this.roomInfo;
    }
    this.roomInfo = normalizeRoomInfo(await this.ctx.storage.get<unknown>("roomInfo"));
    return this.roomInfo;
  }

  private async loadRoomPasswordVerifier(): Promise<PasswordVerifier | undefined> {
    if (this.roomPasswordVerifier !== undefined) {
      return this.roomPasswordVerifier;
    }
    this.roomPasswordVerifier = normalizePasswordVerifier(await this.ctx.storage.get<unknown>("roomPasswordVerifier"));
    return this.roomPasswordVerifier;
  }

  private async saveRoomPasswordVerifier(passwordVerifier: PasswordVerifier | undefined): Promise<void> {
    this.roomPasswordVerifier = passwordVerifier;
    if (passwordVerifier) {
      await this.ctx.storage.put("roomPasswordVerifier", passwordVerifier);
    } else {
      await this.ctx.storage.delete("roomPasswordVerifier");
    }
  }

  private async loadHostPeerId(): Promise<string | undefined> {
    if (this.hostPeerId !== undefined) {
      return this.hostPeerId || undefined;
    }
    const stored = await this.ctx.storage.get<string>("hostPeerId");
    this.hostPeerId = typeof stored === "string" ? stored : "";
    return this.hostPeerId || undefined;
  }

  private async saveHostPeerId(peerId: string): Promise<void> {
    this.hostPeerId = peerId;
    await this.ctx.storage.put("hostPeerId", peerId);
  }

  private async applyRoomInfoUpdate(update: Partial<RoomInfo> | undefined): Promise<RoomInfo | undefined> {
    const current = await this.loadRoomInfo();
    const merged = mergeRoomInfo(current, update);
    this.roomInfo = merged;
    if (merged) {
      await this.ctx.storage.put("roomInfo", merged);
    } else {
      await this.ctx.storage.delete("roomInfo");
    }
    return merged;
  }

  private async savePeers(roomId: string, peers: Map<string, Peer>, now = Date.now()): Promise<void> {
    if (peers.size === 0) {
      await this.ctx.storage.delete("peers");
      await this.ctx.storage.delete("roomAccessKey");
      await this.ctx.storage.delete("roomInfo");
      await this.ctx.storage.delete("roomPasswordVerifier");
      await this.ctx.storage.delete("hostPeerId");
      this.roomAccessKey = undefined;
      this.roomInfo = undefined;
      this.roomPasswordVerifier = undefined;
      this.hostPeerId = "";
      await trySyncRoomDirectory(this.env, roomId, [], undefined, now);
      return;
    }
    await this.ctx.storage.put("peers", [...peers.values()]);
    await trySyncRoomDirectory(this.env, roomId, [...peers.values()], await this.loadRoomInfo(), now);
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

  private broadcastRoomEvent(roomId: string, event: Record<string, unknown>, exceptPeerId?: string): void {
    const message = JSON.stringify({
      ok: true,
      roomId,
      ...event,
    });
    for (const webSocket of this.ctx.getWebSockets()) {
      try {
        const attachment = webSocket.deserializeAttachment() as RoomEventSocketAttachment | undefined;
        if (attachment?.peerId && attachment.peerId === exceptPeerId) {
          continue;
        }
        webSocket.send(message);
      } catch (error) {
        console.error("Failed to send room event", error);
      }
    }
  }

  private async handleEvents(request: Request, roomId: string): Promise<Response> {
    if ((request.headers.get("Upgrade") ?? "").toLowerCase() !== "websocket") {
      throw new HttpError(426, "upgrade_required", "Room events require a WebSocket upgrade.");
    }

    const url = new URL(request.url);
    const peerId = validatePeerId(url.searchParams.get("peerId"));
    const peers = await this.loadPeers();
    const changed = this.cleanupPeers(peers);
    if (changed) {
      await this.savePeers(roomId, peers);
    }
    // The event stream carries other peers' candidates, so require the caller to
    // be a joined member presenting its token (sent as a header on the upgrade).
    const self = peers.get(peerId);
    if (!self || !peerTokenMatches(self.token, peerTokenFromRequest(request))) {
      throw new HttpError(403, "peer_token_invalid",
        "A valid peer token for a joined peer is required to stream room events.");
    }

    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.ctx.acceptWebSocket(server);
    server.serializeAttachment({ peerId });
    server.send(JSON.stringify({
      ok: true,
      type: "hello",
      roomId,
      peerId,
      peers: otherPeers({ peers: [...peers.values()] }, peerId),
    }));

    return new Response(null, {
      status: 101,
      webSocket: client,
    });
  }

  private async handleJoin(request: Request, roomId: string): Promise<Response> {
    const body = await readJson<JoinRequest>(request);
    const peerId = validatePeerId(body.peerId);
    const candidates = filterReflectionCandidates(
      validateCandidates(body.candidates),
      request.headers.get("CF-Connecting-IP") ?? "");
    const metadata = validateMetadata(body.metadata);
    const roomInfoUpdate = validateRoomInfo(body.room);
    const isHost = validateHostFlag(body.host);
    const now = Date.now();
    const peers = await this.loadPeers();
    this.cleanupPeers(peers, now);
    const currentInfo = await this.loadRoomInfo();
    const passwordVerifier = await this.loadRoomPasswordVerifier();
    const security = await applyJoinRoomSecurity({
      peers: [...peers.values()],
      roomAccessKey: this.roomAccessKey,
      info: currentInfo,
      passwordVerifier,
    }, roomInfoUpdate);
    await this.saveRoomPasswordVerifier(security.passwordVerifier);
    const roomInfo = await this.applyRoomInfoUpdate(security.infoUpdate);
    const roomAccessKey = await this.ensureRoomAccessKey();

    const previousPeer = peers.get(peerId);
    // Updating an existing peerId requires proof of ownership (its token), so a
    // caller cannot overwrite another peer's candidates and hijack the media
    // path. A fresh peerId is issued a new server-generated token.
    const presentedToken = peerTokenFromRequest(request);
    if (previousPeer && !peerTokenMatches(previousPeer.token, presentedToken)) {
      throw new HttpError(403, "peer_token_invalid",
        "This peerId is already claimed; a valid peer token is required to update it.");
    }

    // Bound the number of distinct peers per room. Adding a brand-new peer to
    // an already-full room is rejected; re-announcing an existing peer is
    // always allowed so active members are never locked out.
    if (!previousPeer && peers.size >= MAX_PEERS_PER_ROOM) {
      throw new HttpError(409, "room_full", "This room already has the maximum number of participants.");
    }

    const token = previousPeer ? previousPeer.token : generatePeerToken();
    const nextPeer: Peer = { peerId, candidates, metadata, lastSeen: now, token };
    const shouldBroadcastPeer =
      !previousPeer || peerAnnouncementKey(previousPeer) !== peerAnnouncementKey(nextPeer);
    peers.set(peerId, nextPeer);
    await this.savePeers(roomId, peers, now);
    // Host role is bound to a peer's token: only the first claimant of a peerId
    // (or a re-announce with its token, checked above) can assert host.
    if (isHost && (await this.loadHostPeerId()) !== peerId) {
      await this.saveHostPeerId(peerId);
    }
    if (shouldBroadcastPeer) {
      this.broadcastRoomEvent(roomId, {
        type: previousPeer ? "peer_updated" : "peer_joined",
        peer: publicPeerForEvent(nextPeer),
      }, peerId);
    }

    return jsonResponse({
      ok: true,
      peerToken: token,
      roomAccessKey,
      roomName: roomInfo?.name,
      passwordProtected: roomInfo?.passwordProtected === true,
      peers: otherPeers({ peers: [...peers.values()] }, peerId),
    });
  }

  private async handlePeers(request: Request, roomId: string): Promise<Response> {
    const url = new URL(request.url);
    const peerId = validatePeerId(url.searchParams.get("peerId"));
    const peers = await this.loadPeers();
    const changed = this.cleanupPeers(peers);
    if (changed) {
      await this.savePeers(roomId, peers);
    }
    // Candidates (peer IP:port) and the room access key are only returned to a
    // caller that has joined this room and presents its peer token. This stops
    // anonymous callers from harvesting participants' IPs and the media key.
    const self = peers.get(peerId);
    if (!self || !peerTokenMatches(self.token, peerTokenFromRequest(request))) {
      throw new HttpError(403, "peer_token_invalid",
        "A valid peer token for a joined peer is required to read room peers.");
    }
    const roomInfo = await this.loadRoomInfo();
    const roomAccessKey = await this.ensureRoomAccessKey();

    return jsonResponse({
      ok: true,
      roomAccessKey,
      roomName: roomInfo?.name,
      passwordProtected: roomInfo?.passwordProtected === true,
      peers: otherPeers({ peers: [...peers.values()] }, peerId),
    });
  }

  private async handleHeartbeat(request: Request, roomId: string): Promise<Response> {
    const body = await readJson<PeerRequest>(request);
    const peerId = validatePeerId(body.peerId);
    const now = Date.now();
    const peers = await this.loadPeers();
    this.cleanupPeers(peers, now);
    // Only the owning peer may refresh its own liveness. Return the same error
    // whether the peer is absent or the token is wrong, so heartbeat cannot be
    // used to probe who is in a (locked) room.
    const peer = peers.get(peerId);
    if (!peer || !peerTokenMatches(peer.token, peerTokenFromRequest(request))) {
      await this.savePeers(roomId, peers, now);
      throw new HttpError(403, "peer_token_invalid", "A valid peer token is required to heartbeat.");
    }

    peer.lastSeen = now;
    await this.savePeers(roomId, peers, now);

    return jsonResponse({ ok: true });
  }

  private async handleLeave(request: Request, roomId: string): Promise<Response> {
    const body = await readJson<PeerRequest>(request);
    const peerId = validatePeerId(body.peerId);
    const peers = await this.loadPeers();
    const leaving = peers.get(peerId);
    // A peer may only remove itself, proven by its token. A missing peer or a
    // caller without the matching token is a silent no-op success: this blocks
    // unauthenticated eviction / room teardown (which bypassed the room
    // password) without revealing whether the peer exists.
    if (!leaving || !peerTokenMatches(leaving.token, peerTokenFromRequest(request))) {
      return jsonResponse({ ok: true });
    }
    const removed = peers.delete(peerId);
    const hostPeerId = await this.loadHostPeerId();

    if (removed && hostPeerId === peerId) {
      // The host left: close the whole room. Tell remaining viewers first, then
      // wipe all room state (the empty-map branch of savePeers also clears the
      // access key, info, password verifier, hostPeerId, and the directory entry).
      this.broadcastRoomEvent(roomId, { type: "room_closed" }, peerId);
      peers.clear();
      await this.savePeers(roomId, peers);
      return jsonResponse({ ok: true });
    }

    await this.savePeers(roomId, peers);
    if (removed) {
      this.broadcastRoomEvent(roomId, {
        type: "peer_left",
        peerId,
      }, peerId);
    }

    return jsonResponse({ ok: true });
  }

  private async handleSummary(roomId: string): Promise<Response> {
    const now = Date.now();
    const peers = await this.loadPeers();
    const changed = this.cleanupPeers(peers, now);
    const summary = roomSummary(roomId, [...peers.values()], await this.loadRoomInfo(), now);
    if (changed || !summary) {
      await this.savePeers(roomId, peers, now);
    } else {
      await trySyncRoomDirectory(this.env, roomId, [...peers.values()], await this.loadRoomInfo(), now);
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

  const match = pathname.match(/^\/rooms\/([^/]+)\/(join|peers|heartbeat|leave|summary|events)$/);
  if (!match) {
    throw new HttpError(404, "not_found", "Endpoint not found.");
  }

  const roomId = validateRoomId(decodeURIComponent(match[1]));
  const action = match[2];

  if (!env.ROOM_OBJECTS) {
    // Per-room state is served only by the authenticated RoomObject Durable
    // Object. The legacy unauthenticated KV path has been retired, so refuse
    // rather than fall back to it.
    throw new HttpError(503, "durable_objects_required",
      "Room signaling requires the RoomObject Durable Object binding.");
  }
  void action;
  const id = env.ROOM_OBJECTS.idFromName(roomId);
  return env.ROOM_OBJECTS.get(id).fetch(request);
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
