"""Silent, isolated real-packet network scenarios with fail-closed evidence checks."""
import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import statistics

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('room_runner', ROOT / 'scripts/test-room-regression.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
SCENARIOS = ('collapse', 'loss2', 'loss5', 'reorder', 'duplicate', 'processes')


def numeric(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def validate(report, scenario, executable_hash):
    if report.get('passed') is not True or report.get('timedOut') is not False or report.get('exitCode') != 0 or \
       report.get('logLimitExceeded', False) or report.get('executableSha256') != executable_hash:
        raise ValueError('Native process failed, timed out, or executable identity differs')
    metrics = report.get('metrics', {})
    if scenario == 'processes':
        viewers = metrics.get('viewers')
        if metrics.get('passed') is not True or metrics.get('separateProcesses') != 5 or \
           metrics.get('externalLatencyVerified') is not False or metrics.get('physicalInput') is not False or \
           not isinstance(viewers, list) or len(viewers) != 4:
            raise ValueError('Missing separate-process evidence')
        ids = set()
        for viewer in viewers:
            if viewer.get('passed') is not True or viewer.get('runtimeReleased') is not True or \
               type(viewer.get('pid')) is not int or viewer['pid'] <= 0 or viewer['pid'] in ids or \
               not numeric(viewer.get('frames')) or viewer['frames'] < 250 or \
               not numeric(viewer.get('audioBlocks')) or viewer['audioBlocks'] < 700:
                raise ValueError('Invalid separate-process receiver result')
            ids.add(viewer['pid'])
        return {'separateProcesses': 5, 'runtimeReleased': True, 'externalLatencyVerified': False}
    if metrics.get('schema') != 1 or metrics.get('scenario') != scenario or metrics.get('seed') != 12345 or \
       metrics.get('released') is not True or metrics.get('externalLatencyVerified') is not False or \
       metrics.get('inputApplied') is not True or metrics.get('inputRevoked') is not True:
        raise ValueError('Missing scenario, seed, ownership, or scope evidence')
    for key, maximum in (('peakQueued', 2048), ('peakBytes', 16 * 1024 * 1024)):
        if type(metrics.get(key)) is not int or not 0 < metrics[key] <= maximum:
            raise ValueError('Missing or excessive packet ownership')
    if type(metrics.get('maximumSchedulingDelayUs')) is not int or metrics['maximumSchedulingDelayUs'] < 0:
        raise ValueError('Missing simulator scheduling observation')
    samples = metrics.get('samples')
    if not isinstance(samples, list) or len(samples) != 36:
        raise ValueError('Missing phase samples')
    previous = None
    phases = {name: [] for name in ('baseline', 'impaired', 'recovery')}
    for index, sample in enumerate(samples):
        phase = ('baseline', 'impaired', 'recovery')[index // 12]
        if sample.get('phase') != phase or sample.get('second') != index % 12 + 1:
            raise ValueError('Unexpected phase ordering')
        impaired = phase == 'impaired'
        jittered = impaired and scenario in ('loss2', 'loss5', 'reorder')
        configuration = {'capacityBps': 4000000 if impaired and scenario == 'collapse' else 20000000,
            'delayMeanMs': 25 if jittered else 0, 'delayStddevMs': 10 if jittered else 0,
            'configuredLossPercent': (2 if scenario == 'loss2' else 5) if impaired and scenario in ('loss2', 'loss5') else 0,
            'allowReordering': impaired and scenario == 'reorder', 'duplicateEvery': 50 if impaired and scenario == 'duplicate' else 0}
        if any(sample.get(key) != value for key, value in configuration.items()):
            raise ValueError('Recorded network configuration differs from requested scenario')
        fps, peers = sample.get('fps'), sample.get('peers')
        if not isinstance(fps, list) or len(fps) != 4 or not all(numeric(n) and n <= 120 for n in fps):
            raise ValueError('Missing or invalid four-viewer frame rates')
        if not isinstance(peers, list) or len(peers) != 4:
            raise ValueError('Missing peer telemetry')
        for i, peer in enumerate(peers):
            if peer.get('viewer') != i or not all(numeric(peer.get(key)) for key in
                ('audioBlocks', 'payloadBps', 'availableOutgoingBps', 'rttMs', 'lossFraction', 'jitterBufferMeanMs')):
                raise ValueError('Missing peer observations')
            if type(peer.get('pending')) is not int or not 0 <= peer['pending'] <= 1:
                raise ValueError('Presentation queue is missing or unbounded')
            if previous and peer['audioBlocks'] <= previous['peers'][i]['audioBlocks']:
                raise ValueError('Audio stopped progressing')
        for key in ('lost', 'overflow', 'queued', 'duplicated', 'reordered'):
            if type(sample.get(key)) is not int or sample[key] < 0 or (key != 'queued' and previous and sample[key] < previous[key]):
                raise ValueError('Invalid packet accounting')
        if sample['queued'] > 2048 or not numeric(sample.get('ingressBps')):
            raise ValueError('Invalid ingress/queue measurement')
        phases[phase].append(sample)
        previous = sample
    # Ignore the initial convergence period, but never discard its raw evidence.
    baseline, recovery = phases['baseline'][-5:], phases['recovery'][-5:]
    for viewer in range(4):
        healthy_fps = statistics.mean(s['fps'][viewer] for s in baseline)
        if healthy_fps < 20 or statistics.mean(s['fps'][viewer] for s in recovery) < max(20, healthy_fps * .8):
            raise ValueError('Healthy media or recovery frame rate failed')
        if viewer and min(s['fps'][viewer] for s in phases['impaired']) < healthy_fps * .7:
            raise ValueError('Impairment disrupted a healthy viewer')
    before, after = phases['baseline'][-1], phases['impaired'][-1]
    if scenario == 'collapse':
        if statistics.mean(s['ingressBps'] for s in baseline) <= 5000000:
            raise ValueError('Insufficient offered load to exercise 4 Mbps collapse')
        if statistics.mean(s['ingressBps'] for s in phases['impaired'][-5:]) > 4400000:
            raise ValueError('Bandwidth shaping was bypassed')
        if statistics.mean(s['ingressBps'] for s in recovery) <= 4400000:
            raise ValueError('Sender failed to regain bandwidth after recovery')
    else:
        counter = 'lost' if scenario in ('loss2', 'loss5') else 'reordered' if scenario == 'reorder' else 'duplicated'
        if after[counter] <= before[counter]:
            raise ValueError('Requested impairment was not observed')
        if scenario in ('reorder', 'duplicate') and (after['lost'] != before['lost'] or after['overflow'] != before['overflow']):
            raise ValueError('Reordering/duplication scenario introduced packet loss')
    return {'healthyViewerIsolation': True, 'recovered': True,
            'baselineIngressBps': statistics.mean(s['ingressBps'] for s in baseline),
            'impairedIngressBps': statistics.mean(s['ingressBps'] for s in phases['impaired'][-5:]),
            'recoveryFps': [statistics.mean(s['fps'][i] for s in recovery) for i in range(4)],
            'externalLatencyVerified': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--scenario', choices=SCENARIOS)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    executable = args.build.resolve() / 'RoomImpairmentProof.exe'
    digest = runner.sha256(executable)
    node = shutil.which('node')
    if not node:
        raise RuntimeError('Node is required')
    report = {'schema': 1, 'passed': False, 'scenarios': {}, 'externalLatencyVerified': False,
              'runnerSha256': runner.sha256(Path(__file__)),
              'fixtureSha256': runner.sha256(ROOT / 'signaling-worker/tests/run-native-service.mjs'),
              'pinnedWebRtc': json.loads((ROOT / 'refactor/webrtc-source.json').read_text())['commit'],
              'limitations': ['640x360@30 software H264; synthetic noise and discarded audio',
                              'One viewer UDP ingress only; no real NIC/NAT or physical input',
                              'Jitter is Gaussian (25 ms mean, 10 ms standard deviation), not capped at 50 ms',
                              'Mean jitter-buffer telemetry is not capture-to-display latency']}
    try:
        for scenario in [args.scenario] if args.scenario else SCENARIOS:
            output = args.output / scenario
            output.mkdir()
            process = runner.run_process([node, ROOT / 'signaling-worker/tests/run-native-service.mjs', executable,
                output, 'network-impairment', scenario], output / 'runner.log', os.environ.copy(), timeout=110)
            report['scenarios'][scenario] = {'process': process}
            if not process['passed']:
                raise ValueError(f'{scenario}: native process failed; inspect retained logs')
            files = list(output.glob('native-service-*/result.json'))
            if len(files) != 1:
                raise ValueError('Missing or ambiguous native result')
            report['scenarios'][scenario]['validation'] = validate(json.loads(files[0].read_text()), scenario, digest)
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    finally:
        (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
