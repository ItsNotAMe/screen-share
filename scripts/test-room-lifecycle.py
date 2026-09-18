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


def evaluate(detail, cycles, seconds, identity):
    if detail.get('passed') is not True or detail.get('executableSha256') != identity or \
       detail.get('cycles') != cycles or detail.get('soakSeconds') != seconds or \
       detail.get('completed') != list(range(1, cycles + 1)):
        raise ValueError('Incomplete, mismatched or failed native evidence')
    samples = detail.get('samples', [])
    if not isinstance(samples, list) or any(not isinstance(s, dict) or type(s.get('cycle')) is not int for s in samples):
        raise ValueError('Malformed resource sample')
    finished = [s for s in samples if s.get('cycle', -1) >= 0]
    if [s.get('cycle') for s in finished] != list(range(cycles + 1)):
        raise ValueError('Missing or reordered resource samples')
    resource = trends(finished)
    if cycles >= 20 and ('handles' not in resource or resource['handles']['growth'] > 8):
        raise ValueError('Room restart handle growth exceeds the unchanged +8 bound or is unknown')
    active = [s for s in samples if s.get('cycle') == -1]
    if seconds >= 10 and (len(active) < seconds // 5 - 1 or len(detail.get('progress', [])) != len(active) or
                          type(detail.get('elapsedSeconds')) not in (int, float) or
                          not math.isfinite(detail['elapsedSeconds']) or detail['elapsedSeconds'] < seconds):
        raise ValueError('Missing continuous soak samples')
    return {'restartResources': resource, 'activeResources': trends(active),
            'handleBoundApplied': cycles >= 20, 'soakAcceptanceComplete': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_directory', type=Path)
    parser.add_argument('output_directory', type=Path)
    parser.add_argument('--cycles', type=int, default=100)
    parser.add_argument('--soak-seconds', type=int, default=0, help='1..7200; requires --cycles 1')
    args = parser.parse_args()
    if not 1 <= args.cycles <= 100 or not 0 <= args.soak_seconds <= 7200 or (args.soak_seconds and args.cycles != 1):
        parser.error('Use 1..100 restart cycles, or --cycles 1 --soak-seconds 1..7200')
    executable = args.build_directory.resolve() / 'RoomLifecycleStress.exe'
    if not executable.is_file() or not shutil.which('node'):
        parser.error('Build RoomLifecycleStress and install Node.js first')
    output = args.output_directory.resolve()
    output.mkdir(parents=True, exist_ok=False)
    fixture = ROOT / 'signaling-worker/tests/run-room-lifecycle.mjs'
    report = {'schema': 1, 'passed': False, 'cycles': args.cycles, 'soakSeconds': args.soak_seconds,
              'executableSha256': runner.sha256(executable), 'runnerSha256': runner.sha256(Path(__file__)),
              'fixtureSha256': runner.sha256(fixture),
              'limitations': ['Synthetic capture/audio and recording input; no physical output or injection',
                'Fresh service fixture per restart; native process remains alive across all cycles',
                'No private-memory, full queue-age, impaired-network or external latency acceptance inferred']}
    try:
        command = [shutil.which('node'), fixture, executable, output / 'native', str(args.cycles), str(args.soak_seconds)]
        report['process'] = runner.run_process(command, output / 'runner.log', dict(os.environ),
            timeout=args.soak_seconds + args.cycles * 30 + 120, max_log_bytes=4 * 1024 * 1024)
        detail = json.loads((output / 'native/result.json').read_text(encoding='utf-8'))
        report['observations'] = detail
        report.update(evaluate(detail, args.cycles, args.soak_seconds, report['executableSha256']))
        report['passed'] = report['process']['passed']
    except Exception as error:
        report['error'] = str(error)
    finally:
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(json.dumps({'passed': report['passed'], 'output': str(output), 'error': report.get('error')}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
