// One native process, fresh production Worker fixture per completed room cycle.
// This isolates resource lifetime from per-IP admission budgets without changing
// production quotas or using a privileged room-reset endpoint.
import { build } from 'esbuild';
import { Miniflare } from 'miniflare';
import { spawn } from 'node:child_process';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { appendFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { dirname, resolve, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const [program, destination, count = '100', duration = '0', idle = '0', slow = '0', accounting = '0'] = process.argv.slice(2);
const cycles = Number(count), seconds = Number(duration), idleSeconds = Number(idle), slowViewer = slow === '1';
const memoryAccounting = accounting === '1';
if (!program || !destination || !Number.isInteger(cycles) || cycles < 1 || cycles > 100 ||
    !Number.isInteger(seconds) || seconds < 0 || seconds > 7200 || (cycles !== 1 && seconds !== 0) ||
    !Number.isInteger(idleSeconds) || idleSeconds < 0 || idleSeconds > 600 || !['0', '1'].includes(slow) ||
    (slowViewer && (cycles !== 1 || seconds < 60)) || !['0', '1'].includes(accounting))
  throw new Error('Usage: run-room-lifecycle.mjs EXE NEW_OUTPUT CYCLES_1..100 SOAK_SECONDS_0..7200 [IDLE_SECONDS_0..600 SLOW_VIEWER_0_OR_1 MEMORY_ACCOUNTING_0_OR_1] (slow viewer requires >=60s soak)');
const executable = resolve(program), output = resolve(destination);
await mkdir(output, { recursive: false });
const hash = value => createHash('sha256').update(value).digest('hex');
const report = { schema: 2, passed: false, cycles, soakSeconds: seconds, idleSeconds, slowViewer,
  completed: [], progress: [], ownership: [], idleProgress: [], idleSamples: [], memoryAccounting, memorySamples: [],
  executableSha256: hash(await readFile(executable)), samples: [],
  limitations: ['Same-process synthetic media with four viewers; no physical devices/input/playback',
    'Fresh loopback Worker per cycle; production quotas unchanged, no sustained service-cost/TLS/NAT proof',
    'Capture handoff age is not end-to-end latency or complete queue-age coverage'] };
let mf, child, watchdog, deadline, log = '', stderr = '', pending = '', chain = Promise.resolve();
const started = Date.now();
const fail = error => { report.error ??= String(error.message ?? error); child?.kill(); };
try {
  const bundle = await build({ stdin: { resolveDir: root, contents: `
    import worker from './src/v2/worker.ts';
    export { V2Room, V2Control, V2Directory } from './src/v2/worker.ts';
    export default { fetch(request, env, ctx) {
      const url = new URL(request.url);
      if (url.hostname !== '127.0.0.1') return new Response(null, {status:403});
      url.protocol = 'https:';
      const headers = new Headers(request.headers); headers.set('CF-Connecting-IP','127.0.0.1');
      return worker.fetch(new Request(url,{method:request.method,headers,body:request.body}),env,ctx);
    }};` }, bundle: true, write: false, format: 'esm', target: 'es2022' });
  const script = bundle.outputFiles[0].text; report.workerBundleSha256 = hash(script);
  async function fresh() {
    if (mf) await mf.dispose();
    mf = new Miniflare({ modules: true, script, host: '127.0.0.1', port: 0,
      compatibilityDate: '2026-05-21', durableObjects: {
        V2_ROOMS: {className:'V2Room',useSQLite:true}, V2_CONTROL: {className:'V2Control',useSQLite:true},
        V2_DIRECTORY: {className:'V2Directory',useSQLite:true} } });
    return (await mf.ready).origin;
  }
  const origin = await fresh();
  child = spawn(executable, [String(cycles), String(seconds), String(idleSeconds), slow, accounting], {windowsHide:true, stdio:['pipe','pipe','pipe']});
  child.stdin.on('error', fail);
  const touch = () => { clearTimeout(watchdog); watchdog = setTimeout(() => fail('Native lifecycle progress timed out'), 60000); };
  touch(); deadline = setTimeout(() => fail('Total lifecycle deadline exceeded'), (seconds + idleSeconds + cycles * 30 + 90) * 1000);
  child.stdin.write(origin + '\n');
  function append(chunk) {
    if (log.length + chunk.length > 4 * 1024 * 1024) { fail('Native log limit exceeded'); return false; }
    try { appendFileSync(join(output, 'native.log'), chunk); }
    catch (error) { fail(error); return false; }
    log += chunk; return true;
  }
  child.stderr.on('data', chunk => {
    const text = chunk.toString(); if (!append(text)) return;
    stderr += text;
    try { appendFileSync(join(output, 'resources.log'), text); } catch (error) { fail(error); }
  });
  child.stdout.on('data', chunk => {
    const text = chunk.toString(); if (!append(text)) return;
    pending += text;
    let newline;
    while ((newline = pending.indexOf('\n')) >= 0) {
      const line = pending.slice(0, newline); pending = pending.slice(newline + 1);
      chain = chain.then(async () => {
        const value = JSON.parse(line);
        if (value.type === 'cycle') {
          if (value.cycle !== report.completed.length + 1 || value.cycle > cycles || value.ownershipReleased !== true ||
              !Number.isSafeInteger(value.peakCaptureResources) || value.peakCaptureResources < 1 || value.peakCaptureResources > 10)
            throw new Error('Invalid cycle sequence or retained dependency');
          report.completed.push(value.cycle); report.ownership.push(value); touch();
          if (value.cycle < cycles) child.stdin.write(await fresh() + '\n');
        } else if (value.type === 'progress') {
          if (value.cycle !== report.completed.length + 1 || !Number.isSafeInteger(value.frames) || value.frames <= 0)
            throw new Error('Invalid progress sample');
          report.progress.push(value); touch();
        } else if (value.type === 'idle') {
          if (!idleSeconds || report.completed.length !== cycles || value.second !== report.idleProgress.length || value.second > idleSeconds)
            throw new Error('Invalid post-stop observation');
          report.idleProgress.push(value.second); touch();
        } else if (value.type === 'complete') {
          if (report.complete || value.passed !== true || value.cycles !== cycles || value.soakSeconds !== seconds ||
              value.idleSeconds !== idleSeconds || value.slowViewer !== slowViewer || value.memoryAccounting !== memoryAccounting)
            throw new Error('Invalid completion');
          report.complete = value;
        } else throw new Error('Unknown native record');
      }).catch(fail);
    }
  });
  report.exitCode = await new Promise((resolve, reject) => { child.on('error', reject); child.on('close', resolve); });
  await chain;
  for (const line of stderr.split(/\r?\n/)) {
    if (line.startsWith('LIFECYCLE ')) report.samples.push(JSON.parse(line.slice(10)));
    if (line.startsWith('ROOM_IDLE ')) report.idleSamples.push(JSON.parse(line.slice(10)));
    if (line.startsWith('MEMORY_ACCOUNTING ')) report.memorySamples.push(JSON.parse(line.slice(18)));
  }
  const finished = report.samples.filter(s => s.cycle >= 0);
  const validSamples = finished.length === cycles + 1 && finished.every((s, i) => s.cycle === i &&
    Number.isSafeInteger(s.handles) && s.handles >= 0 && Number.isSafeInteger(s.privateBytes) && s.privateBytes >= 0);
  const validIdle = report.idleSamples.length === (idleSeconds ? idleSeconds + 1 : 0) &&
    report.idleSamples.length === report.idleProgress.length && report.idleSamples.every((s, i) => s.cycle === i);
  report.passed = !report.error && report.exitCode === 0 && !pending.trim() && report.completed.length === cycles && !!report.complete && validSamples && validIdle;
} catch (error) { fail(error); }
finally {
  clearTimeout(watchdog); clearTimeout(deadline);
  if (child && child.exitCode === null) child.kill();
  if (mf) await mf.dispose();
  report.elapsedSeconds = (Date.now() - started) / 1000;
  await writeFile(join(output, 'native.log'), log);
  await writeFile(join(output, 'resources.log'), stderr);
  await writeFile(join(output, 'result.json'), JSON.stringify(report, null, 2));
}
console.log(JSON.stringify({passed:report.passed,completed:report.completed.length,output,error:report.error}));
if (!report.passed) process.exitCode = 1;
