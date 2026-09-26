import { json } from './admission';
import { validateClientCommand, validateServerEvent } from './protocol';

export type Summary = { roomId: string; name: string; hostNickname?: string; viewerCount: number; viewerLimit: number;
  passwordProtected: boolean; status: 'open' | 'full' | 'reconnecting' };
export type Publication = { roomId: string; version: number; leaseExpiresAt: number; room: Summary | null };
type Row = Publication;
type Subscriber = { connectedAt: number; window: number; count: number; hostNickname?: boolean };
const encode = (value: unknown) => new TextEncoder().encode(JSON.stringify(value));

// Listing reads one directory object and never contacts individual rooms.
// A null row is a short-lived version fence against delayed publication retries.
export class V2Directory {
  constructor(private ctx: DurableObjectState) {
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('v2:ping', 'v2:pong'));
  }
  private wire(row: Row) { return { ...row.room!, summaryVersion: row.version, leaseExpiresAt: row.leaseExpiresAt }; }
  private send(ws: WebSocket, value: unknown): void {
    try { ws.send(this.encodeFor(value, !!(ws.deserializeAttachment() as Subscriber)?.hostNickname)); } catch { try { ws.close(1011, 'delivery_failed'); } catch {} }
  }
  private encodeFor(value: unknown, hostNickname: boolean): string {
    // v1.0.0 validates exact summary keys. Feature opt-in also survives DO
    // hibernation, while attachments created before deployment default to off.
    return JSON.stringify(value, (key, item) => key === 'hostNickname' && !hostNickname ? undefined : item);
  }
  private async snapshot() {
    const rows = await this.ctx.storage.list<Row>({ prefix: 'room:' });
    return { v: 2, type: 'state.snapshot', revision: await this.ctx.storage.get<number>('revision') ?? 0,
      payload: { rooms: [...rows.values()].filter(row => row.room && row.leaseExpiresAt > Date.now()).map(row => this.wire(row)) } };
  }
  private async commit(key: string, row?: Row, payload?: unknown): Promise<void> {
    const revision = await this.ctx.storage.transaction(async txn => {
      if (row) await txn.put(key, row); else await txn.delete(key);
      if (payload === undefined) return undefined;
      const next = (await txn.get<number>('revision') ?? 0) + 1;
      await txn.put('revision', next);
      return next;
    });
    if (revision !== undefined) for (const ws of this.ctx.getWebSockets()) this.send(ws, { v: 2, type: 'state.delta', revision, payload });
  }
  private async schedule(): Promise<void> {
    const current = await this.ctx.storage.getAlarm();
    const now = Date.now();
    if (current === null || current <= now || current > now + 60000) await this.ctx.storage.setAlarm(now + 60000);
  }
  private async sweep(): Promise<void> {
    const rows = await this.ctx.storage.list<Row>({ prefix: 'room:' });
    for (const [key, row] of rows) if (row.leaseExpiresAt <= Date.now()) {
      await this.commit(key, undefined, row.room ? { op: 'remove', roomId: row.roomId } : undefined);
    }
  }
  async fetch(request: Request): Promise<Response> {
    return this.ctx.blockConcurrencyWhile(async () => {
      const path = new URL(request.url).pathname;
      await this.sweep();
      if (path === '/publish' && request.method === 'POST') {
        let publication: Publication;
        try { publication = await request.json(); } catch { return json({ error: 'invalid_publication' }, 400); }
        if (!publication || Object.keys(publication).sort().join(',') !== 'leaseExpiresAt,room,roomId,version' ||
            !/^[A-Za-z0-9_-]{22}$/.test(publication.roomId) || !Number.isSafeInteger(publication.version) || publication.version < 1 ||
            !Number.isSafeInteger(publication.leaseExpiresAt) || publication.leaseExpiresAt <= Date.now() || publication.leaseExpiresAt > Date.now() + 180000)
          return json({ error: 'invalid_publication' }, 400);
        if (publication.room !== null && (publication.room.roomId !== publication.roomId ||
            !validateServerEvent(encode({ v: 2, type: 'state.delta', revision: 0, payload: { op: 'upsert', room: this.wire(publication) } }), 'directory').ok))
          return json({ error: 'invalid_publication' }, 400);
        const key = 'room:' + publication.roomId;
        const previous = await this.ctx.storage.get<Row>(key);
        if (previous && publication.version < previous.version) return new Response(null, { status: 204 });
        if (previous && publication.version === previous.version && JSON.stringify(previous.room) !== JSON.stringify(publication.room))
          return json({ error: 'version_conflict' }, 409);
        if (publication.room && !previous?.room) {
          const rows = await this.ctx.storage.list<Row>({ prefix: 'room:' });
          if ([...rows.values()].filter(row => row.room).length >= 500) return json({ error: 'full' }, 409);
        }
        const next = { ...publication, leaseExpiresAt: Math.max(previous?.leaseExpiresAt ?? 0, publication.leaseExpiresAt) };
        let delta: unknown;
        // Equal-version lease renewal has no visible revision or broadcast.
        if (!previous || publication.version > previous.version) {
          if (publication.room) delta = { op: 'upsert', room: this.wire(next) };
          else if (previous?.room) delta = { op: 'remove', roomId: publication.roomId };
        }
        await this.commit(key, next, delta);
        await this.schedule();
        return new Response(null, { status: 204 });
      }
      const hostNickname = request.headers.get('X-ScreenShare-Directory-Features') === 'host-nickname';
      if (path === '/snapshot' && request.method === 'GET') return new Response(this.encodeFor(await this.snapshot(), hostNickname), {
        headers: { 'Content-Type': 'application/json' } });
      if (path !== '/events' || request.method !== 'GET' || request.headers.get('Upgrade')?.toLowerCase() !== 'websocket')
        return json({ error: 'invalid_request' }, 400);
      if (this.ctx.getWebSockets().length >= 512) return json({ v: 2, error: 'full' }, 409);
      const pair = new WebSocketPair();
      this.ctx.acceptWebSocket(pair[1]);
      pair[1].serializeAttachment({ connectedAt: Date.now(), window: 0, count: 0, hostNickname } satisfies Subscriber);
      this.send(pair[1], await this.snapshot());
      await this.schedule();
      return new Response(null, { status: 101, webSocket: pair[0] });
    });
  }
  async webSocketMessage(ws: WebSocket, raw: string | ArrayBuffer): Promise<void> {
    await this.ctx.blockConcurrencyWhile(async () => {
      const state = ws.deserializeAttachment() as Subscriber;
      const window = Math.floor(Date.now() / 60000);
      state.count = state.window === window ? state.count + 1 : 1;
      state.window = window;
      ws.serializeAttachment(state);
      const bytes = typeof raw === 'string' ? new TextEncoder().encode(raw) : new Uint8Array(raw);
      if (state.count > 6 || !validateClientCommand(bytes, 'directory').ok) { ws.close(1008, 'invalid_or_excessive_command'); return; }
      await this.sweep();
      this.send(ws, await this.snapshot());
    });
  }
  async webSocketClose(): Promise<void> {}
  async webSocketError(ws: WebSocket): Promise<void> { try { ws.close(1011, 'socket_error'); } catch {} }
  async alarm(): Promise<void> {
    await this.ctx.blockConcurrencyWhile(async () => {
      await this.sweep();
      for (const ws of this.ctx.getWebSockets()) {
        const last = this.ctx.getWebSocketAutoResponseTimestamp(ws)?.getTime() ?? (ws.deserializeAttachment() as Subscriber).connectedAt;
        if (last + 90000 <= Date.now()) { try { ws.close(1000, 'idle'); } catch {} }
      }
      if (this.ctx.getWebSockets().length || (await this.ctx.storage.list({ prefix: 'room:', limit: 1 })).size) await this.schedule();
    });
  }
}
