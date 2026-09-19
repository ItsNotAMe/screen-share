import { test } from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import { Miniflare } from 'miniflare';
import { roomCapacity } from '../src/v2/capacity.ts';

test('deployment room capacity is bounded and fails closed', () => {
  assert.equal(roomCapacity(undefined), 500);
  for (const n of [1, 2, 499, 500]) assert.equal(roomCapacity(String(n)), n);
  for (const invalid of ['', '0', '-1', '501', '1.5', '1e2', ' 2', '02', '2 ', null, 2])
    assert.throws(() => roomCapacity(invalid));
});

test('workerd capacity serializes reservations and preserves renewals above a lowered cap', { timeout: 60000 }, async t => {
  const bundle = await build({ stdin: { resolveDir: process.cwd(), contents: `
    export { default, V2Room, V2Directory } from './src/v2/worker.ts';
    import { V2Control as Control } from './src/v2/worker.ts';
    export class V2Control extends Control {
      constructor(ctx, env) { super(ctx, env); this.testEnv = env; }
      async fetch(request) {
        if (new URL(request.url).pathname === '/test/capacity') {
          this.testEnv.V2_MAX_ROOMS = await request.text();
          return new Response(null, { status: 204 });
        }
        return super.fetch(request);
      }
    }
  ` }, bundle: true, write: false, format: 'esm', target: 'es2022' });
  const mf = new Miniflare({ modules: true, script: bundle.outputFiles[0].text,
    compatibilityDate: '2026-05-21', bindings: { V2_MAX_ROOMS: '2' },
    durableObjects: { V2_ROOMS: { className: 'V2Room', useSQLite: true },
      V2_CONTROL: { className: 'V2Control', useSQLite: true },
      V2_DIRECTORY: { className: 'V2Directory', useSQLite: true } } });
  t.after(() => mf.dispose());
  const controls = await mf.getDurableObjectNamespace('V2_CONTROL');
  const capacity = controls.get(controls.idFromName('capacity'));
  const send = (path, body) => capacity.fetch('https://internal/' + path, { method: 'POST', body });
  const ids = Array.from({ length: 8 }, (_, i) => String(i).padStart(22, 'A'));
  const attempts = await Promise.all(ids.map(id => send('reserve', id)));
  const accepted = ids.filter((_, i) => attempts[i].status === 204);
  assert.equal(accepted.length, 2);
  assert.equal(attempts.filter(r => r.status === 409).length, 6);
  await send('test/capacity', '1');
  for (const id of accepted) assert.equal((await send('renew', id)).status, 204);
  assert.equal((await send('reserve', ids[7])).status, 409);
  assert.equal((await send('release', accepted[0])).status, 204);
  assert.equal((await send('reserve', ids[7])).status, 409);
  assert.equal((await send('release', accepted[1])).status, 204);
  assert.equal((await send('reserve', ids[7])).status, 204);
  await send('test/capacity', 'invalid');
  assert.equal((await send('reserve', ids[0])).status, 503);
  assert.equal((await send('renew', ids[7])).status, 204);
  assert.equal((await send('release', ids[7])).status, 204);
  // The public router must propagate unavailable capacity without creating a room.
  const response = await mf.dispatchFetch('https://test/v2/rooms', { method: 'POST',
    headers: { 'CF-Connecting-IP': '192.0.2.1', 'Content-Type': 'application/json' },
    body: JSON.stringify({ v: 2, nickname: 'Host', password: '',
      policy: { name: 'Capacity', visibility: 'public', viewerLimit: 4 } }) });
  assert.equal(response.status, 503);
});
