// Explicit loopback-only fixture. Production Worker HTTPS enforcement is intact;
// this test adapter supplies the edge HTTPS URL/IP normally supplied by Cloudflare.
import { build } from 'esbuild';
import { Miniflare } from 'miniflare';
import { spawn } from 'node:child_process';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { appendFileSync } from 'node:fs';
import { createHash, randomUUID } from 'node:crypto';
import { dirname, resolve, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const workerRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const executable = resolve(process.argv[2] ?? '');
const fault = process.argv[5] ?? '';
const impairment = process.argv[4] === 'network-impairment';
if (impairment ? !['collapse', 'loss2', 'loss5', 'reorder', 'duplicate', 'processes'].includes(fault) :
    fault && !['mutation-ack-delay', 'controllers', 'desktop-input', 'native-resolution', 'delayed-viewer'].includes(fault)) throw new Error('Unknown native service scenario');
if (!process.argv[2] || !process.argv[3]) throw new Error('Usage: node run-native-service.mjs <RoomServiceTests.exe> <artifact-root>');
const artifact = join(resolve(process.argv[3]), 'native-service-' + randomUUID());
await mkdir(artifact, { recursive: true });
let mf, child, timer;
const report = { schema: 1, passed: false, timedOut: false, executableSha256: createHash('sha256').update(await readFile(executable)).digest('hex'),
  limitations: ['Loopback plaintext test adapter; remote TLS is not exercised', process.argv[4] === 'windows-media' ? 'Generated-window WGC capture and synthetic audio; no physical input' : process.argv[4] === 'media' || impairment ? 'Synthetic capture/audio; no physical devices or input' : 'Signaling payloads are synthetic; no media or physical input', 'No hibernation or NAT acceptance'], elapsedMs: 0 };
const started = Date.now();
report.fault = fault || null;
let log = '';
let stdout = '';
try {
  const bundle = await build({ stdin: { resolveDir: workerRoot, contents: `
    import worker from './src/v2/worker.ts';
    import { V2Room as BaseRoom } from './src/v2/worker.ts';
    export { V2Control, V2Directory } from './src/v2/worker.ts';
    ${fault === 'mutation-ack-delay' ? `
    // Test-only override: persist normally and push all state/media immediately,
    // but deliver the first acknowledgement after the client's real 10s deadline.
    export class V2Room extends BaseRoom {
      constructor(ctx, env) { super(ctx, env); this.testContext = ctx; }
      send(ws, value) {
        const delay = value.type === 'command.result' ?
          (value.requestId === 'mutation_1' ? 12000 : value.requestId === 'mutation_2' ? 4000 : 0) : 0;
        if (!delay) return super.send(ws, value);
        this.testContext.waitUntil(new Promise(resolve => setTimeout(() => { super.send(ws, value); resolve(); }, delay)));
      }
    }` : 'export { BaseRoom as V2Room };'}
    export default { fetch(request, env, ctx) {
      const url = new URL(request.url);
      if (url.hostname !== '127.0.0.1') return new Response(null, { status: 403 });
      url.protocol = 'https:';
      const headers = new Headers(request.headers); headers.set('CF-Connecting-IP', '127.0.0.1');
      return worker.fetch(new Request(url, { method: request.method, headers, body: request.body }), env, ctx);
    } };
  ` }, bundle: true, write: false, format: 'esm', target: 'es2022' });
  report.workerBundleSha256 = createHash('sha256').update(bundle.outputFiles[0].text).digest('hex');
  mf = new Miniflare({ modules: true, script: bundle.outputFiles[0].text, host: '127.0.0.1', port: 0, compatibilityDate: '2026-05-21', durableObjects: {
    V2_ROOMS: { className: 'V2Room', useSQLite: true }, V2_CONTROL: { className: 'V2Control', useSQLite: true }, V2_DIRECTORY: { className: 'V2Directory', useSQLite: true } } });
  const origin = (await mf.ready).origin;
  const extra = process.argv.slice(6);
  if (extra.some(value => !impairment || fault === 'processes' || !['--fast-audio-experiment', '--event-logs'].includes(value)) || new Set(extra).size !== extra.length)
    throw new Error('Unknown or duplicate impairment option');
  const nativeExtra = extra.flatMap(value => value === '--event-logs' ? [value, join(artifact, 'rtc-events')] : [value]);
  child = spawn(executable, [origin, ...(fault ? [fault] : []), ...nativeExtra], { windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
  child.stdout.on('data', chunk => { stdout += chunk.toString(); });
  for (const stream of [child.stdout, child.stderr]) stream.on('data', chunk => {
    log += chunk.toString();
    if (impairment) appendFileSync(join(artifact, 'progress.log'), chunk);
    if (log.length > 1024 * 1024) { report.logLimitExceeded = true; child.kill(); }
  });
  timer = setTimeout(() => { report.timedOut = true; child.kill(); }, impairment ? 90000 : 60000);
  report.exitCode = await new Promise((resolve, reject) => { child.on('error', reject); child.on('close', resolve); });
  clearTimeout(timer);
  report.passed = report.exitCode === 0 && !report.timedOut && !report.logLimitExceeded;
  if (report.passed) report.metrics = JSON.parse(impairment ? stdout.trim() : log.trim());
} catch (error) { report.passed = false; report.error = error.message; }
finally {
  clearTimeout(timer);
  if (child && child.exitCode === null) child.kill();
  if (mf) await mf.dispose();
  report.elapsedMs = Date.now() - started;
  await writeFile(join(artifact, 'native.log'), log);
  await writeFile(join(artifact, 'result.json'), JSON.stringify(report, null, 2));
}
console.log(JSON.stringify({ passed: report.passed, artifact, elapsedMs: report.elapsedMs }));
if (!report.passed) { console.error(log || report.error); process.exitCode = 1; }
