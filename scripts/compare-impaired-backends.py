"""Matched real-packet legacy/v2 comparison; silent generated content and no OS network changes."""
import argparse
import importlib.util
import json
import math
from pathlib import Path
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('comparison', ROOT / 'scripts/compare-backends.py')
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


def validate(metrics, variant, viewers, seconds, scenario):
    backend = 'legacy' if variant.startswith('legacy') else 'v2'
    if metrics.get('networkAdaptation') != ('legacy-feedback-enabled' if backend == 'legacy' else 'webrtc-congestion-control'):
        raise ValueError('Both backends must retain their existing bandwidth adaptation')
    comparison.validate(metrics, backend, 'motion', viewers, seconds, variant, 'honor-resolution', network_scenario=scenario)
    network = metrics.get('network', {})
    if network.get('scenario') != scenario or network.get('seed') != 12345 or network.get('impairedViewer') != 0 or \
            network.get('queuePackets') != 256 or network.get('invalid') is not False:
        raise ValueError('Missing or mismatched packet impairment')
    if any(type(network.get(k)) is not int for k in ['seed', 'impairedViewer', 'queuePackets']):
        raise ValueError('Invalid network configuration types')
    phases = network.get('phases', [])
    if len(phases) != 3 or [p.get('phase') for p in phases] != ['baseline', 'impaired', 'recovery']:
        raise ValueError('Missing network phases')
    for index, phase in enumerate(phases):
        impaired = index == 1
        settings = dict(capacityBps=4000000 if impaired and scenario == 'collapse' else 20000000,
                        lossPercent=5 if impaired and scenario == 'loss5' else 0,
                        delayMs=25 if impaired and scenario == 'loss5' else 0,
                        jitterMs=10 if impaired and scenario == 'loss5' else 0)
        if any(phase.get(k) != v for k, v in settings.items()) or not seconds / 3 <= phase['seconds'] < seconds / 3 + 2:
            raise ValueError('Phase settings or duration mismatch')
        for key in ['received', 'delivered', 'deliveredBytes', 'lost', 'overflow']:
            a, b = phase['before'].get(key), phase['after'].get(key)
            if type(a) is not int or type(b) is not int or not 0 <= a <= b:
                raise ValueError('Invalid network counters')
        if phase['after']['received'] - phase['before']['received'] < 100:
            raise ValueError('Media did not traverse impaired link')
        receivers = phase.get('receivers')
        if not isinstance(receivers, list) or len(receivers) != viewers:
            raise ValueError('Missing phase receiver evidence')
        for receiver in receivers:
            for key in ['frames', 'invalidMarkers', 'freshFrames', 'freshFps', 'displayedAgeSamples']:
                if not comparison.finite(receiver.get(key)):
                    raise ValueError('Missing phase frame evidence')
            if any(type(receiver[k]) is not int for k in ['frames', 'invalidMarkers', 'freshFrames', 'displayedAgeSamples']):
                raise ValueError('Noninteger phase counts')
            if receiver['freshFrames'] > receiver['frames'] or receiver['invalidMarkers'] > receiver['frames']:
                raise ValueError('Inconsistent frame counters')
            if not math.isclose(receiver['freshFps'], receiver['freshFrames'] / phase['seconds'], rel_tol=1e-6):
                raise ValueError('Inconsistent measured FPS')
            if receiver['displayedAgeSamples'] < seconds / 3 * 90:
                raise ValueError('Missing continuous held-image samples')
            for key in ['displayedAgeP95Ms', 'displayedAgeP99Ms', 'displayedAgeMaximumMs']:
                if not comparison.finite(receiver.get(key)):
                    raise ValueError('Missing held-image latency')
            if not receiver['displayedAgeP95Ms'] <= receiver['displayedAgeP99Ms'] <= receiver['displayedAgeMaximumMs']:
                raise ValueError('Inconsistent held-image quantiles')
    base, impaired, recovery = phases
    baseline_bps = 8 * (base['after']['deliveredBytes'] - base['before']['deliveredBytes']) / base['seconds']
    if scenario == 'collapse' and baseline_bps <= 5000000:
        raise ValueError('Baseline did not load the constrained link')
    if scenario == 'loss5' and impaired['after']['lost'] <= impaired['before']['lost']:
        raise ValueError('Configured packet loss was not observed')
    if backend == 'v2':
        encoder = 'mf-h264-software' if variant == 'v2-software' else 'mf-h264-hardware'
        if len(metrics.get('senders', [])) != viewers or any(p.get('encoder') != encoder for p in metrics['senders']) or metrics.get('softwareFallbacks') != 0:
            raise ValueError('Requested v2 encoder was not observed')
    return {'baselineIngressBps': baseline_bps,
            'phases': {p['phase']: p['receivers'] for p in phases},
            'mediaCpuCorePercent': metrics['mediaCpuCorePercent'],
            'privateMiB': statistics.median(s['privateBytes'] for s in metrics['resourceSamples']) / 1048576}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--origin', required=True)
    parser.add_argument('--scenario', choices=['collapse', 'loss5'], action='append')
    parser.add_argument('--viewers', type=int, choices=[1, 4], action='append')
    parser.add_argument('--encoder', choices=['software', 'hardware'], action='append')
    parser.add_argument('--rounds', type=int, choices=[1, 2], default=2)
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not args.origin.startswith('https://') or 'CMAKE_BUILD_TYPE:STRING=Release' not in (executable.parent / 'CMakeCache.txt').read_text():
        parser.error('Use a Release build and an authorized HTTPS service')
    args.output.mkdir(parents=True, exist_ok=False)
    digest = comparison.sha(executable)
    result = {'schema': 1, 'measurementComplete': False, 'backendAccepted': False, 'executableSha256': digest,
              'runs': [], 'sourceHashes': {}, 'limitations': [
                  'Same-process loopback and CPU image consumption, not physical image/input latency or Internet/NAT',
                  'Legacy requires an additional UDP loopback relay; its measured baseline includes that overhead',
                  'Identical seeded network model and phase settings; packet sizes and timing differ between protocols',
                  'Configured ceilings match; actual quality, encoder scheduling and decoder implementation differ',
                  'Measurement validity is separate from comparative performance or full backend acceptance']}
    for relative in ['scripts/compare-impaired-backends.py', 'tools/backend-comparison/BackendComparison.cpp',
                     'tools/backend-comparison/LegacyImpairedLink.h', 'tools/webrtc-proof/ImpairedPacketSocket.h',
                     'tools/backend-comparison/ComparisonScene.h', 'backend/media/webrtc/WindowsRoomRuntime.cpp', 'scripts/compare-backends.py']:
        result['sourceHashes'][relative] = comparison.sha(ROOT / relative)
    try:
        for scenario in args.scenario or ['collapse', 'loss5']:
            for viewers in args.viewers or [1, 4]:
                for encoder in args.encoder or ['hardware', 'software']:
                    variants = ['legacy-hardware', 'v2'] if encoder == 'hardware' else ['legacy-lowlatency', 'v2-software']
                    for iteration in range(args.rounds):
                        for variant in variants if iteration % 2 == 0 else variants[::-1]:
                            name = f'{scenario}-{viewers}-{encoder}-{iteration}-{variant}'
                            directory = args.output / name; directory.mkdir()
                            entry = {'name': name, 'scenario': scenario, 'viewers': viewers, 'encoder': encoder,
                                     'variant': variant, 'round': iteration, 'validated': False}
                            result['runs'].append(entry)
                            try:
                                with (directory / 'process.log').open('w') as log:
                                    process = subprocess.run([str(executable), variant, args.origin, 'motion', str(viewers), '36',
                                        str(comparison.ports(5)), str((directory / 'metrics.json').resolve()), 'honor-timers', 'network=' + scenario],
                                        stdout=log, stderr=subprocess.STDOUT, timeout=140,
                                        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                                entry['exitCode'] = process.returncode
                                if process.returncode:
                                    raise ValueError('Native workload failed; retained report and log')
                                metrics = json.loads((directory / 'metrics.json').read_text())
                                entry['metricsSha256'] = comparison.sha(directory / 'metrics.json')
                                entry['summary'] = validate(metrics, variant, viewers, 36, scenario)
                                if variant.startswith('legacy'):
                                    expected = 'backend=hardware' if encoder == 'hardware' else 'backend=software'
                                    if expected not in (directory / 'process.log').read_text(errors='replace'):
                                        raise ValueError('Missing actual legacy encoder confirmation')
                                    if 'adaptive bitrate enabled' not in (directory / 'process.log').read_text(errors='replace'):
                                        raise ValueError('Legacy bandwidth adaptation was not enabled')
                                entry['validated'] = True
                            except (ValueError, subprocess.TimeoutExpired) as error:
                                entry['error'] = str(error)
                            entry['logSha256'] = comparison.sha(directory / 'process.log')
                            if comparison.sha(executable) != digest:
                                raise RuntimeError('Executable changed during comparisons')
                            (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
                            print(name, 'VALID' if entry['validated'] else 'FAILED', flush=True)
        result['measurementComplete'] = bool(result['runs']) and all(r['validated'] for r in result['runs'])
    except Exception as error:
        result['error'] = str(error)
    finally:
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return 0 if result['measurementComplete'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
