import { test } from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';

test('eight-hour steady-room operation model executes the production alarm/outbox code', async t => {
  t.mock.timers.enable({ apis: ['Date'], now: 1800000000000 });
  const originalPair = globalThis.WebSocketRequestResponsePair;
  globalThis.WebSocketRequestResponsePair = class {};
  t.after(() => { if (originalPair === undefined) delete globalThis.WebSocketRequestResponsePair; else globalThis.WebSocketRequestResponsePair = originalPair; });
  const bundle = await build({ stdin: { resolveDir: process.cwd(), contents: `
    export { V2Room, V2Directory, V2Control } from './src/v2/worker.ts';
  ` }, bundle: true, write: false, format: 'esm', target: 'es2022' });
  const { V2Room, V2Directory, V2Control } = await import('data:text/javascript;base64,' + Buffer.from(bundle.outputFiles[0].text).toString('base64'));
  async function runModel(maintenanceFirst) {
  const counts = { get: 0, put: 0, delete: 0, list: 0, listedRows: 0, getAlarm: 0, setAlarm: 0,
    transaction: 0, crossObjectCalls: 0, alarms: 0, broadcasts: 0 };
  const pending = new Set();
  class Store {
    data = new Map(); alarm = null;
    async get(key) { ++counts.get; return structuredClone(this.data.get(key)); }
    async put(key, value) { ++counts.put; this.data.set(key, structuredClone(value)); }
    async delete(key) { ++counts.delete; return this.data.delete(key); }
    async list({ prefix = '', limit = Infinity } = {}) {
      ++counts.list;
      const values = [...this.data].filter(([key]) => key.startsWith(prefix)).slice(0, limit);
      counts.listedRows += values.length; return new Map(structuredClone(values));
    }
    async getAlarm() { ++counts.getAlarm; return this.alarm; }
    async setAlarm(at) { ++counts.setAlarm; this.alarm = Number(at); }
    async transaction(fn) { ++counts.transaction; return fn(this); }
  }
  const contexts = [];
  function context(members = []) {
    const sockets = members.map(member => ({ readyState: 1, member, connectedAt: Date.now(),
      deserializeAttachment() { return { peerId: member.peerId, generation: member.generation, connectedAt: this.connectedAt }; },
      send() { ++counts.broadcasts; }, close() { this.readyState = 3; } }));
    const ctx = { storage: new Store(), sockets,
      setWebSocketAutoResponse() {},
      // Model assumption: each attached client supplies timely automatic pings.
      getWebSocketAutoResponseTimestamp() { return new Date(Date.now()); },
      getWebSockets(peer) { return sockets.filter(socket => socket.readyState === 1 && (!peer || socket.member.peerId === peer)); },
      blockConcurrencyWhile(fn) { return fn(); },
      waitUntil(work) { pending.add(work); work.finally(() => pending.delete(work)); } };
    contexts.push(ctx); return ctx;
  }
  const directoryContext = context(Array.from({ length: 10 }, (_, i) => ({ peerId: 'subscriber' + i, generation: 1 })));
  const directory = new V2Directory(directoryContext);
  const controlContext = context(); const control = new V2Control(controlContext);
  function namespace(object) {
    return { idFromName(name) { return name; }, get() { return { fetch(url, options) {
      ++counts.crossObjectCalls; return object.fetch(new Request(url, options));
    } }; } };
  }
  const env = { V2_DIRECTORY: namespace(directory), V2_CONTROL: namespace(control) };
  const rooms = [];
  for (let i = 0; i < 10; ++i) {
    const roomId = 'room_' + String(i).padStart(17, '0');
    assert.equal((await control.fetch(new Request('https://internal/reserve', { method: 'POST', body: roomId }))).status, 204);
    const members = Array.from({ length: 5 }, (_, j) => ({ peerId: 'member' + j, nickname: 'member', role: j ? 'viewer' : 'host',
      hash: 'model-only', generation: 1, expires: Date.now() + 90000, attached: true }));
    const ctx = context(members);
    ctx.storage.data.set('state', { roomId, policy: { name: 'Model', visibility: 'public', viewerLimit: 4 }, revision: 5,
      members, closed: false, capacityRenew: 0 });
    const room = new V2Room(ctx, env); rooms.push({ room, ctx });
    await room.alarm();
  }
  async function drain() { while (pending.size) await Promise.all([...pending]); }
  await drain();
  const before = { ...counts };
  async function maintenance() {
    for (const [object, ctx] of [[directory, directoryContext], [control, controlContext]]) {
      if (ctx.storage.alarm !== null && ctx.storage.alarm <= Date.now()) { ++counts.alarms; await object.alarm(); }
    }
  }
  for (let step = 0; step < 960; ++step) {
    t.mock.timers.setTime(Date.now() + 30000);
    if (maintenanceFirst) await maintenance();
    for (const { room, ctx } of rooms) if (ctx.storage.alarm <= Date.now()) { ++counts.alarms; await room.alarm(); }
    await drain();
    if (!maintenanceFirst) await maintenance();
  }
  await drain();
  const operations = Object.fromEntries(Object.keys(counts).map(key => [key, counts[key] - before[key]]));
  for (const { ctx } of rooms) {
    const state = ctx.storage.data.get('state');
    assert.equal(state.closed, false); assert.equal(state.members.length, 5);
    assert.equal(state.revision, 5); assert.equal(state.directory.pending, undefined);
  }
  assert.equal(directoryContext.storage.data.size, 11); // ten summaries + revision
  assert.equal(Object.keys(controlContext.storage.data.get('rooms')).length, 10);
  assert.equal(operations.broadcasts, 0, 'unchanged leases must not broadcast room-list changes');
  assert.equal(operations.crossObjectCalls, 9600, 'one directory and capacity renewal per room/minute');
  assert.ok(operations.put <= 35000 && operations.get <= 45000 && operations.listedRows <= 54000,
    'steady-state operation budget regressed; these are model operations, not billed SQL rows');
  assert.ok(operations.setAlarm <= 10600 && operations.put + operations.delete + operations.setAlarm <= 46000,
    'redundant alarm writes consume the service storage budget');
  assert.equal(operations.alarms, maintenanceFirst ? 10560 : 9600,
    'due directory/control maintenance can be fulfilled and rescheduled by simultaneous room renewal');
  t.diagnostic(JSON.stringify({ simulatedHours: 8, ordering: maintenanceFirst ? 'maintenance-first' : 'room-renewal-first',
    rooms: 10, participants: 50, directorySubscribers: 10, operations,
    assumptions: ['steady memberships', 'timely automatic pings', 'no signaling, retries, reconnects or user changes'],
    measuredProductionBilling: false, activeDurationMeasured: false, dailyHeadroomVerified: false }));
  }
  await runModel(false);
  await runModel(true);
});
