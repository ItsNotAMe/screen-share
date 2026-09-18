"""Silent same-process room restarts or a continuous four-viewer soak, with durable resource evidence."""
import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('room_runner', ROOT / 'scripts/test-room-regression.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
spec = importlib.util.spec_from_file_location('handoff_evidence', ROOT / 'scripts/frame_handoff_evidence.py')
handoff = importlib.util.module_from_spec(spec)
spec.loader.exec_module(handoff)


def trends(samples):
    result = {}
    if len(samples) < 21:
        return result
    for key in ('handles', 'privateBytes', 'workingSetBytes', 'gdiObjects', 'userObjects'):
        values = [s.get(key) for s in samples]
        if all(type(v) is int and v >= 0 for v in values):
            baseline, final = statistics.median(values[6:11]), statistics.median(values[-5:])
            result[key] = {'baselineMedian': baseline, 'finalMedian': final, 'growth': final - baseline,
                           'peak': max(values), 'final': values[-1]}
    return result


def finite(value):
    return type(value) in (int, float) and math.isfinite(value)


def resource_samples(samples):
    if not isinstance(samples, list):
        raise ValueError('Malformed resource samples')
    for sample in samples:
        if not isinstance(sample, dict) or type(sample.get('cycle')) is not int or \
           not finite(sample.get('seconds')) or sample['seconds'] < 0:
            raise ValueError('Malformed resource sample')
        for key in ('handles', 'privateBytes', 'workingSetBytes', 'gdiObjects', 'userObjects'):
            if type(sample.get(key)) is not int or sample[key] < 0:
                raise ValueError('Missing resource measurement')


def progress_samples(progress, active, seconds, slow_viewer):
    if not isinstance(progress, list) or len(progress) != len(active):
        raise ValueError('Mismatched continuous resource/progress samples')
    previous = None
    phases = {'healthy': [], 'slow': [], 'recovered': []}
    for entry, resource in zip(progress, active):
        if not isinstance(entry, dict) or entry.get('cycle') != 1 or \
           not finite(entry.get('elapsedSeconds')) or not finite(entry.get('intervalSeconds')) or \
           not 5 <= entry['intervalSeconds'] <= 10 or \
           abs(entry['elapsedSeconds'] - (previous['elapsedSeconds'] if previous else 0) - entry['intervalSeconds']) > .001 or \
           abs(resource['seconds'] - entry['intervalSeconds']) > .001:
            raise ValueError('Invalid continuous timing')
        if entry.get('phase') not in (*phases, 'transition') or (not slow_viewer and entry['phase'] != 'healthy'):
            raise ValueError('Unexpected impairment phase')
        def phase_at(elapsed):
            return 'healthy' if not slow_viewer or elapsed < 15 else 'slow' if elapsed < 35 else 'recovered'
        phase = phase_at(entry['elapsedSeconds'])
        expected = phase if phase == phase_at(previous['elapsedSeconds'] if previous else 0) else 'transition'
        if entry['phase'] != expected:
            raise ValueError('Impairment phase does not match elapsed time')
        if type(entry.get('peakCaptureResources')) is not int or not 1 <= entry['peakCaptureResources'] <= 10:
            raise ValueError('Capture resource budget exceeded or unknown')
        if type(entry.get('maxCaptureHandoffUs')) is not int or entry['maxCaptureHandoffUs'] < 0:
            raise ValueError('Missing capture handoff observation')
        viewers = entry.get('viewers')
        if not isinstance(viewers, list) or len(viewers) != 4:
            raise ValueError('Missing viewer progress')
        for index, viewer in enumerate(viewers):
            if not isinstance(viewer, dict) or type(viewer.get('viewer')) is not int or viewer['viewer'] != index:
                raise ValueError('Invalid viewer order')
            for total, delta in (('frames', 'frameDelta'), ('audioBlocks', 'audioDelta')):
                if type(viewer.get(total)) is not int or type(viewer.get(delta)) is not int or \
                   not 0 < viewer[delta] <= viewer[total] or \
                   (previous and viewer[total] - previous['viewers'][index][total] != viewer[delta]):
                    raise ValueError('Stalled viewer or inconsistent counters')
            if any(type(viewer.get(key)) is not int or viewer[key] < 0 for key in ('received', 'replaced', 'pending')) or \
               viewer['pending'] > 1 or viewer['received'] != viewer['frames'] + viewer['replaced'] + viewer['pending']:
                raise ValueError('Presentation queue is unbounded or unaccounted')
            if previous and (viewer['received'] < previous['viewers'][index]['received'] or
                             viewer['replaced'] < previous['viewers'][index]['replaced']):
                raise ValueError('Presentation counters moved backwards')
            receiver = viewer.get('receiver')
            if not isinstance(receiver, dict) or type(receiver.get('jitterBufferMeanMs')) is not int or \
               not 0 <= receiver['jitterBufferMeanMs'] <= 100:
                raise ValueError('Loopback receiver buffering exceeds 100ms or is unmeasured')
            if viewer['frameDelta'] / entry['intervalSeconds'] > 36:
                raise ValueError('Presentation catch-up burst exceeds the fixed 30fps source allowance')
            if slow_viewer and entry['phase'] == 'slow' and index == 0 and previous and \
               viewer['replaced'] <= previous['viewers'][index]['replaced']:
                raise ValueError('Slow presentation did not replace pending frames')
        if entry.get('frames') != sum(v['frames'] for v in viewers):
            raise ValueError('Aggregate progress does not match the viewers')
        if entry['phase'] in phases:
            phases[entry['phase']].append([v['frameDelta'] / entry['intervalSeconds'] for v in viewers])
        previous = entry
    # Each interval is validated against both monotonic elapsed time and its
    # matching resource sample above. Small scheduling delays accumulate over a
    # long run; dividing duration by an ideal five seconds rejects intact logs.
    if seconds >= 10 and (not previous or not seconds - 6 <= previous['elapsedSeconds'] <= seconds + 1):
        raise ValueError('Missing continuous soak samples')
    if not seconds and progress:
        raise ValueError('Unexpected continuous samples in restart run')
    isolation = None
    if slow_viewer:
        if any(len(phases[name]) < 2 for name in phases):
            raise ValueError('Incomplete slow-viewer phases')
        baseline = [statistics.mean(row[i] for row in phases['healthy']) for i in range(4)]
        slow = [statistics.mean(row[i] for row in phases['slow']) for i in range(4)]
        recovered = [statistics.mean(row[i] for row in phases['recovered'][-2:]) for i in range(4)]
        if slow[0] > baseline[0] * .5 or any(slow[i] < baseline[i] * .7 for i in range(1, 4)) or \
           any(recovered[i] < baseline[i] * .7 for i in range(4)):
            raise ValueError(f'Slow-viewer isolation or recovery failed: baselineFps={baseline}, impairedFps={slow}, recoveredFps={recovered}')
        isolation = {'baselineFps': baseline, 'impairedFps': slow, 'recoveredFps': recovered,
                     'presentationIntervalMs': 250, 'healthyMinimumRatio': .7, 'impairedMaximumRatio': .5,
                     'maximumPendingFrames': 1, 'maximumPresentationFps': 36}
    return isolation


def evaluate(detail, cycles, seconds, identity, idle_seconds=0, slow_viewer=False, memory_accounting=False, require_handoff=False):
    if detail.get('schema') != 2 or detail.get('passed') is not True or detail.get('executableSha256') != identity or \
       detail.get('cycles') != cycles or detail.get('soakSeconds') != seconds or \
       detail.get('idleSeconds') != idle_seconds or detail.get('slowViewer') is not slow_viewer or \
       detail.get('memoryAccounting') is not memory_accounting or \
       detail.get('completed') != list(range(1, cycles + 1)):
        raise ValueError('Incomplete, mismatched or failed native evidence')
    samples = detail.get('samples', [])
    resource_samples(samples)
    finished = [s for s in samples if s.get('cycle', -1) >= 0]
    if [s.get('cycle') for s in finished] != list(range(cycles + 1)):
        raise ValueError('Missing or reordered resource samples')
    resource = trends(finished)
    if cycles >= 20 and ('handles' not in resource or resource['handles']['growth'] > 8):
        raise ValueError('Room restart handle growth exceeds the unchanged +8 bound or is unknown')
    ownership = detail.get('ownership', [])
    if not isinstance(ownership, list) or len(ownership) != cycles:
        raise ValueError('Missing ownership evidence')
    for index, record in enumerate(ownership, 1):
        if not isinstance(record, dict) or record.get('cycle') != index or record.get('ownershipReleased') is not True or \
           type(record.get('peakCaptureResources')) is not int or not 1 <= record['peakCaptureResources'] <= 10 or \
           not finite(record.get('activeSeconds')) or record['activeSeconds'] < seconds:
            raise ValueError('Retained dependencies or incomplete active run')
    active = [s for s in samples if s['cycle'] < 0]
    if any(s['cycle'] != -1 for s in active):
        raise ValueError('Unexpected active cycle')
    isolation = progress_samples(detail.get('progress', []), active, seconds, slow_viewer)
    handoffs = []
    previous_handoffs = [None] * 4
    for entry in detail.get('progress', []):
        for index, viewer in enumerate(entry['viewers']):
            if require_handoff or 'handoff' in viewer:
                value = viewer.get('handoff')
                handoff.validate(value, previous_handoffs[index])
                if any(value[key] != viewer[key] for key in ('pending', 'received', 'replaced')) or value['delivered'] != viewer['frames']:
                    raise ValueError('Handoff counters differ from presentation')
                previous_handoffs[index] = value
                handoffs.append(value)
    idle = detail.get('idleSamples', [])
    resource_samples(idle)
    expected_idle = list(range(idle_seconds + 1)) if idle_seconds else []
    if [s['cycle'] for s in idle] != expected_idle or detail.get('idleProgress', []) != expected_idle or \
       any(s['seconds'] < s['cycle'] for s in idle):
        raise ValueError('Incomplete idle observation')
    if not finite(detail.get('elapsedSeconds')) or detail['elapsedSeconds'] < seconds + idle_seconds:
        raise ValueError('Invalid total elapsed time')
    memory = detail.get('memorySamples', [])
    expected_memory = []
    if memory_accounting:
        expected_memory = [('cycle', n) for n in sorted({0, cycles, *range(10, cycles + 1, 10)})]
        if idle_seconds:
            expected_memory += [('idle', n) for n in sorted({0, idle_seconds, *range(10, idle_seconds + 1, 10)})]
    if not isinstance(memory, list) or any(not isinstance(s, dict) for s in memory) or \
       [(s.get('stage'), s.get('cycle')) for s in memory] != expected_memory:
        raise ValueError('Missing or unexpected memory accounting')
    for sample in memory:
        if sample.get('complete') is not True:
            raise ValueError('Heap walk or virtual memory scan incomplete; cannot claim accounting')
        for key in ('heaps', 'heapBusyBytes', 'heapFreeBytes', 'heapOverheadBytes', 'heapBusyBlocks',
                    'heapRegionCommittedBytes', 'privateCommittedBytes', 'mappedCommittedBytes', 'imageCommittedBytes'):
            if type(sample.get(key)) is not int or sample[key] < 0:
                raise ValueError('Missing heap/virtual memory measurement')
    return {'restartResources': resource, 'activeResources': trends(active),
            'decodedFrameHandoff': handoff.summarize(handoffs),
            'restartByShutdownOrder': {'hostFirst': trends(finished[2::2]), 'viewerFirst': trends(finished[1::2])},
            'idleObservation': {'seconds': idle_seconds, 'resources': trends(idle)},
            'ownershipVerified': True, 'slowViewerIsolation': isolation,
            'receiverBuffering': {'maximumReportedMeanMs': max((v['receiver']['jitterBufferMeanMs']
                for p in detail.get('progress', []) for v in p['viewers']), default=None),
                'loopbackLimitMs': 100, 'externalLatencyVerified': False},
            'memoryAccountingVerified': memory_accounting,
            'continuousDurationVerified': seconds, 'handleBoundApplied': cycles >= 20, 'soakAcceptanceComplete': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_directory', type=Path)
    parser.add_argument('output_directory', type=Path)
    parser.add_argument('--cycles', type=int, default=100)
    parser.add_argument('--soak-seconds', type=int, default=0, help='1..7200; requires --cycles 1')
    parser.add_argument('--idle-seconds', type=int, default=0, help='0..600 post-stop observation; does not replace restart samples')
    parser.add_argument('--slow-viewer', action='store_true', help='Consume viewer 0 presentation every 250ms for 20s; requires >=60s continuous run')
    parser.add_argument('--memory-accounting', action='store_true', help='Diagnostic heap walks/virtual memory scans after shutdown only; no compaction')
    args = parser.parse_args()
    if not 1 <= args.cycles <= 100 or not 0 <= args.soak_seconds <= 7200 or (args.soak_seconds and args.cycles != 1):
        parser.error('Use 1..100 restart cycles, or --cycles 1 --soak-seconds 1..7200')
    if not 0 <= args.idle_seconds <= 600 or (args.slow_viewer and args.soak_seconds < 60):
        parser.error('Idle observation must be 0..600 seconds; slow viewer requires at least 60 continuous seconds')
    executable = args.build_directory.resolve() / 'RoomLifecycleStress.exe'
    if not executable.is_file() or not shutil.which('node'):
        parser.error('Build RoomLifecycleStress and install Node.js first')
    output = args.output_directory.resolve()
    output.mkdir(parents=True, exist_ok=False)
    fixture = ROOT / 'signaling-worker/tests/run-room-lifecycle.mjs'
    report = {'schema': 2, 'passed': False, 'cycles': args.cycles, 'soakSeconds': args.soak_seconds,
              'idleSeconds': args.idle_seconds, 'slowViewer': args.slow_viewer,
              'memoryAccounting': args.memory_accounting,
              'executableSha256': runner.sha256(executable), 'runnerSha256': runner.sha256(Path(__file__)),
              'handoffValidatorSha256': runner.sha256(ROOT / 'scripts/frame_handoff_evidence.py'),
              'fixtureSha256': runner.sha256(fixture),
              'limitations': ['Synthetic capture/audio and recording input; no physical output or injection',
                'Fresh service fixture per restart; native process remains alive across all cycles',
                'No private-memory, full queue-age, impaired-network or external latency acceptance inferred']}
    try:
        command = [shutil.which('node'), fixture, executable, output / 'native', str(args.cycles), str(args.soak_seconds),
                   str(args.idle_seconds), '1' if args.slow_viewer else '0', '1' if args.memory_accounting else '0']
        report['process'] = runner.run_process(command, output / 'runner.log', dict(os.environ),
            timeout=args.soak_seconds + args.idle_seconds + args.cycles * 30 + 120, max_log_bytes=4 * 1024 * 1024)
        detail = json.loads((output / 'native/result.json').read_text(encoding='utf-8'))
        report['observations'] = detail
        report.update(evaluate(detail, args.cycles, args.soak_seconds, report['executableSha256'],
                               args.idle_seconds, args.slow_viewer, args.memory_accounting, require_handoff=True))
        report['passed'] = report['process']['passed']
    except Exception as error:
        report['error'] = str(error)
    finally:
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(json.dumps({'passed': report['passed'], 'output': str(output), 'error': report.get('error')}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
