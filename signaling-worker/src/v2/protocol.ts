// Wire validation only. Membership, role, socket/ICE generations and rate limits
// must be checked by the authenticated dispatcher before acting on a command.
export type WireObject = Record<string, unknown>;
export type Validation = { ok: true; message: WireObject } | { ok: false; error: string };
const metadataLimit = 16 * 1024;
const signalingLimit = 64 * 1024;
const utf8 = new TextEncoder();
function object(value: unknown): value is WireObject {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}
function keys(value: WireObject, required: string[], optional: string[] = []): boolean {
  return required.every(key => Object.hasOwn(value, key)) &&
    Object.keys(value).every(key => required.includes(key) || optional.includes(key));
}
function integer(value: unknown, min = 0, max = Number.MAX_SAFE_INTEGER): boolean {
  return typeof value === "number" && Number.isSafeInteger(value) && value >= min && value <= max;
}
function identifier(value: unknown): boolean {
  return typeof value === "string" && /^[A-Za-z0-9_-]{1,128}$/.test(value);
}
function text(value: unknown, min: number, maxBytes: number): value is string {
  if (typeof value !== "string" || value.length < min || utf8.encode(value).length > maxBytes) return false;
  for (const char of value) {
    const point = char.codePointAt(0)!;
    if (point >= 0xd800 && point <= 0xdfff) return false;
  }
  return true;
}
export function normalizeName(value: unknown, maxPoints = 32, maxBytes = 128): string | null {
  if (!text(value, 0, 1024)) return null;
  // Explicit Unicode White_Space set shared with Qt; JS trim additionally strips
  // BOM while Qt's Unicode whitespace table does not. Do not rely on either.
  const normalized = value.normalize("NFC").replace(/^[\u0009-\u000d\u0020\u0085\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000]+|[\u0009-\u000d\u0020\u0085\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000]+$/gu, "");
  const points = [...normalized];
  if (!points.length || points.length > maxPoints || utf8.encode(normalized).length > maxBytes) return null;
  if (/[\u0000-\u001f\u007f-\u009f\u061c\u200e\u200f\u202a-\u202e\u2066-\u2069\ufeff]/u.test(normalized)) return null;
  return normalized;
}
export function validateClientCommand(bytes: Uint8Array, scope: "room" | "directory" = "room"): Validation {
  const fail = (error: string): Validation => ({ ok: false, error });
  if (bytes.byteLength > signalingLimit) return fail("too_large");
  let message: unknown;
  try { message = JSON.parse(new TextDecoder("utf-8", { fatal: true, ignoreBOM: false }).decode(bytes)); }
  catch { return fail("invalid_json"); }
  if (!object(message) || message.v !== 2 || typeof message.type !== "string" || !object(message.payload))
    return fail("invalid_envelope");
  const type = message.type;
  const signal = ["signal.offer", "signal.answer", "signal.candidate", "signal.restart_request"].includes(type);
  const mutation = ["profile.update", "room.update", "peer.disconnect", "peer.leave"].includes(type);
  if (!signal && !mutation && type !== "state.resync") return fail("invalid_envelope");
  if (!signal && bytes.byteLength > metadataLimit) return fail("too_large");
  const required = ["v", "type", "payload"];
  if (scope === "directory") {
    if (type !== "state.resync") return fail("invalid_envelope");
  } else required.push("roomId");
  if (mutation) required.push("requestId");
  if (signal) required.push("connectionId", "toPeerId");
  if (!keys(message, required) || required.filter(key => key.endsWith("Id")).some(key => !identifier(message[key])))
    return fail("invalid_envelope");
  const p = message.payload;
  let valid = false;
  switch (type) {
    case "profile.update": {
      const nickname = normalizeName(p.nickname);
      valid = keys(p, ["nickname", "expectedRevision"]) && integer(p.expectedRevision) && nickname !== null;
      if (valid) p.nickname = nickname;
      break;
    }
    case "room.update": {
      valid = keys(p, ["expectedRevision"], ["name", "visibility", "viewerLimit"]) && integer(p.expectedRevision) && Object.keys(p).length > 1;
      if (Object.hasOwn(p, "name")) { const name = normalizeName(p.name, 64, 256); valid &&= name !== null; if (name !== null) p.name = name; }
      if (Object.hasOwn(p, "visibility")) valid &&= p.visibility === "public" || p.visibility === "unlisted";
      if (Object.hasOwn(p, "viewerLimit")) valid &&= integer(p.viewerLimit, 1, 63);
      break;
    }
    case "peer.disconnect": valid = keys(p, ["peerId"]) && identifier(p.peerId); break;
    case "signal.offer": case "signal.answer": valid = keys(p, ["sdp"]) && text(p.sdp, 1, 60 * 1024) && !p.sdp.includes("\0"); break;
    case "signal.candidate":
      valid = keys(p, ["candidate", "sdpMid", "sdpMLineIndex"], ["usernameFragment"]) && text(p.candidate, 0, 4096) && !p.candidate.includes("\0") &&
        (p.sdpMid === null || (text(p.sdpMid, 1, 64) && !p.sdpMid.includes("\0"))) && (p.sdpMLineIndex === null || integer(p.sdpMLineIndex, 0, 31)) &&
        (!Object.hasOwn(p, "usernameFragment") || p.usernameFragment === null || (text(p.usernameFragment, 1, 256) && !p.usernameFragment.includes("\0")));
      break;
    default: valid = keys(p, []);
  }
  return valid ? { ok: true, message } : fail("invalid_payload");
}


