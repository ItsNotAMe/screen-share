"""Balanced codec/timer controls; distinguish defaults from architectural effects."""
import argparse
import importlib.util
import json
import pathlib
import shutil
import statistics
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
    parser.add_argument('--seconds', type=int, default=20, choices=range(5, 121))
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not args.origin.startswith('https://') or 'CMAKE_BUILD_TYPE:STRING=Release' not in (executable.parent / 'CMakeCache.txt').read_text():
        parser.error('Use an identified Release build and authorized HTTPS service')
    args.output.mkdir(parents=True, exist_ok=False)
    for source in [pathlib.Path(__file__), ROOT / 'scripts/compare-backends.py', ROOT / 'tools/backend-comparison/BackendComparison.cpp']:
        shutil.copyfile(source, args.output / source.name)
    digest = comparison.sha(executable)
    # A binary built from an uncommitted portability patch must remain
    # distinguishable from the preceding baseline at the same git commit.
    source_hashes = {}
    for relative in (
        'backend/core/ShortWait.h', 'backend/codec/HardwareFrameWait.h',
        'backend/codec/H264StreamEncoder.cpp', 'backend/codec/H264StreamEncoder.h',
        'backend/codec/H264StreamDecoder.cpp', 'backend/codec/H264StreamDecoder.h',
        'backend/media/webrtc/D3dVideoFrameBuffer.cpp', 'backend/media/webrtc/D3dVideoFrameBuffer.h',
        'backend/capture/DesktopCapturer.cpp', 'backend/runtime/ScreenShareRuntimeExecution.cpp',
        'backend/runtime/ScreenShareSessionOptions.cpp', 'backend/media/webrtc/MediaEngine.cpp',
        'backend/media/webrtc/MediaNetworkPolicy.h',
        'backend/media/webrtc/MfVideoEncoderFactory.cpp', 'backend/media/webrtc/WindowsRoomRuntime.cpp',
        'backend/media/webrtc/WindowsRoomRuntime.h', 'backend/media/audio/PcmBlockPacer.h'):
        source = ROOT / relative
        destination = args.output / 'source-tree' / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        source_hashes[relative] = comparison.sha(source)
    result = {'schema': 1, 'passed': False, 'performanceAcceptancePassed': False,
              'sourceHashes': source_hashes,
              'executableSha256': digest, 'runnerSha256': comparison.sha(pathlib.Path(__file__)),
              'validatorSha256': comparison.sha(ROOT / 'scripts/compare-backends.py'),
              'sourceSha256': comparison.sha(ROOT / 'tools/backend-comparison/BackendComparison.cpp'),
              'gitCommit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'runs': [], 'summary': {}, 'limitations': [
                  'Current legacy with its existing lowLatency mode; hardware variant overrides only existing encoder selection',
                  'All test processes honor their requested timer precision; not a machine-wide setting',
                  'Legacy decoder remains software; v2 decoder remains hardware-preferred in both encoder variants',
                  'Same configured ceiling, not equal measured bitrate or identical encoder scheduling',
                  'No physical presentation/input, Internet impairment or long-term driver stability claim']}
    output = args.output / 'result.json'
    try:
        variants = ['legacy-lowlatency', 'legacy-hardware', 'v2-software', 'v2']
        for viewers in (1, 4):
            for index, variant in enumerate(variants + variants[::-1]):
                name = f'{viewers}-{index}-{variant}'
                directory = args.output / name; directory.mkdir()
                entry = {'name': name, 'variant': variant, 'validated': False}; result['runs'].append(entry)
                with (directory / 'process.log').open('w', encoding='utf-8') as log:
                    try:
                        process = subprocess.run([str(executable), variant, args.origin, 'motion', str(viewers),
                            str(args.seconds), str(comparison.ports(viewers)), str((directory / 'metrics.json').resolve()), 'honor-timers'],
                            stdout=log, stderr=subprocess.STDOUT, timeout=args.seconds + 100,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                        entry['exitCode'] = process.returncode
                        if process.returncode: raise ValueError('Native control failed; preserved report/log')
                        metrics = json.loads((directory / 'metrics.json').read_text())
                        backend = 'legacy' if variant.startswith('legacy') else 'v2'
                        comparison.validate(metrics, backend, 'motion', viewers, args.seconds, variant, 'honor-resolution')
                        if backend == 'v2':
                            expected = 'mf-h264-software' if variant == 'v2-software' else 'mf-h264-hardware'
                            if len(metrics.get('senders', [])) != viewers or any(p['encoder'] != expected for p in metrics['senders']):
                                raise ValueError('Requested encoder was not actually observed')
                            if metrics['softwareFallbacks'] != 0: raise ValueError('Fallback invalidates codec control')
                        entry['metrics'] = metrics; entry['validated'] = True
                    except (ValueError, subprocess.TimeoutExpired) as error:
                        entry['error'] = str(error)
                if entry['validated'] and variant.startswith('legacy'):
                    expected = 'backend=hardware' if variant == 'legacy-hardware' else 'backend=software'
                    if expected not in (directory / 'process.log').read_text(errors='replace'):
                        entry.update(validated=False, error='Legacy encoder confirmation missing')
                entry['logSha256'] = comparison.sha(directory / 'process.log')
                if (directory / 'metrics.json').exists(): entry['metricsSha256'] = comparison.sha(directory / 'metrics.json')
                if comparison.sha(executable) != digest: raise RuntimeError('Executable changed during controls')
                output.write_text(json.dumps(result, indent=2) + '\n')
                print(name, 'PASS' if entry['validated'] else 'FAIL', flush=True)
        for viewers in (1, 4):
            group = result['summary'][str(viewers)] = {}
            for variant in variants:
                runs = [r['metrics'] for r in result['runs'] if r['validated'] and r['variant'] == variant and r['metrics']['viewers'] == viewers]
                if len(runs) != 2: continue
                group[variant] = {'runs': 2,
                    'imageAgeP95Ms': statistics.median(max(v['imageAgeP95Ms'] for v in r['receivers']) for r in runs),
                    'freshFps': statistics.median(statistics.mean(v['uniqueMarkerFps'] for v in r['receivers']) for r in runs),
                    'lumaPsnrDb': statistics.median(statistics.mean(v['sampledLumaPsnrDb'] for v in r['receivers']) for r in runs),
                    'cpuCorePercent': statistics.median(r['mediaCpuCorePercent'] for r in runs),
                    'privateMiB': statistics.median(statistics.median(s['privateBytes'] for s in r['resourceSamples']) / 1048576 for r in runs)}
        result['passed'] = len(result['runs']) == 16 and all(r['validated'] for r in result['runs'])
    except Exception as error:
        result['error'] = str(error)
    finally:
        output.write_text(json.dumps(result, indent=2) + '\n')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
