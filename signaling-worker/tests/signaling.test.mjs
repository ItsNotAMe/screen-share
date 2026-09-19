import { test } from 'node:test';
import assert from 'node:assert/strict';
import { authorizeSignal } from '../src/v2/signaling.ts';

test('signaling permissions, socket generations, recovery and candidate bounds', () => {
  const host = { peerId: 'host', role: 'host', generation: 1 };
  const viewer = { peerId: 'viewer', role: 'viewer', generation: 1 };
  const message = (type, connectionId = 'first') => ({ type: 'signal.' + type, connectionId });
  const apply = (type, sender, target, state, id = 'first', now = 0) => authorizeSignal(message(type, id), sender, target, state, id, now);
  assert.equal(apply('offer', viewer, host).ok, false);
  assert.equal(apply('offer', host, host).ok, false);
  let state = apply('offer', host, viewer).session;
  assert.equal(apply('candidate', viewer, host, state).ok, false);
  assert.equal(apply('answer', host, viewer, state).ok, false);
  assert.equal(apply('answer', viewer, { ...host, generation: 2 }, state).ok, false);
  state = apply('answer', viewer, host, state).session;
  assert.equal(apply('answer', viewer, host, state).ok, false);
  assert.equal(apply('restart_request', host, viewer, state).ok, false);
  assert.equal(apply('restart_request', viewer, host, state).ok, true);
  for (let i = 0; i < 64; ++i) { const result = apply('candidate', viewer, host, state); assert.equal(result.ok, true); state = result.session; }
  assert.equal(apply('candidate', viewer, host, state).reason, 'candidate_limit');
  assert.equal(apply('candidate', host, viewer, state).ok, true);
  for (let i = 1; i <= 3; ++i) state = apply('offer', host, viewer, state, 'restart' + i).session;
  assert.equal(apply('offer', host, viewer, state, 'too-fast').reason, 'rate_limited');
  assert.equal(apply('offer', host, viewer, state, 'first', 60001).reason, 'stale_connection');
  const restart = apply('offer', host, viewer, state, 'later', 60001);
  assert.equal(restart.ok, true);
  assert.equal(apply('candidate', viewer, host, restart.session, 'restart3').reason, 'stale_connection');
  assert.equal(apply('offer', host, viewer, { ...state, used: Array(1024).fill('old') }, 'limit', 60001).reason, 'rate_limited');
});
