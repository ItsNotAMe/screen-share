import { test } from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import { Miniflare } from 'miniflare';

test('viewer departure and retired ICE do not disconnect the host or other viewers', { timeout: 30000 }, async t => {
  const bundle = await build({ entryPoints: ['src/v2/worker.ts'], bundle: true, write: false, format: 'esm', target: 'es2022' });
  const mf = new Miniflare({ modules: true, script: bundle.outputFiles[0].text, compatibilityDate: '2026-05-21', durableObjects: {
    V2_ROOMS: { className: 'V2Room', useSQLite: true }, V2_CONTROL: { className: 'V2Control', useSQLite: true },
    V2_DIRECTORY: { className: 'V2Directory', useSQLite: true } } });
  t.after(() => mf.dispose());
  const request = (path, body, headers = {}) => mf.dispatchFetch('https://test' + path, {
    method: body === undefined ? 'GET' : 'POST', headers: { 'CF-Connecting-IP': '192.0.2.20', 'Content-Type': 'application/json', ...headers },
    body: body === undefined ? undefined : JSON.stringify(body) });
  const created = await request('/v2/rooms', { v: 2, nickname: 'Host', password: '', policy: { name: 'Departure', visibility: 'public', viewerLimit: 2 } });
  assert.equal(created.status, 201);
  const host = await created.json();
  const path = `/v2/rooms/${host.roomId}`;
  async function attach(member) {
    const response = await request(path + '/events', undefined, { Upgrade: 'websocket', Authorization: 'Bearer ' + member.token });
    assert.equal(response.status, 101);
    const socket = { member, ws: response.webSocket, messages: [], closed: false };
    socket.ws.addEventListener('message', event => socket.messages.push(JSON.parse(event.data)));
    socket.ws.addEventListener('close', () => { socket.closed = true; });
    socket.ws.accept();
    t.after(() => { try { socket.ws.close(); } catch {} });
    await until(() => socket.messages.length > 0);
    return socket;
  }
  async function join() {
    const response = await request(path + '/join', { v: 2, nickname: 'Viewer', password: '' });
    assert.equal(response.status, 200);
    return attach(await response.json());
  }
  function send(socket, type, fields = {}) {
    socket.ws.send(JSON.stringify({ v: 2, type, roomId: host.roomId, payload: {}, ...fields }));
  }
  async function resync(socket) {
    const start = socket.messages.length;
    send(socket, 'state.resync');
    await until(() => socket.messages.slice(start).some(message => message.type === 'state.snapshot'));
    assert.equal(socket.closed, false);
    return socket.messages.at(-1);
  }
  let sequence = 0;
  async function command(socket, type, payload) {
    const requestId = 'departure-' + ++sequence;
    send(socket, type, { requestId, payload });
    await until(() => socket.messages.some(message => message.requestId === requestId));
    assert.equal(socket.messages.find(message => message.requestId === requestId).payload.status, 'ok');
  }
  function candidate(socket, target, connectionId, marker) {
    send(socket, 'signal.candidate', { toPeerId: target.member.peerId, connectionId,
      payload: { candidate: marker, sdpMid: '0', sdpMLineIndex: 0 } });
  }
  async function offer(source, destination, connectionId) {
    send(source, 'signal.offer', { toPeerId: destination.member.peerId, connectionId, payload: { sdp: 'v=0\r\n' } });
    await until(() => destination.messages.some(message => message.type === 'signal.offer' && message.connectionId === connectionId));
    send(destination, 'signal.answer', { toPeerId: source.member.peerId, connectionId, payload: { sdp: 'v=0\r\n' } });
    await until(() => source.messages.some(message => message.type === 'signal.answer' && message.connectionId === connectionId));
  }
  const hosting = await attach(host);
  const survivor = await join();
  await offer(hosting, survivor, 'survivor');
  for (const mode of ['disconnect', 'leave', 'kick']) {
    let departing = await join();
    await offer(hosting, departing, mode);
    if (mode === 'disconnect') departing.ws.close(1000, 'test_departure');
    else if (mode === 'leave') await command(departing, 'peer.leave', {});
    else await command(hosting, 'peer.disconnect', { peerId: departing.member.peerId });
    await until(() => hosting.messages.some(message => message.type === 'state.delta' &&
      (message.payload.peerId === departing.member.peerId || message.payload.member?.peerId === departing.member.peerId && message.payload.member.status === 'reconnecting')));
    const start = survivor.messages.length;
    candidate(hosting, departing, mode, 'late-' + mode);
    send(hosting, 'signal.offer', { toPeerId: departing.member.peerId, connectionId: 'late-offer-' + mode, payload: { sdp: 'v=0\r\n' } });
    assert.equal((await resync(hosting)).payload.status, 'open');
    candidate(hosting, survivor, 'survivor', 'healthy-' + mode);
    await until(() => survivor.messages.slice(start).some(message => message.payload?.candidate === 'healthy-' + mode));
    assert.equal(survivor.messages.slice(start).some(message => message.payload?.op === 'host.status'), false);
    assert.equal(hosting.closed || survivor.closed, false);
    if (mode === 'disconnect') {
      departing = await attach(departing.member);
      // Candidates queued before reattachment must not retire the host socket.
      candidate(hosting, departing, mode, 'retired-generation');
      await resync(hosting);
      assert.equal(departing.messages.some(message => message.payload?.candidate === 'retired-generation'), false);
      await offer(hosting, departing, 'recovered');
      // Late candidates from the previous offer are harmless in both directions.
      candidate(hosting, departing, mode, 'retired-host');
      candidate(departing, hosting, mode, 'retired-viewer');
      await resync(hosting); await resync(departing);
      assert.equal(hosting.messages.some(message => message.payload?.candidate === 'retired-viewer'), false);
      assert.equal(departing.messages.some(message => message.payload?.candidate === 'retired-host'), false);
      await command(departing, 'peer.leave', {});
    }
  }
  const newcomer = await join();
  await offer(hosting, newcomer, 'newcomer');
  assert.equal((await resync(hosting)).payload.members.length, 3);
});

async function until(predicate) {
  const end = Date.now() + 5000;
  while (!predicate()) { if (Date.now() > end) throw new Error('event deadline exceeded'); await new Promise(resolve => setTimeout(resolve, 10)); }
}
