import { test } from 'node:test';
import assert from 'node:assert/strict';
import { BackgroundOutbox, SerializedState } from '../src/v2/background-outbox.ts';

test('outbox coalesces wakeups, bounds each drain and leaves failed work retryable', async () => {
  const tasks = [];
  let pending = 8, attempts = 0, active = 0, maximum = 0;
  let release;
  const held = new Promise(resolve => { release = resolve; });
  const outbox = new BackgroundOutbox(work => tasks.push(work), async () => pending ? pending : undefined, async () => {
    ++active; maximum = Math.max(maximum, active); ++attempts;
    await held; --pending; --active; return true;
  });
  outbox.request(); outbox.start();
  await Promise.resolve();
  for (let i = 0; i < 100; ++i) { outbox.request(); outbox.start(); }
  assert.equal(tasks.length, 1);
  release(); await tasks[0];
  assert.equal(maximum, 1); assert.equal(attempts, 4); assert.equal(pending, 4);
  outbox.request(); outbox.start(); await tasks[1];
  assert.equal(pending, 0);
  let failedAttempts = 0;
  const failed = new BackgroundOutbox(work => tasks.push(work), async () => 'pending', async () => { ++failedAttempts; throw new Error('network'); });
  failed.request(); failed.start(); await tasks[2];
  assert.equal(failedAttempts, 1);
  failed.start(); assert.equal(tasks.length, 3); // no uncontrolled retry
  failed.request(); failed.start(); await tasks[3]; assert.equal(failedAttempts, 2);
});

test('state serialization preserves updates across awaits and recovers after rejection', async () => {
  const lane = new SerializedState();
  let value = 0;
  const work = Array.from({ length: 64 }, () => lane.run(async () => { const before = value; await Promise.resolve(); value = before + 1; }));
  await Promise.all(work);
  assert.equal(value, 64);
  await assert.rejects(lane.run(async () => { throw new Error('injected'); }));
  await lane.run(async () => { ++value; });
  assert.equal(value, 65);
});
