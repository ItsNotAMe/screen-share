import { test } from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import { Miniflare } from 'miniflare';

test('ten rooms: automatic heartbeats do no application/storage work and listing has no room fanout', { timeout: 60000 }, async t => {
  // Instrument only the generated test module. No production inspection routes.
  const bundle = await build({ stdin: { resolveDir: process.cwd(), contents: `
    export { default, V2Control } from './src/v2/worker.ts';
    import { V2Room as Room } from './src/v2/room.ts';
    import { V2Directory as Directory } from './src/v2/directory.ts';
    function counted(ctx, counts) {
      function storage(target) { return new Proxy(target, { get(object, key) {
        const value = Reflect.get(object, key, object);
        if (typeof value !== 'function') return value;
        return (...args) => {
          counts[key] = (counts[key] ?? 0) + 1;
          if (key === 'transaction') return value.call(object, txn => args[0](storage(txn)));
          return value.apply(object, args);
        };
      } }); }
      const wrapped = storage(ctx.storage);
      return new Proxy(ctx, { get(object, key) {
        if (key === 'storage') return wrapped;
        const value = Reflect.get(object, key, object);
        return typeof value === 'function' ? value.bind(object) : value;
      } });
    }
    export class V2Room extends Room {
      constructor(ctx, env) { const counts = {}; super(counted(ctx, counts), env); this.counts = counts; }
      async fetch(request) {
        if (new URL(request.url).pathname === '/test/counts') return Response.json({ counts: this.counts, busy: this.outbox.running });
        this.counts.fetch = (this.counts.fetch ?? 0) + 1; return super.fetch(request);
      }
      async webSocketMessage(...args) { this.counts.messages = (this.counts.messages ?? 0) + 1; return super.webSocketMessage(...args); }
    }
    export class V2Directory extends Directory {
      constructor(ctx) { const counts = {}; super(counted(ctx, counts)); this.counts = counts; }
      async fetch(request) {
        if (new URL(request.url).pathname === '/test/counts') return Response.json({ counts: this.counts });
        return super.fetch(request);
      }
      async webSocketMessage(...args) { this.counts.messages = (this.counts.messages ?? 0) + 1; return super.webSocketMessage(...args); }
    }
  ` }, bundle: true, write: false, format: 'esm', target: 'es2022' });
  const mf = new Miniflare({ modules: true, script: bundle.outputFiles[0].text, compatibilityDate: '2026-05-21',
    durableObjects: { V2_ROOMS: { className: 'V2Room', useSQLite: true }, V2_CONTROL: { className: 'V2Control', useSQLite: true }, V2_DIRECTORY: { className: 'V2Directory', useSQLite: true } } });
  t.after(() => mf.dispose());
  const sockets = [], rooms = [];
  t.after(() => { for (const socket of sockets) { try { socket.ws.close(); } catch {} } });
  const namespace = await mf.getDurableObjectNamespace('V2_ROOMS');
  const directoryNamespace = await mf.getDurableObjectNamespace('V2_DIRECTORY');
  const directory = directoryNamespace.get(directoryNamespace.idFromName('directory'));
  let requests = 0;
  const request = (path, ip, body, token) => {
    ++requests;
    return mf.dispatchFetch('https://test' + path, { method: body ? 'POST' : 'GET',
      headers: { 'CF-Connecting-IP': ip, 'Content-Type': 'application/json',
        ...(token !== undefined ? { Upgrade: 'websocket', ...(token ? { Authorization: 'Bearer ' + token } : {}) } : {}) },
      body: body ? JSON.stringify(body) : undefined });
  };
  async function attach(path, ip, token) {
    const response = await request(path, ip, undefined, token);
    assert.equal(response.status, 101);
    const socket = { ws: response.webSocket, pongs: 0, snapshots: 0 };
    socket.ws.addEventListener('message', event => { if (event.data === 'v2:pong') ++socket.pongs; else if (JSON.parse(event.data).type === 'state.snapshot') ++socket.snapshots; });
    socket.ws.accept(); sockets.push(socket);
    await until(() => socket.snapshots === 1);
  }
  for (let index = 0; index < 10; ++index) {
    const ip = '192.0.2.' + (index + 1);
    const response = await request('/v2/rooms', ip, { v: 2, nickname: 'Host', password: '', policy: { name: 'Cost test', visibility: 'public', viewerLimit: 4 } });
    assert.equal(response.status, 201);
    const host = await response.json();
    rooms.push(namespace.get(namespace.idFromName(host.roomId)));
    await attach('/v2/rooms/' + host.roomId + '/events', ip, host.token);
    for (let viewer = 0; viewer < 4; ++viewer) {
      const joined = await request('/v2/rooms/' + host.roomId + '/join', ip, { v: 2, nickname: 'Viewer', password: '' });
      assert.equal(joined.status, 200);
      await attach('/v2/rooms/' + host.roomId + '/events', ip, (await joined.json()).token);
    }
    await attach('/v2/directory/events', ip, '');
  }
  const count = async object => (await (await object.fetch('https://internal/test/counts')).json());
  await until(async () => (await Promise.all(rooms.map(count))).every(value => !value.busy));
  const before = await Promise.all([...rooms, directory].map(count));
  const beforeRequests = requests;
  // One heartbeat per connection, with real workerd automatic response handling.
  // Do not call this an eight-hour or production hibernation/billing measurement.
  for (const socket of sockets) socket.ws.send('v2:ping');
  await until(() => sockets.every(socket => socket.pongs === 1));
  const after = await Promise.all([...rooms, directory].map(count));
  assert.deepEqual(after, before, 'heartbeats must not invoke handlers or access storage');
  assert.equal(requests, beforeRequests, 'heartbeats must not create HTTP requests');
  for (let index = 0; index < 10; ++index) {
    const response = await request('/v2/rooms', '192.0.2.100');
    assert.equal(response.status, 200);
    assert.equal((await response.json()).payload.rooms.length, 10);
  }
  assert.deepEqual(await Promise.all(rooms.map(count)), before.slice(0, 10), 'listing must not verify individual rooms');
  t.diagnostic(JSON.stringify({ rooms: 10, participants: 50, directorySubscribers: 10, automaticHeartbeats: 60,
    heartbeatApplicationCalls: 0, heartbeatStorageCalls: 0, listingRoomFanout: 0,
    productionHibernationVerified: false, dailyHeadroomVerified: false }));
});

async function until(condition) {
  const deadline = Date.now() + 5000;
  while (!await condition()) { assert.ok(Date.now() < deadline, 'condition deadline'); await new Promise(resolve => setTimeout(resolve, 5)); }
}
