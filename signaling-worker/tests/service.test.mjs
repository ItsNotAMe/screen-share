import { test } from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import { Miniflare } from 'miniflare';
import { validateServerEvent } from '../src/v2/protocol.ts';

test('v2 admission and membership in workerd', { timeout: 60000 }, async t => {
  // Inspection/expiry injection exists only in this generated test entry point.
  // Production routing cannot access storage or invoke these test endpoints.
  const bundle = await build({ stdin: { resolveDir: process.cwd(), contents: `
    export { default, V2Control } from './src/v2/worker.ts';
    import { V2Room as Room } from './src/v2/room.ts';
    export class V2Room extends Room {
      constructor(ctx, env) { super(ctx, env); this.testState = ctx; }
      async fetch(request) {
        const path = new URL(request.url).pathname;
        if (path === '/test/state') return Response.json(await this.testState.storage.get('state') ?? null);
        if (path === '/test/expire-provisional') {
          const state = await this.testState.storage.get('state');
          for (const member of state.members) if (!member.attached) member.expires = 0;
          await this.testState.storage.put('state', state);
          await this.alarm();
          return new Response(null, { status: 204 });
        }
        return super.fetch(request);
      }
    }
  ` }, bundle: true, write: false, format: 'esm', platform: 'browser', target: 'es2022' });
  const mf = new Miniflare({ modules: true, script: bundle.outputFiles[0].text, compatibilityDate: '2026-05-21',
    durableObjects: { V2_ROOMS: { className: 'V2Room', useSQLite: true }, V2_CONTROL: { className: 'V2Control', useSQLite: true } } });
  t.after(() => mf.dispose());
  const request = (path, body, extra = {}) => mf.dispatchFetch('https://test' + path, { method: body === undefined ? 'GET' : 'POST',
    headers: { 'CF-Connecting-IP': '192.0.2.1', ...(body === undefined ? {} : { 'Content-Type': 'application/json' }), ...extra },
    body: body === undefined ? undefined : JSON.stringify(body) });
  const input = { v: 2, nickname: ' Host ', password: 'secret', policy: { name: ' Test ', visibility: 'public', viewerLimit: 1 } };
  const made = await request('/v2/rooms', input);
  assert.equal(made.status, 201);
  const host = await made.json();
  const rooms = await mf.getDurableObjectNamespace('V2_ROOMS');
  const room = rooms.get(rooms.idFromName(host.roomId));
  const stored = JSON.stringify(await (await room.fetch('https://internal/test/state')).json());
  assert.equal(stored.includes(host.token), false);
  assert.equal(stored.includes('secret'), false);
  assert.equal(JSON.parse(stored).verifier.iterations, 100000);
  assert.deepEqual(Object.keys(host).sort(), ['peerId', 'role', 'roomId', 'token', 'v']);
  assert.match(host.token, /^[A-Za-z0-9_-]{43}$/);
  const path = `/v2/rooms/${host.roomId}`;
  assert.equal((await request(path + '/join', { v: 2, nickname: 'Viewer', password: 'secret' })).status, 409);
  assert.equal((await request(path + '/events', undefined, { Upgrade: 'websocket', Authorization: 'Bearer incorrect' })).status, 403);
  async function attach(member) {
    const response = await request(path + '/events', undefined, { Upgrade: 'websocket', Authorization: 'Bearer ' + member.token });
    assert.equal(response.status, 101);
    const ws = response.webSocket;
    const messages = [];
    ws.addEventListener('message', e => messages.push(e.data));
    ws.accept();
    t.after(() => { try { ws.close(); } catch {} });
    await until(() => messages.some(m => JSON.parse(m).type === 'state.snapshot'));
    const snapshot = JSON.parse(messages.find(m => JSON.parse(m).type === 'state.snapshot'));
    assert.equal(JSON.parse(messages[0]).type, 'state.snapshot');
    assert.equal(validateServerEvent(new TextEncoder().encode(JSON.stringify(snapshot))).ok, true);
    assert.equal(snapshot.payload.selfPeerId, member.peerId);
    return { ws, messages, snapshot };
  }
  const first = await attach(host);
  assert.equal(first.snapshot.payload.policy.name, 'Test');
  assert.equal((await request(path + '/join', { v: 2, nickname: 'Viewer', password: 'wrong' })).status, 403);
  const attempts = await Promise.all(Array.from({ length: 8 }, () => request(path + '/join', { v: 2, nickname: 'Viewer', password: 'secret' })));
  assert.equal(attempts.filter(r => r.status === 200).length, 1);
  assert.equal(attempts.filter(r => r.status === 409).length, 7);
  const viewer = await attempts.find(r => r.status === 200).json();
  await room.fetch('https://internal/test/expire-provisional');
  assert.equal((await request(path + '/events', undefined, { Upgrade: 'websocket', Authorization: 'Bearer ' + viewer.token })).status, 403);
  const readmitted = await (await request(path + '/join', { v: 2, nickname: 'Viewer', password: 'secret' })).json();
  const viewing = await attach(readmitted);
  assert.equal(viewing.snapshot.payload.members.length, 2);
  const replacement = await attach(host);
  replacement.ws.send('v2:ping');
  await until(() => replacement.messages.includes('v2:pong'));
  replacement.ws.send(JSON.stringify({ v: 2, type: 'state.resync', roomId: host.roomId, payload: {} }));
  await until(() => replacement.messages.filter(m => m !== 'v2:pong' && JSON.parse(m).type === 'state.snapshot').length === 2);
  assert.equal(JSON.parse(replacement.messages.at(-1)).payload.status, 'open');
  replacement.ws.close(1000, 'test_disconnect');
  await until(() => viewing.messages.some(m => JSON.parse(m).payload?.op === 'host.status' && JSON.parse(m).payload.status === 'reconnecting'));
  assert.equal((await request(path + '/join', { v: 2, nickname: 'Viewer', password: 'secret' })).status, 409);
  const recovered = await attach(host);
  assert.equal(recovered.snapshot.payload.status, 'open');
  assert.equal((await request('/v2/rooms', { ...input, role: 'host' })).status, 400);
  assert.equal((await request('/v2/rooms', { ...input, password: '😀'.repeat(33) })).status, 400);
  assert.equal((await request('/v2/rooms', input, { Origin: 'https://evil.test' })).status, 403);
  assert.equal((await request(path + '/events?token=' + host.token, undefined, { Upgrade: 'websocket' })).status, 400);
  const abandoned = await (await request('/v2/rooms', input)).json();
  const abandonedRoom = rooms.get(rooms.idFromName(abandoned.roomId));
  await abandonedRoom.fetch('https://internal/test/expire-provisional');
  assert.equal(await (await abandonedRoom.fetch('https://internal/test/state')).json(), null);
  assert.equal((await request(`/v2/rooms/${abandoned.roomId}/events`, undefined, { Upgrade: 'websocket', Authorization: 'Bearer ' + abandoned.token })).status, 404);
  for (let i = 0; i < 11; ++i) await request('/v2/rooms', input, { 'CF-Connecting-IP': '192.0.2.2' });
  assert.equal((await request('/v2/rooms', input, { 'CF-Connecting-IP': '192.0.2.2' })).status, 429);
  // Exercise the real authoritative object without burning admission CPU or
  // weakening the production cap for a test-specific configuration.
  const controls = await mf.getDurableObjectNamespace('V2_CONTROL');
  const capacity = controls.get(controls.idFromName('capacity-test'));
  const reserve = id => capacity.fetch('https://internal/reserve', { method: 'POST', body: id });
  const reservations = await Promise.all(Array.from({ length: 501 }, (_, i) => reserve(String(i).padStart(22, '0'))));
  assert.equal(reservations.filter(r => r.status === 204).length, 500);
  assert.equal(reservations.filter(r => r.status === 409).length, 1);
  await capacity.fetch('https://internal/release', { method: 'POST', body: '0'.repeat(22) });
  assert.equal((await reserve('X'.repeat(22))).status, 204);
  const stateNow = async () => (await room.fetch('https://internal/test/state')).json();
  async function command(socket, type, requestId, payload) {
    const start = socket.messages.length;
    socket.ws.send(JSON.stringify({ v: 2, type, roomId: host.roomId, requestId, payload }));
    await until(() => socket.messages.slice(start).some(raw => JSON.parse(raw).requestId === requestId));
    const result = JSON.parse(socket.messages.slice(start).find(raw => JSON.parse(raw).requestId === requestId));
    assert.equal(validateServerEvent(new TextEncoder().encode(JSON.stringify(result))).ok, true);
    return result.payload;
  }
  let revision = (await stateNow()).revision;
  assert.deepEqual(await command(viewing, 'room.update', 'forbidden', { expectedRevision: revision, name: 'Stolen' }), { status: 'error', code: 'forbidden' });
  const edit = { expectedRevision: revision, nickname: ' New Viewer ' };
  assert.deepEqual(await command(viewing, 'profile.update', 'rename', edit), { status: 'ok' });
  assert.equal((await stateNow()).members.find(m => m.peerId === readmitted.peerId).nickname, 'New Viewer');
  assert.deepEqual(await command(viewing, 'profile.update', 'rename', edit), { status: 'ok' });
  assert.equal((await stateNow()).revision, revision + 1);
  assert.deepEqual(await command(viewing, 'profile.update', 'rename', { ...edit, nickname: 'Different' }), { status: 'error', code: 'invalid_command' });
  assert.deepEqual(await command(recovered, 'room.update', 'stale', { expectedRevision: revision, name: 'Stale' }), { status: 'conflict', currentRevision: revision + 1 });
  revision = (await stateNow()).revision;
  assert.deepEqual(await command(recovered, 'room.update', 'expand', { expectedRevision: revision, viewerLimit: 2, visibility: 'unlisted' }), { status: 'ok' });
  const secondViewer = await (await request(path + '/join', { v: 2, nickname: 'Second', password: 'secret' })).json();
  const secondSocket = await attach(secondViewer);
  revision = (await stateNow()).revision;
  await command(recovered, 'room.update', 'shrink', { expectedRevision: revision, viewerLimit: 1 });
  assert.equal((await stateNow()).members.length, 3);
  assert.equal((await request(path + '/join', { v: 2, nickname: 'Overflow', password: 'secret' })).status, 409);
  assert.deepEqual(await command(viewing, 'peer.disconnect', 'no-kick', { peerId: secondViewer.peerId }), { status: 'error', code: 'forbidden' });
  assert.deepEqual(await command(recovered, 'peer.disconnect', 'kick', { peerId: readmitted.peerId }), { status: 'ok' });
  await until(() => viewing.messages.some(raw => JSON.parse(raw).type === 'room.closed'));
  assert.equal((await request(path + '/events', undefined, { Upgrade: 'websocket', Authorization: 'Bearer ' + readmitted.token })).status, 403);
  assert.deepEqual(await command(secondSocket, 'peer.leave', 'leave', {}), { status: 'ok' });
  assert.equal((await stateNow()).members.length, 1);
  assert.equal((await request(path + '/events', undefined, { Upgrade: 'websocket', Authorization: 'Bearer ' + secondViewer.token })).status, 403);
  const floodingMember = await (await request(path + '/join', { v: 2, nickname: 'Flood', password: 'secret' })).json();
  const flooding = await attach(floodingMember);
  let floodClosed = false;
  flooding.ws.addEventListener('close', () => { floodClosed = true; });
  for (let i = 0; i < 121; ++i) flooding.ws.send(JSON.stringify({ v: 2, type: 'state.resync', roomId: host.roomId, payload: {} }));
  await until(() => floodClosed);
  assert.deepEqual(await command(recovered, 'peer.leave', 'close', {}), { status: 'ok' });
  await until(() => recovered.messages.some(raw => JSON.parse(raw).type === 'room.closed'));
  assert.equal((await request(path + '/join', { v: 2, nickname: 'Late', password: 'secret' })).status, 404);
});

async function until(predicate) {
  const end = Date.now() + 5000;
  while (!predicate()) { if (Date.now() > end) throw new Error('event deadline exceeded'); await new Promise(resolve => setTimeout(resolve, 10)); }
}
