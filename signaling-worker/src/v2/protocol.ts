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
