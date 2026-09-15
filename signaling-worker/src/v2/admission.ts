import { normalizeName } from './protocol';

export class AdmissionError extends Error {
  constructor(readonly status: number, readonly code: string) { super(code); }
}
export type Policy = { name: string; visibility: 'public' | 'unlisted'; viewerLimit: number };
export type Admission = { nickname: string; password: string; policy?: Policy };
export type Verifier = { algorithm: 'pbkdf2-sha256'; iterations: number; salt: string; hash: string };
const encoder = new TextEncoder();
function exact(value: unknown, keys: string[]): value is Record<string, unknown> {
  return value !== null && typeof value === 'object' && !Array.isArray(value) &&
    Object.keys(value).length === keys.length && keys.every(k => Object.hasOwn(value, k));
}
export async function readAdmission(request: Request, create: boolean): Promise<Admission> {
  const invalid = () => new AdmissionError(400, 'invalid_request');
  if (request.headers.get('content-type')?.split(';')[0].trim().toLowerCase() !== 'application/json' || !request.body) throw invalid();
  const reader = request.body.getReader();
  const chunks: Uint8Array[] = [];
  let size = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      size += value.length;
      if (size > 16384) { await reader.cancel(); throw invalid(); }
      chunks.push(value);
    }
  } finally { reader.releaseLock(); }
  const bytes = new Uint8Array(size);
  let offset = 0;
  for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.length; }
  let v: unknown;
  try { v = JSON.parse(new TextDecoder('utf-8', { fatal: true, ignoreBOM: false }).decode(bytes)); } catch { throw invalid(); }
  if (!exact(v, create ? ['v', 'nickname', 'password', 'policy'] : ['v', 'nickname', 'password']) || v.v !== 2) throw invalid();
  const nickname = normalizeName(v.nickname);
  const password = v.password;
  if (!nickname || typeof password !== 'string' || encoder.encode(password).length > 128 || /[\u0000-\u001f\u007f]/u.test(password) ||
      [...password].some(c => { const n = c.codePointAt(0)!; return n >= 0xd800 && n <= 0xdfff; })) throw invalid();
  if (!create) return { nickname, password };
  const p = v.policy;
  if (!exact(p, ['name', 'visibility', 'viewerLimit'])) throw invalid();
  const name = normalizeName(p.name, 64, 256);
  if (!name || (p.visibility !== 'public' && p.visibility !== 'unlisted') || typeof p.viewerLimit !== 'number' ||
      !Number.isInteger(p.viewerLimit) || p.viewerLimit < 1 || p.viewerLimit > 63) throw invalid();
  return { nickname, password, policy: { name, visibility: p.visibility, viewerLimit: p.viewerLimit } };
}
function encode(bytes: Uint8Array): string { return btoa(String.fromCharCode(...bytes)).replaceAll('+', '-').replaceAll('/', '_').replaceAll('=', ''); }
export function randomId(bytes = 16): string { return encode(crypto.getRandomValues(new Uint8Array(bytes))); }
export async function tokenHash(token: string): Promise<string> {
  return encode(new Uint8Array(await crypto.subtle.digest('SHA-256', encoder.encode(token))));
}
export function validToken(token: string): boolean { return /^[A-Za-z0-9_-]{42}[AEIMQUYcgkosw048]$/.test(token); }
export function constantEqual(a: string, b: string): boolean {
  let difference = a.length ^ b.length;
  for (let i = 0; i < Math.max(a.length, b.length); ++i) difference |= (a.charCodeAt(i) || 0) ^ (b.charCodeAt(i) || 0);
  return difference === 0;
}
async function derive(password: string, salt: string): Promise<string> {
  const key = await crypto.subtle.importKey('raw', encoder.encode(password), 'PBKDF2', false, ['deriveBits']);
  const bytes = Uint8Array.from(atob(salt.replaceAll('-', '+').replaceAll('_', '/')), c => c.charCodeAt(0));
  return encode(new Uint8Array(await crypto.subtle.deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt: bytes, iterations: 100000 }, key, 256)));
}
export async function passwordVerifier(password: string): Promise<Verifier | undefined> {
  if (!password) return undefined;
  const salt = randomId();
  return { algorithm: 'pbkdf2-sha256', iterations: 100000, salt, hash: await derive(password, salt) };
}
export async function verifyPassword(password: string, verifier?: Verifier): Promise<boolean> {
  return !verifier || (verifier.algorithm === 'pbkdf2-sha256' && verifier.iterations === 100000 && constantEqual(await derive(password, verifier.salt), verifier.hash));
}
export function json(value: unknown, status = 200): Response {
  return Response.json(value, { status, headers: { 'Cache-Control': 'no-store' } });
}
export function failure(error: unknown): Response {
  return error instanceof AdmissionError ? json({ v: 2, error: error.code }, error.status) : json({ v: 2, error: 'unavailable' }, 503);
}
