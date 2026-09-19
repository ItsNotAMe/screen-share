import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { validateClientCommand, validateServerEvent } from '../src/v2/protocol.ts';
import { RevisionTracker } from '../src/v2/revision-tracker.ts';
const fixtures = name => JSON.parse(readFileSync(new URL(`../../tests/fixtures/room-v2/${name}.json`, import.meta.url), 'utf8'));
for (const file of ['commands', 'events']) for (const fixture of fixtures(file)) test(`${file}: ${fixture.name}`, () => {
  const m = structuredClone(fixture.message);
  if (fixture.repeat) m.payload[fixture.repeat.field] = fixture.repeat.text.repeat(fixture.repeat.count);
  const bytes = fixture.hex ? Buffer.from(fixture.hex, 'hex') : Buffer.from((fixture.raw ?? JSON.stringify(m)) + ' '.repeat(fixture.padding ?? 0));
  const result = (file === 'commands' ? validateClientCommand : validateServerEvent)(bytes, fixture.scope);
  assert.equal(result.ok, fixture.ok ?? false);
  if (fixture.error) assert.equal(result.error, fixture.error);
  if (fixture.nickname) assert.equal(result.message.payload.nickname, fixture.nickname);
});
test('subscription lifecycle and one resync per gap', () => {
  const tracker = new RevisionTracker();
  for (const [event, generation, revision, decision, expected] of fixtures('revisions')) {
    if (event === 'start') assert.equal(tracker.start(), generation);
    else if (event === 'stop') tracker.stop();
    else assert.equal(tracker[event](generation, revision), decision);
    assert.equal(tracker.revision, event === 'start' || event === 'stop' ? null : expected);
  }
  const independent = new RevisionTracker();
  assert.equal(independent.start(), 1);
  assert.equal(independent.snapshot(1, 0), 'apply');
  assert.equal(independent.delta(1, 1), 'apply');
  assert.equal(tracker.revision, Number.MAX_SAFE_INTEGER);
});
