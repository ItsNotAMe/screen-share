import { AdmissionError, constantEqual, failure, json, passwordVerifier, randomId, readAdmission, tokenHash, validToken, verifyPassword } from './admission';
import type { Policy, Verifier } from './admission';
import { validateClientCommand } from './protocol';

type Member = { peerId: string; nickname: string; role: 'host' | 'viewer'; hash: string; generation: number; expires: number; attached: boolean };
type State = { roomId: string; policy: Policy; verifier?: Verifier; revision: number; members: Member[]; closed: boolean };
type Attachment = { peerId: string; generation: number; connectedAt: number };
export interface RoomEnv { V2_ROOMS: DurableObjectNamespace; V2_CONTROL: DurableObjectNamespace; }

// All admission/attachment/expiry mutations share one input gate, including
// asynchronous password derivation. An overlapping join cannot overbook a room.
export class V2Room {
  constructor(private ctx: DurableObjectState, private env: RoomEnv) {
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('v2:ping', 'v2:pong'));
  }
  private sockets(member: Member): WebSocket[] {
    return this.ctx.getWebSockets(member.peerId).filter(ws => ws.deserializeAttachment()?.generation === member.generation && ws.readyState === 1);
  }
  private send(ws: WebSocket, value: unknown): void { try { ws.send(JSON.stringify(value)); } catch { try { ws.close(1011, 'delivery_failed'); } catch {} } }
  private view(member: Member) { return { peerId: member.peerId, nickname: member.nickname, role: member.role, status: this.sockets(member).length ? 'connected' : 'reconnecting' }; }
  private status(state: State): string { return this.sockets(state.members[0]).length ? 'open' : 'reconnecting'; }
  private snapshot(ws: WebSocket, state: State, self: Member): void {
    this.send(ws, { v: 2, type: 'state.snapshot', roomId: state.roomId, revision: state.revision,
      payload: { selfPeerId: self.peerId, policy: { ...state.policy, passwordProtected: !!state.verifier }, status: this.status(state),
        members: state.members.filter(m => m.attached).map(m => this.view(m)) } });
  }
  private delta(state: State, payload: unknown, exclude?: WebSocket): void {
    ++state.revision;
    for (const member of state.members) for (const ws of this.sockets(member)) if (ws !== exclude) this.send(ws, { v: 2, type: 'state.delta', roomId: state.roomId, revision: state.revision, payload });
  }
  private async save(state: State): Promise<void> { await this.ctx.storage.put('state', state); await this.ctx.storage.setAlarm(Date.now() + 30000); }
  private async close(state: State, reason: string): Promise<void> {
    state.closed = true;
    for (const ws of this.ctx.getWebSockets()) {
      this.send(ws, { v: 2, type: 'room.closed', roomId: state.roomId, payload: { reason } });
      try { ws.close(1000, 'room_closed'); } catch {}
    }
    // Retain a tombstone until the control-object release is acknowledged.
    await this.save(state);
    const response = await this.env.V2_CONTROL.get(this.env.V2_CONTROL.idFromName('capacity')).fetch('https://internal/release', { method: 'POST', body: state.roomId });
    if (!response.ok) throw new Error('release_failed');
    await this.ctx.storage.deleteAll();
    await this.ctx.storage.deleteAlarm();
  }
  async fetch(request: Request): Promise<Response> {
    try { return await this.ctx.blockConcurrencyWhile(async () => { try {
      const url = new URL(request.url);
      let state = await this.ctx.storage.get<State>('state');
      if (url.pathname === '/create' && request.method === 'POST') {
        if (state) throw new AdmissionError(409, 'closed');
        const input = await readAdmission(request, true);
        const roomId = request.headers.get('X-Room-Id')!;
        const token = randomId(32);
        const host: Member = { peerId: randomId(), nickname: input.nickname, role: 'host', hash: await tokenHash(token), generation: 0, expires: Date.now() + 30000, attached: false };
        state = { roomId, policy: input.policy!, verifier: await passwordVerifier(input.password), revision: 0, members: [host], closed: false };
        await this.save(state);
        return json({ v: 2, roomId, peerId: host.peerId, role: 'host', token }, 201);
      }
      if (!state) throw new AdmissionError(404, 'not_found');
      if (state.closed || state.members[0].expires <= Date.now() && !this.sockets(state.members[0]).length) throw new AdmissionError(409, 'closed');
      if (url.pathname === '/join' && request.method === 'POST') {
        const input = await readAdmission(request, false);
        if (!await verifyPassword(input.password, state.verifier)) throw new AdmissionError(403, 'forbidden');
        if (this.status(state) !== 'open') throw new AdmissionError(409, 'closed');
        // Expired provisional reservations do not consume capacity until alarm.
        state.members = state.members.filter(m => m.attached || m.expires > Date.now());
        if (state.members.length - 1 >= state.policy.viewerLimit) throw new AdmissionError(409, 'full');
        const token = randomId(32);
        const member: Member = { peerId: randomId(), nickname: input.nickname, role: 'viewer', hash: await tokenHash(token), generation: 0, expires: Date.now() + 30000, attached: false };
        state.members.push(member);
        await this.save(state);
        return json({ v: 2, roomId: state.roomId, peerId: member.peerId, role: 'viewer', token });
      }
      if (url.pathname !== '/events' || request.headers.get('Upgrade')?.toLowerCase() !== 'websocket') throw new AdmissionError(400, 'invalid_request');
      const token = request.headers.get('Authorization')?.match(/^Bearer (\S+)$/)?.[1] ?? '';
      if (!validToken(token)) throw new AdmissionError(403, 'forbidden');
      const hash = await tokenHash(token);
      const member = state.members.find(m => constantEqual(hash, m.hash));
      if (!member || member.expires <= Date.now() && !this.sockets(member).length) throw new AdmissionError(403, 'forbidden');
      const previous = this.ctx.getWebSockets(member.peerId);
      ++member.generation;
      member.attached = true;
      member.expires = Date.now() + 90000;
      const pair = new WebSocketPair();
      this.ctx.acceptWebSocket(pair[1], [member.peerId]);
      pair[1].serializeAttachment({ peerId: member.peerId, generation: member.generation, connectedAt: Date.now() } satisfies Attachment);
      for (const ws of previous) { try { ws.close(1000, 'replaced'); } catch {} }
      this.delta(state, member.role === 'host' ? { op: 'host.status', status: 'open' } : { op: 'member.upsert', member: this.view(member) }, pair[1]);
      await this.save(state);
      this.snapshot(pair[1], state, member);
      return new Response(null, { status: 101, webSocket: pair[0] });
    } catch (error) { return failure(error); } }); } catch (error) { return failure(error); }
  }
  async webSocketMessage(ws: WebSocket, raw: string | ArrayBuffer): Promise<void> {
    await this.ctx.blockConcurrencyWhile(async () => {
      const state = await this.ctx.storage.get<State>('state');
      const attachment = ws.deserializeAttachment() as Attachment;
      const member = state?.members.find(m => m.peerId === attachment.peerId && m.generation === attachment.generation);
      if (!state || state.closed || !member) { ws.close(1008, 'stale_membership'); return; }
      const validation = validateClientCommand(typeof raw === 'string' ? new TextEncoder().encode(raw) : new Uint8Array(raw));
      if (!validation.ok || validation.message.roomId !== state.roomId) { ws.close(1008, 'invalid_command'); return; }
      const message = validation.message;
      if (message.type === 'state.resync') { this.snapshot(ws, state, member); return; }
      // Unsupported commands fail explicitly until the authenticated dispatcher
      // lands; never forward signaling without connection-generation ownership.
      if (message.requestId) this.send(ws, { v: 2, type: 'command.result', roomId: state.roomId, requestId: message.requestId, payload: { status: 'error', code: 'invalid_state' } });
      else ws.close(1008, 'unsupported_command');
    });
  }
  async webSocketClose(ws: WebSocket): Promise<void> { await this.disconnected(ws); }
  async webSocketError(ws: WebSocket): Promise<void> { try { ws.close(1011, 'socket_error'); } catch {} await this.disconnected(ws); }
  private async disconnected(ws: WebSocket): Promise<void> {
    await this.ctx.blockConcurrencyWhile(async () => {
      const state = await this.ctx.storage.get<State>('state');
      const attachment = ws.deserializeAttachment() as Attachment;
      const member = state?.members.find(m => m.peerId === attachment.peerId && m.generation === attachment.generation);
      if (!state || state.closed || !member || this.sockets(member).length) return;
      member.expires = Date.now() + 90000;
      this.delta(state, member.role === 'host' ? { op: 'host.status', status: 'reconnecting' } : { op: 'member.upsert', member: this.view(member) });
      await this.save(state);
    });
  }
  async alarm(): Promise<void> {
    await this.ctx.blockConcurrencyWhile(async () => {
      const state = await this.ctx.storage.get<State>('state');
      if (!state) return;
      if (state.closed) { await this.close(state, 'host_expired'); return; }
      const now = Date.now();
      for (const member of state.members) for (const ws of this.sockets(member)) {
        const attached = ws.deserializeAttachment() as Attachment;
        const last = this.ctx.getWebSocketAutoResponseTimestamp(ws)?.getTime() ?? attached.connectedAt;
        member.expires = Math.max(member.expires, last + 90000);
      }
      if (state.members[0].expires <= now) { await this.close(state, 'host_expired'); return; }
      for (const member of state.members.slice(1)) if (member.expires <= now) {
        for (const ws of this.sockets(member)) { try { ws.close(1008, 'membership_expired'); } catch {} }
        state.members = state.members.filter(m => m !== member);
        if (member.attached) this.delta(state, { op: 'member.remove', peerId: member.peerId });
      }
      await this.save(state);
      const response = await this.env.V2_CONTROL.get(this.env.V2_CONTROL.idFromName('capacity')).fetch('https://internal/renew', { method: 'POST', body: state.roomId });
      if (!response.ok) await this.close(state, 'server_shutdown');
    });
  }
}
