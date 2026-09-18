import { test } from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';

test('capacity alarms coalesce while admission budget cleanup follows the last request', async t => {
  t.mock.timers.enable({ apis: ['Date'], now: 1800000010000 });
  const bundle = await build({ stdin: { resolveDir: process.cwd(), contents:
    "export { V2Control } from './src/v2/worker.ts';" }, bundle: true, write: false, format: 'esm' });
  const { V2Control } = await import('data:text/javascript;base64,' + Buffer.from(bundle.outputFiles[0].text).toString('base64'));
  const data = new Map(); let alarm = null, alarmWrites = 0;
  const storage = {
    async get(key) { return structuredClone(data.get(key)); },
    async put(key, value) { data.set(key, structuredClone(value)); },
    async delete(key) { data.delete(key); },
    async getAlarm() { return alarm; },
    async setAlarm(value) { alarm = value; ++alarmWrites; }
  };
  const control = new V2Control({ storage, blockConcurrencyWhile: action => action() });
  const send = (path, body) => control.fetch(new Request('https://internal/' + path, { method: 'POST', body }));
  const first = 'A'.repeat(22), second = 'B'.repeat(22);
  assert.equal((await send('reserve', first)).status, 204);
  const original = alarm;
  t.mock.timers.setTime(Date.now() + 1000);
  assert.equal((await send('reserve', second)).status, 204);
  assert.equal(alarmWrites, 1); assert.equal(alarm, original);
  t.mock.timers.setTime(original + 1);
  assert.equal((await send('renew', first)).status, 204);
  assert.equal(alarmWrites, 2); assert.equal(alarm, Date.now() + 60000);
  data.clear(); alarm = null;
  await send('limit', 'create');
  const firstBudgetAlarm = alarm;
  t.mock.timers.setTime(Date.now() + 119000);
  for (let i = 0; i < 10; ++i) assert.equal((await send('limit', 'create')).status, 204);
  assert.equal((await send('limit', 'create')).status, 429);
  assert.ok(alarm > firstBudgetAlarm);
  assert.equal(alarm, Date.now() + 120000);
  t.mock.timers.setTime(alarm);
  await control.alarm();
  assert.equal(data.has('budget'), false);
});
