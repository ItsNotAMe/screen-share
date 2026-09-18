"""Separate capture scheduling and receiver smoothing with the same v2 codecs/transport."""
import argparse
import importlib.util
import json
import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('comparison', ROOT / 'scripts/compare-backends.py')
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=pathlib.Path)
    parser.add_argument('output', type=pathlib.Path)
    parser.add_argument('--origin', required=True)
    parser.add_argument('--case', choices=('all', 'codecs'), default='all')
    args = parser.parse_args()
    exe = args.executable.resolve()
    if not args.origin.startswith('https://') or 'CMAKE_BUILD_TYPE:STRING=Release' not in (exe.parent / 'CMakeCache.txt').read_text():
        parser.error('Use Release and an authorized HTTPS test service')
    args.output.mkdir(parents=True, exist_ok=False)
    sources = [pathlib.Path(__file__), ROOT / 'scripts/compare-backends.py',
               ROOT / 'tools/backend-comparison/BackendComparison.cpp', ROOT / 'tools/backend-comparison/PipelineTrace.h',
               ROOT / 'backend/media/webrtc/WindowsRoomRuntime.cpp', ROOT / 'backend/media/webrtc/WindowsRoomRuntime.h',
               ROOT / 'backend/codec/H264StreamDecoder.cpp', ROOT / 'backend/capture/DesktopCapturer.cpp',
               ROOT / 'backend/media/capture/CaptureSession.cpp']
    report = {'schema': 1, 'passed': False, 'performanceAcceptancePassed': False,
              'executableSha256': comparison.sha(exe), 'sourceHashes': {}, 'runs': []}
    for source in sources:
        relative = source.relative_to(ROOT); destination = args.output / 'sources' / relative
        destination.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(source, destination)
        report['sourceHashes'][str(relative)] = comparison.sha(source)
    variants = [('v2', ['trace']), ('v2', ['trace', 'legacy-cadence']),
                ('v2', ['trace', 'no-smoothing']), ('v2-software', ['trace'])]
    captures = [('capture-legacy', []), ('capture-owned', [])]
    if args.case == 'codecs':
        variants = [('v2', ['trace']), ('v2-software', ['trace'])]
        captures = []
    cases = variants + variants[::-1] + captures + captures[::-1]
    report['case'] = args.case
    report['expectedRuns'] = len(cases)
    for index, (variant, flags) in enumerate(cases):
        name = f'{index}-{variant}' + ''.join('-' + f for f in flags)
        directory = args.output / name; directory.mkdir()
        entry = {'name': name, 'variant': variant, 'flags': flags, 'validated': False}; report['runs'].append(entry)
        try:
            with (directory / 'process.log').open('w', encoding='utf-8') as log:
                process = subprocess.run([str(exe), variant, args.origin, 'motion', '1', '15', str(comparison.ports(1)),
                    str((directory / 'metrics.json').resolve()), 'honor-timers', *flags],
                    stdout=log, stderr=subprocess.STDOUT, timeout=110,
                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            if process.returncode: raise ValueError('Native diagnostic failed')
            metrics = json.loads((directory / 'metrics.json').read_text())
            if variant.startswith('v2'):
                comparison.validate(metrics, 'v2', 'motion', 1, 15, variant, 'honor-resolution', True)
                expected = 'mf-h264-software' if variant == 'v2-software' else 'mf-h264-hardware'
                if len(metrics['senders']) != 1 or metrics['senders'][0]['encoder'] != expected or metrics['softwareFallbacks']:
                    raise ValueError('Unexpected codec/fallback')
                if metrics['prerenderSmoothing'] != ('no-smoothing' not in flags) or metrics['captureCadence'] != ('legacy-fixed-60' if 'legacy-cadence' in flags else 'native'):
                    raise ValueError('Wrong diagnostic control')
                if len(metrics['pipelineStages']) != 4 or any(v['samples'] < 100 or not comparison.finite(v['p95Ms']) for v in metrics['pipelineStages'].values()):
                    raise ValueError('Incomplete stage association')
            else:
                if metrics.get('passed') is not True or metrics.get('consumer') != 'capture-only-cpu-pixels' or metrics.get('backend') != 'capture-only' or metrics['variant'] != variant:
                    raise ValueError('Wrong capture-only scope')
                if not 15 <= metrics['measuredSeconds'] <= 18 or len(metrics['receivers']) != 1 or metrics['receivers'][0]['uniqueMarkers'] < 100:
                    raise ValueError('Incomplete capture-only measurement')
                if metrics['physicalInput'] or metrics['audibleOutput'] or metrics['externalLatencyVerified'] or metrics['settings']['encryption']:
                    raise ValueError('Wrong capture-only safety/transport scope')
            entry.update(validated=True, metrics=metrics, metricsSha256=comparison.sha(directory / 'metrics.json'))
        except (ValueError, subprocess.TimeoutExpired) as error:
            entry['error'] = str(error)
        entry['logSha256'] = comparison.sha(directory / 'process.log')
        if comparison.sha(exe) != report['executableSha256']: raise RuntimeError('Executable changed during diagnostics')
        report['passed'] = len(report['runs']) == len(cases) and all(r['validated'] for r in report['runs'])
        (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print(name, 'PASS' if entry['validated'] else 'FAIL', flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