// Server output is canonical: do not silently repair server-supplied names.
function canonicalName(v: unknown, points = 32, bytes = 128): boolean {
  return typeof v === "string" && normalizeName(v, points, bytes) === v;
}
function policy(v: unknown): boolean {
  return object(v) && keys(v, ["name", "visibility", "viewerLimit", "passwordProtected"]) &&
    canonicalName(v.name, 64, 256) && ["public", "unlisted"].includes(v.visibility as string) &&
    integer(v.viewerLimit, 1, 63) && typeof v.passwordProtected === "boolean";
}
function member(v: unknown): v is WireObject {
  return object(v) && keys(v, ["peerId", "nickname", "role", "status"]) && identifier(v.peerId) &&
    canonicalName(v.nickname) && ["host", "viewer"].includes(v.role as string) &&
    ["connected", "reconnecting"].includes(v.status as string);
}
function summary(v: unknown): v is WireObject {
  return object(v) && keys(v, ["roomId", "name", "viewerCount", "viewerLimit", "passwordProtected", "status", "summaryVersion", "leaseExpiresAt"]) &&
    identifier(v.roomId) && canonicalName(v.name, 64, 256) && integer(v.viewerCount, 0, 63) &&
    integer(v.viewerLimit, 1, 63) && typeof v.passwordProtected === "boolean" &&
    ["open", "full", "reconnecting"].includes(v.status as string) && integer(v.summaryVersion) && integer(v.leaseExpiresAt) &&
    (v.status === "reconnecting" || (v.status === "full") === ((v.viewerCount as number) >= (v.viewerLimit as number)));
}
function roomState(p: WireObject): boolean {
  if (!keys(p, ["selfPeerId", "policy", "status", "members"]) || !identifier(p.selfPeerId) || !policy(p.policy) ||
      !["open", "reconnecting"].includes(p.status as string) || !Array.isArray(p.members) || p.members.length < 1 || p.members.length > 64 || !p.members.every(member)) return false;
  const ids = new Set(p.members.map(m => m.peerId));
  const hosts = p.members.filter(m => m.role === "host");
  return ids.size === p.members.length && ids.has(p.selfPeerId) && hosts.length === 1 &&
    ((p.status === "reconnecting") === (hosts[0].status === "reconnecting"));
}
export function validateServerEvent(bytes: Uint8Array, scope: "room" | "directory" = "room"): Validation {
  const fail = (error: string): Validation => ({ ok: false, error });
  if (bytes.byteLength > (scope === "directory" ? 256 : 64) * 1024) return fail("too_large");
  let m: unknown;
  try { m = JSON.parse(new TextDecoder("utf-8", { fatal: true, ignoreBOM: false }).decode(bytes)); }
  catch { return fail("invalid_json"); }
  if (!object(m) || m.v !== 2 || typeof m.type !== "string" || !object(m.payload)) return fail("invalid_envelope");
  const state = m.type === "state.snapshot" || m.type === "state.delta";
  const signal = ["signal.offer", "signal.answer", "signal.candidate", "signal.restart_request"].includes(m.type);
  if ((!state && !signal && m.type !== "command.result" && m.type !== "room.closed") || (scope === "directory" && !state)) return fail("invalid_envelope");
  if (!signal && !(scope === "directory" && m.type === "state.snapshot") && bytes.byteLength > metadataLimit) return fail("too_large");
  const required = ["v", "type", "payload", ...(scope === "room" ? ["roomId"] : []),
    ...(state ? ["revision"] : []), ...(signal ? ["connectionId", "fromPeerId", "toPeerId"] : []), ...(m.type === "command.result" ? ["requestId"] : [])];
  if (!keys(m, required) || required.filter(k => k.endsWith("Id")).some(k => !identifier(m[k])) || (state && !integer(m.revision))) return fail("invalid_envelope");
  const p = m.payload;
  let valid = false;
  if (signal) {
    if (m.fromPeerId === m.toPeerId) return fail("invalid_payload");
    const { fromPeerId, ...command } = m;
    const validated = validateClientCommand(utf8.encode(JSON.stringify(command)));
    return validated.ok ? { ok: true, message: m } : fail("invalid_payload");
  }
  if (m.type === "command.result") {
    valid = p.status === "ok" ? keys(p, ["status"]) : p.status === "conflict" ? keys(p, ["status", "currentRevision"]) && integer(p.currentRevision) :
      p.status === "error" && keys(p, ["status", "code"]) && ["forbidden", "not_found", "invalid_state", "rate_limited", "invalid_command"].includes(p.code as string);
  } else if (m.type === "room.closed") {
    valid = keys(p, ["reason"]) && ["host_left", "host_expired", "kicked", "server_shutdown"].includes(p.reason as string);
  } else if (m.type === "state.snapshot") {
    if (scope === "room") valid = roomState(p);
    else valid = keys(p, ["rooms"]) && Array.isArray(p.rooms) && p.rooms.length <= 500 && p.rooms.every(summary) && new Set(p.rooms.map(r => r.roomId)).size === p.rooms.length;
  } else if (scope === "directory") {
    valid = p.op === "upsert" ? keys(p, ["op", "room"]) && summary(p.room) : p.op === "remove" && keys(p, ["op", "roomId"]) && identifier(p.roomId);
  } else {
    valid = p.op === "policy" ? keys(p, ["op", "policy"]) && policy(p.policy) :
      p.op === "member.upsert" ? keys(p, ["op", "member"]) && member(p.member) :
      p.op === "member.remove" ? keys(p, ["op", "peerId"]) && identifier(p.peerId) :
      p.op === "host.status" && keys(p, ["op", "status"]) && ["open", "reconnecting"].includes(p.status as string);
  }
  return valid ? { ok: true, message: m } : fail("invalid_payload");
}
