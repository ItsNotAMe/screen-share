import { test } from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import { Miniflare } from 'miniflare';
import { validateServerEvent } from '../src/v2/protocol.ts';

test('directory publication, subscriptions, retries and leases in workerd', { timeout: 60000 }, async t => {
  // Fault injection is bundled only into this test entry point. No production
  // route exposes these controls, and both actual DO implementations still run.
  const bundle = await build({ stdin: { resolveDir: process.cwd(), contents: `
    export { default, V2Control } from './src/v2/worker.ts';
    import { V2Room as Room } from './src/v2/room.ts';
    import { V2Directory as Directory } from './src/v2/directory.ts';
    export class V2Room extends Room {
      constructor(ctx, env) { super(ctx, env); this.testCtx = ctx; this.calls = 0; }
      async fetch(request) {
        const path = new URL(request.url).pathname;
        if (path === '/test/state') return Response.json(await this.testCtx.storage.get('state') ?? null);
        if (path === '/test/delivery') return Response.json({ running: this.outbox.running });
        if (path === '/test/calls') return Response.json(this.calls);
        if (path === '/test/expire-provisional') {
          await this.stateLane.run(async () => {
          const state = await this.testCtx.storage.get('state');
          for (const member of state.members) if (!member.attached) member.expires = 0;
          await this.testCtx.storage.put('state', state);
          });
          await this.alarm(); return new Response(null, { status: 204 });
        }
        if (path === '/test/alarm' || path === '/test/renew') {
          if (path === '/test/renew') {
            await this.stateLane.run(async () => {
            const state = await this.testCtx.storage.get('state'); state.directory.nextRenew = 0;
            await this.testCtx.storage.put('state', state);
            });
          }
          await this.alarm(); return new Response(null, { status: 204 });
        }
        ++this.calls; return super.fetch(request);
      }
    }
    export class V2Directory extends Directory {
      constructor(ctx) { super(ctx); this.testCtx = ctx; this.fault = ''; this.holds = []; this.active = 0; this.maximum = 0; }
      async fetch(request) {
        const path = new URL(request.url).pathname;
        if (path === '/test/fault') { this.fault = await request.text(); return new Response(null, { status: 204 }); }
        if (path === '/test/inflight') return Response.json({ active: this.active, maximum: this.maximum });
        if (path === '/test/release') { this.fault = ''; for (const resolve of this.holds.splice(0)) resolve(); return new Response(null, { status: 204 }); }
        if (path === '/test/expire') {
          const rows = await this.testCtx.storage.list({ prefix: 'room:' });
          for (const [key, row] of rows) { row.leaseExpiresAt = 0; await this.testCtx.storage.put(key, row); }
          await this.alarm(); return new Response(null, { status: 204 });
        }
        if (path === '/test/rows') return Response.json([...await this.testCtx.storage.list({ prefix: 'room:' })]);
        if (path === '/publish' && this.fault === 'before') return new Response(null, { status: 503 });
        if (path === '/publish' && this.fault === 'hold') {
          ++this.active; this.maximum = Math.max(this.maximum, this.active);
          await new Promise(resolve => this.holds.push(resolve));
          --this.active;
        }
        const response = await super.fetch(request);
        if (path === '/publish' && this.fault === 'after') return new Response(null, { status: 503 });
        return response;
      }
    }
  ` }, bundle: true, write: false, format: 'esm', target: 'es2022' });
  const mf = new Miniflare({ modules: true, script: bundle.outputFiles[0].text, compatibilityDate: '2026-05-21', durableObjects: {
    V2_ROOMS: { className: 'V2Room', useSQLite: true }, V2_CONTROL: { className: 'V2Control', useSQLite: true }, V2_DIRECTORY: { className: 'V2Directory', useSQLite: true } } });
  t.after(() => mf.dispose());
  const request = (path, body, headers = {}) => mf.dispatchFetch('https://test' + path, { method: body === undefined ? 'GET' : 'POST',
    headers: { 'CF-Connecting-IP': '192.0.2.10', 'Content-Type': 'application/json', ...headers }, body: body === undefined ? undefined : JSON.stringify(body) });
  const namespace = await mf.getDurableObjectNamespace('V2_DIRECTORY');
  const directory = namespace.get(namespace.idFromName('directory'));
  const fault = mode => directory.fetch('https://internal/test/fault', { method: 'POST', body: mode });
  const list = async () => {
    const response = await request('/v2/rooms'); assert.equal(response.status, 200);
    const value = await response.json(); assert.equal(validateServerEvent(new TextEncoder().encode(JSON.stringify(value)), 'directory').ok, true); return value;
  };
  async function socket(path, token, features = {}) {
    const response = await request(path, undefined, { Upgrade: 'websocket', ...features, ...(token ? { Authorization: 'Bearer ' + token } : {}) });
    assert.equal(response.status, 101);
    const ws = response.webSocket, messages = [];
    ws.addEventListener('message', event => messages.push(event.data === 'v2:pong' ? event.data : JSON.parse(event.data)));
    ws.accept(); t.after(() => { try { ws.close(); } catch {} });
    await until(() => messages.length > 0);
    return { ws, messages };
  }
  const subscription = await socket('/v2/directory/events');
  const modern = await socket('/v2/directory/events', undefined, { 'X-ScreenShare-Directory-Features': 'host-nickname' });
  assert.deepEqual(subscription.messages[0].payload.rooms, []);
  subscription.ws.send('v2:ping'); await until(() => subscription.messages.includes('v2:pong'));
  const host = await (await request('/v2/rooms', { v: 2, nickname: 'Host', password: 'secret', policy: { name: 'Public', visibility: 'public', viewerLimit: 2 } })).json();
  assert.equal((await list()).payload.rooms.length, 0);
  const rooms = await mf.getDurableObjectNamespace('V2_ROOMS');
  const room = rooms.get(rooms.idFromName(host.roomId));
  const state = async () => (await room.fetch('https://internal/test/state')).json();
  const idle = () => until(async () => !(await room.fetch('https://internal/test/delivery').then(r => r.json())).running);
  await fault('before');
  let hosting = await socket(`/v2/rooms/${host.roomId}/events`, host.token);
  assert.equal((await list()).payload.rooms.length, 0);
  assert.ok((await state()).directory.pending);
  await idle();
  await fault(''); await room.fetch('https://internal/test/alarm');
  await until(() => subscription.messages.some(m => m.payload?.op === 'upsert'));
  const listed = await list();
  assert.equal(listed.payload.rooms[0].name, 'Public');
  assert.equal(Object.hasOwn(listed.payload.rooms[0], 'hostNickname'), false);
  assert.equal(Object.hasOwn(subscription.messages.find(m => m.payload?.op === 'upsert').payload.room, 'hostNickname'), false);
  await until(() => modern.messages.some(m => m.payload?.room?.hostNickname === 'Host'));
  const modernList = await (await request('/v2/rooms', undefined, { 'X-ScreenShare-Directory-Features': 'host-nickname' })).json();
  assert.equal(modernList.payload.rooms[0].hostNickname, 'Host');
  const modernSnapshot = await socket('/v2/directory/events', undefined, { 'X-ScreenShare-Directory-Features': 'host-nickname' });
  assert.equal(modernSnapshot.messages[0].payload.rooms[0].hostNickname, 'Host');
  assert.equal(listed.payload.rooms[0].passwordProtected, true);
  assert.equal((await state()).directory.pending, undefined);
  for (let i = 0; i < 2; ++i) assert.equal((await request(`/v2/rooms/${host.roomId}/join`, { v: 2, nickname: 'Viewer', password: 'secret' })).status, 200);
  assert.equal((await list()).payload.rooms[0].status, 'full');
  assert.equal((await list()).payload.rooms[0].viewerCount, 2);
  await room.fetch('https://internal/test/expire-provisional');
  assert.equal((await list()).payload.rooms[0].status, 'open');
  assert.equal((await list()).payload.rooms[0].viewerCount, 0);
  hosting.ws.close(1000, 'reconnect-test');
  await until(async () => (await list()).payload.rooms[0].status === 'reconnecting');
  hosting = await socket(`/v2/rooms/${host.roomId}/events`, host.token);
  assert.equal((await list()).payload.rooms[0].status, 'open');
  const roomCalls = await (await room.fetch('https://internal/test/calls')).json();
  for (let i = 0; i < 5; ++i) await list();
  assert.equal(await (await room.fetch('https://internal/test/calls')).json(), roomCalls);
  const beforeRenew = (await list()).revision;
  await room.fetch('https://internal/test/renew');
  assert.equal((await list()).revision, beforeRenew);
  const storedBeforePing = await directory.fetch('https://internal/test/rows').then(r => r.json());
  const pongCount = subscription.messages.filter(m => m === 'v2:pong').length;
  subscription.ws.send('v2:ping');
  await until(() => subscription.messages.filter(m => m === 'v2:pong').length > pongCount);
  assert.deepEqual(await directory.fetch('https://internal/test/rows').then(r => r.json()), storedBeforePing);
  let id = 0;
  async function update(fields) {
    const requestId = 'update' + ++id;
    hosting.ws.send(JSON.stringify({ v: 2, type: 'room.update', roomId: host.roomId, requestId, payload: { expectedRevision: (await state()).revision, ...fields } }));
    await until(() => hosting.messages.some(m => m.requestId === requestId));
    assert.equal(hosting.messages.find(m => m.requestId === requestId).payload.status, 'ok');
  }
  await fault('hold');
  await update({ name: 'BlockedOld' });
  await until(async () => (await directory.fetch('https://internal/test/inflight').then(r => r.json())).active === 1);
  const heldVersion = (await state()).directory.pending.version;
  // Each operation must finish while the directory request is still held.
  // The bound is a test watchdog, not a gaming-latency measurement.
  const deadline = async work => {
    let timer;
    try { return await Promise.race([work, new Promise((_, reject) => { timer = setTimeout(() => reject(new Error('room control blocked by directory')), 1500); })]); }
    finally { clearTimeout(timer); }
  };
  const duringStall = await deadline(request(`/v2/rooms/${host.roomId}/join`, { v: 2, nickname: 'DuringStall', password: 'secret' }).then(r => r.json()));
  const watching = await deadline(socket(`/v2/rooms/${host.roomId}/events`, duringStall.token));
  const snapshots = hosting.messages.filter(m => m.type === 'state.snapshot').length;
  hosting.ws.send(JSON.stringify({ v: 2, type: 'state.resync', roomId: host.roomId, payload: {} }));
  await deadline(until(() => hosting.messages.filter(m => m.type === 'state.snapshot').length > snapshots));
  hosting.ws.send(JSON.stringify({ v: 2, type: 'signal.offer', roomId: host.roomId, connectionId: 'heldDirectoryOffer', toPeerId: duringStall.peerId, payload: { sdp: 'v=0\r\n' } }));
  await deadline(until(() => watching.messages.some(m => m.type === 'signal.offer')));
  watching.ws.send(JSON.stringify({ v: 2, type: 'signal.answer', roomId: host.roomId, connectionId: 'heldDirectoryOffer', toPeerId: host.peerId, payload: { sdp: 'v=0\r\n' } }));
  await deadline(until(() => hosting.messages.some(m => m.type === 'signal.answer')));
  await deadline(update({ name: 'LatestWhileBlocked' }));
  assert.ok((await state()).directory.pending.version > heldVersion);
  assert.equal((await state()).policy.name, 'LatestWhileBlocked');
  assert.deepEqual(await directory.fetch('https://internal/test/inflight').then(r => r.json()), { active: 1, maximum: 1 });
  await directory.fetch('https://internal/test/release');
  await until(async () => (await list()).payload.rooms[0].name === 'LatestWhileBlocked' && !(await state()).directory.pending);
  assert.equal((await state()).members.length, 2); // late ACK did not restore an older roster
  watching.ws.send(JSON.stringify({ v: 2, type: 'peer.leave', roomId: host.roomId, requestId: 'stall-viewer-leave', payload: {} }));
  await until(async () => (await list()).payload.rooms[0].viewerCount === 0);
  await fault('after'); await update({ name: 'Renamed' });
  await until(async () => (await state()).directory.pending !== undefined);
  const renamed = await list(); assert.equal(renamed.payload.rooms[0].name, 'Renamed');
  await idle();
  await fault(''); await room.fetch('https://internal/test/alarm');
  assert.equal((await list()).revision, renamed.revision); // lost ACK retry is idempotent
  const old = (await directory.fetch('https://internal/test/rows').then(r => r.json()))[0][1];
  await update({ visibility: 'unlisted' });
  await until(async () => (await list()).payload.rooms.length === 0);
  assert.equal((await directory.fetch('https://internal/publish', { method: 'POST', body: JSON.stringify(old) })).status, 204);
  assert.equal((await list()).payload.rooms.length, 0); // stale upsert cannot resurrect removal
  await update({ visibility: 'public' });
  await until(async () => (await list()).payload.rooms.length === 1);
  await directory.fetch('https://internal/test/expire');
  assert.equal((await list()).payload.rooms.length, 0);
  await room.fetch('https://internal/test/renew');
  assert.equal((await list()).payload.rooms.length, 1);
  await idle();
  // Exercise the real five-second fetch abort, then retry the persisted value.
  await fault('hold'); await update({ name: 'TimeoutRetry' });
  await until(async () => (await directory.fetch('https://internal/test/inflight').then(r => r.json())).active === 1);
  await until(async () => !(await room.fetch('https://internal/test/delivery').then(r => r.json())).running, 8000);
  assert.ok((await state()).directory.pending);
  await directory.fetch('https://internal/test/release');
  await room.fetch('https://internal/test/alarm');
  await until(async () => !(await state()).directory.pending && (await list()).payload.rooms[0].name === 'TimeoutRetry');
  await idle();
  // A late successful public upsert must not clear a newer closure tombstone.
  const closingHost = await (await request('/v2/rooms', { v: 2, nickname: 'Closing', password: '', policy: { name: 'CloseDuringDelivery', visibility: 'public', viewerLimit: 1 } })).json();
  const closingRoom = rooms.get(rooms.idFromName(closingHost.roomId));
  const closingState = async () => closingRoom.fetch('https://internal/test/state').then(r => r.json());
  await fault('hold');
  const closingSocket = await socket(`/v2/rooms/${closingHost.roomId}/events`, closingHost.token);
  await until(async () => (await directory.fetch('https://internal/test/inflight').then(r => r.json())).active === 1);
  closingSocket.ws.send(JSON.stringify({ v: 2, type: 'peer.leave', roomId: closingHost.roomId, requestId: 'close-in-flight', payload: {} }));
  await until(() => closingSocket.messages.some(m => m.type === 'room.closed'));
  assert.equal((await closingState()).directory.pending.room, null);
  await directory.fetch('https://internal/test/release');
  await until(async () => await closingState() === null);
  assert.equal((await list()).payload.rooms.some(r => r.roomId === closingHost.roomId), false);
  subscription.ws.send(JSON.stringify({ v: 2, type: 'state.resync', payload: {} }));
  await until(() => subscription.messages.filter(m => m.type === 'state.snapshot').length === 2);
  await fault('before');
  hosting.ws.send(JSON.stringify({ v: 2, type: 'peer.leave', roomId: host.roomId, requestId: 'close', payload: {} }));
  await until(() => hosting.messages.some(m => m.type === 'room.closed'));
  assert.equal((await state()).closed, true);
  await idle();
  assert.equal((await list()).payload.rooms.length, 1); // eventual removal while directory unavailable
  await fault(''); await room.fetch('https://internal/test/alarm');
  await until(async () => await state() === null);
  assert.equal(await state(), null);
  assert.equal((await list()).payload.rooms.length, 0);
  for (const event of subscription.messages.filter(m => m !== 'v2:pong'))
    assert.equal(validateServerEvent(new TextEncoder().encode(JSON.stringify(event)), 'directory').ok, true);
  const revisions = subscription.messages.filter(m => m.type === 'state.delta').map(m => m.revision);
  assert.ok(revisions.every((r, i) => i === 0 || r === revisions[i - 1] + 1));
  assert.equal((await request('/v2/directory/events', undefined, { Upgrade: 'websocket', Authorization: 'Bearer secret' })).status, 400);
  assert.equal((await request('/v2/directory/events')).status, 400);
  assert.equal((await request('/publish', {})).status, 404);
  // Runtime serialization at the public protocol's largest list/name sizes.
  const large = namespace.get(namespace.idFromName('large-directory-test'));
  const publication = i => ({ roomId: String(i).padStart(22, '0'), version: 1, leaseExpiresAt: Date.now() + 180000,
    room: { roomId: String(i).padStart(22, '0'), name: '😀'.repeat(64), viewerCount: 63, viewerLimit: 63, passwordProtected: true, status: 'full' } });
  const responses = await Promise.all(Array.from({ length: 501 }, (_, i) => large.fetch('https://internal/publish', { method: 'POST', body: JSON.stringify(publication(i)) })));
  assert.equal(responses.filter(r => r.status === 204).length, 500);
  assert.equal(responses.filter(r => r.status === 409).length, 1);
  const maximum = await large.fetch('https://internal/snapshot').then(r => r.text());
  assert.equal(JSON.parse(maximum).payload.rooms.length, 500);
  assert.equal(validateServerEvent(new TextEncoder().encode(maximum), 'directory').ok, true);
  let flooded = false;
  subscription.ws.addEventListener('close', () => { flooded = true; });
  for (let i = 0; i < 7; ++i) subscription.ws.send(JSON.stringify({ v: 2, type: 'state.resync', payload: {} }));
  await until(() => flooded);
});

async function until(predicate, timeout = 5000) {
  const end = Date.now() + timeout;
  while (!await predicate()) { if (Date.now() > end) throw new Error('event deadline exceeded'); await new Promise(resolve => setTimeout(resolve, 10)); }
}
