import { AdmissionError, failure, json, randomId, readAdmission, tokenHash } from './admission';
import type { RoomEnv } from './room';
import { roomCapacity } from './capacity';
export { V2Room } from './room';
export { V2Directory } from './directory';
interface Env extends RoomEnv { ALLOWED_ORIGINS?: string; V2_MAX_ROOMS?: string; }

// One object per hashed IP for admission budgets; one named object for the
// authoritative global room cap. Missing bindings fail closed at the router.
export class V2Control {
  constructor(private ctx: DurableObjectState, private env: Pick<Env, 'V2_MAX_ROOMS'> = {}) {}
  async fetch(request: Request): Promise<Response> {
    return this.ctx.blockConcurrencyWhile(async () => {
      const path = new URL(request.url).pathname;
      const now = Date.now();
      if (path === '/limit') {
        const kind = await request.text();
        const window = Math.floor(now / 60000);
        let budget = await this.ctx.storage.get<{ window: number; total: number; create: number; join: number }>('budget');
        if (!budget || budget.window !== window) budget = { window, total: 0, create: 0, join: 0 };
        ++budget.total;
        if (kind === 'create') ++budget.create;
        if (kind === 'join') ++budget.join;
        await this.ctx.storage.put('budget', budget);
        await this.ctx.storage.setAlarm(now + 120000);
        return new Response(null, { status: budget.total > 240 || budget.create > 10 || budget.join > 30 ? 429 : 204 });
      }
      const roomId = await request.text();
      if (!/^[A-Za-z0-9_-]{22}$/.test(roomId)) return new Response(null, { status: 400 });
      const rooms = await this.ctx.storage.get<Record<string, number>>('rooms') ?? {};
      for (const [id, expires] of Object.entries(rooms)) if (expires <= now) delete rooms[id];
      if (path === '/reserve') {
        let limit: number;
        try { limit = roomCapacity(this.env.V2_MAX_ROOMS); }
        catch { return new Response(null, { status: 503 }); }
        if (Object.keys(rooms).length >= limit) return new Response(null, { status: 409 });
        rooms[roomId] = now + 180000;
      } else if (path === '/renew') {
        if (!Object.hasOwn(rooms, roomId)) return new Response(null, { status: 409 });
        rooms[roomId] = now + 180000;
      } else if (path === '/release') delete rooms[roomId];
      else return new Response(null, { status: 404 });
      await this.ctx.storage.put('rooms', rooms);
      await this.ctx.storage.setAlarm(now + 60000);
      return new Response(null, { status: 204 });
    });
  }
  async alarm(): Promise<void> {
    await this.ctx.blockConcurrencyWhile(async () => {
      await this.ctx.storage.delete('budget');
      const rooms = await this.ctx.storage.get<Record<string, number>>('rooms');
      if (!rooms) return;
      for (const [id, expires] of Object.entries(rooms)) if (expires <= Date.now()) delete rooms[id];
      await this.ctx.storage.put('rooms', rooms);
      if (Object.keys(rooms).length) await this.ctx.storage.setAlarm(Date.now() + 60000);
    });
  }
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    try {
      const url = new URL(request.url);
      if (url.protocol !== 'https:' || url.search || url.username || url.password) throw new AdmissionError(400, 'invalid_request');
      const origin = request.headers.get('Origin');
      if (origin && !(env.ALLOWED_ORIGINS ?? '').split(',').map(s => s.trim()).filter(Boolean).includes(origin)) throw new AdmissionError(403, 'forbidden');
      if (!env.V2_ROOMS || !env.V2_CONTROL || !env.V2_DIRECTORY) throw new Error('missing_binding');
      const create = url.pathname === '/v2/rooms' && request.method === 'POST';
      const match = url.pathname.match(/^\/v2\/rooms\/([A-Za-z0-9_-]{1,128})\/(join|events)$/);
      const join = match?.[2] === 'join' && request.method === 'POST';
      const events = match?.[2] === 'events' && request.method === 'GET';
      const health = url.pathname === '/v2/health' && request.method === 'GET';
      const listing = url.pathname === '/v2/rooms' && request.method === 'GET';
      const directoryEvents = url.pathname === '/v2/directory/events' && request.method === 'GET';
      if (!create && !join && !events && !health && !listing && !directoryEvents) throw new AdmissionError(404, 'not_found');
      if ((events || directoryEvents) && request.headers.get('Upgrade')?.toLowerCase() !== 'websocket') throw new AdmissionError(400, 'invalid_request');
      const ip = request.headers.get('CF-Connecting-IP');
      if (!ip) throw new AdmissionError(403, 'forbidden');
      const limiter = env.V2_CONTROL.get(env.V2_CONTROL.idFromName('ip-' + await tokenHash(ip)));
      const allowed = await limiter.fetch('https://internal/limit', { method: 'POST', body: create ? 'create' : join ? 'join' : 'other' });
      if (allowed.status === 429) throw new AdmissionError(429, 'rate_limited');
      if (!allowed.ok) throw new Error('limiter_unavailable');
      if (health) return json({ v: 2, status: 'ok' });
      if (listing || directoryEvents) {
        if (request.headers.has('Authorization')) throw new AdmissionError(400, 'invalid_request');
        return await env.V2_DIRECTORY.get(env.V2_DIRECTORY.idFromName('directory')).fetch('https://internal/' + (listing ? 'snapshot' : 'events'), {
          headers: directoryEvents ? { Upgrade: request.headers.get('Upgrade') ?? '' } : {} });
      }
      if (create) {
        // Validate before reserving capacity. Forward only a canonical bounded
        // body and a server-generated ID; never trust client internal headers.
        const input = await readAdmission(request, true);
        const roomId = randomId();
        const capacity = env.V2_CONTROL.get(env.V2_CONTROL.idFromName('capacity'));
        const reservation = await capacity.fetch('https://internal/reserve', { method: 'POST', body: roomId });
        if (reservation.status === 409) throw new AdmissionError(409, 'full');
        if (!reservation.ok) throw new Error('capacity_unavailable');
        // On ambiguous failure keep the reservation: its lease and the room's
        // provisional alarm clean up without admitting beyond the global cap.
        return await env.V2_ROOMS.get(env.V2_ROOMS.idFromName(roomId)).fetch('https://internal/create', {
          method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Room-Id': roomId }, body: JSON.stringify({ v: 2, ...input }) });
      }
      const headers = new Headers();
      if (events) { headers.set('Upgrade', 'websocket'); headers.set('Authorization', request.headers.get('Authorization') ?? ''); }
      else headers.set('Content-Type', 'application/json');
      // Consume and bound incoming bodies before acquiring the room input gate.
      const admission = join ? await readAdmission(request, false) : undefined;
      return await env.V2_ROOMS.get(env.V2_ROOMS.idFromName(match![1])).fetch(new Request(`https://internal/${events ? 'events' : 'join'}`, {
        method: request.method, headers, body: admission ? JSON.stringify({ v: 2, ...admission }) : undefined }));
    } catch (error) { return failure(error); }
  }
} satisfies ExportedHandler<Env>;
