"""Run alternating production-backend pairs; never turn missing evidence into a win."""
import argparse
import hashlib
import json
import math
import pathlib
import shutil
import socket
import statistics
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def finite(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def validate(report, backend, scene, viewers, seconds):
    if type(report.get('schema')) is not int or report['schema'] != 1 or report.get('passed') is not True or \
       report.get('backend') != backend or report.get('scene') != scene or report.get('viewers') != viewers:
        raise ValueError('Missing or mismatched completed workload')
    if any(report.get(k) is not False for k in ('externalLatencyVerified', 'physicalInput', 'audibleOutput')):
        raise ValueError('Wrong measurement/safety scope')
    expected = dict(width=1920, height=1080, fps=60, bitrateLimitBps=12000000,
                    audio='disabled/silent', encryption=True, warmupSeconds=5)
    if report.get('settings') != expected or report['settings'].get('encryption') is not True:
        raise ValueError('Settings differ from paired workload')
    elapsed = report.get('measuredSeconds')
    if not finite(elapsed) or not seconds <= elapsed <= seconds + 3:
        raise ValueError('Incomplete measurement interval')
    for key in ('cpuCorePercent', 'sourceCpuCorePercent', 'mediaCpuCorePercent', 'startupSeconds', 'teardownSeconds', 'sourceUpdates'):
        if not finite(report.get(key)):
            raise ValueError('Invalid process/source measurement')
    receivers = report.get('receivers')
    if not isinstance(receivers, list) or len(receivers) != viewers:
        raise ValueError('Missing viewer measurements')
    for receiver in receivers:
        for key in ('frames', 'fps', 'uniqueMarkers', 'uniqueMarkerFps', 'invalidMarkers', 'imageAgeP50Ms',
                    'imageAgeP95Ms', 'imageAgeP99Ms', 'lumaSamples', 'lumaMse'):
            if not finite(receiver.get(key)):
                raise ValueError('Missing/invalid receiver measurement')
        if not receiver['imageAgeP50Ms'] <= receiver['imageAgeP95Ms'] <= receiver['imageAgeP99Ms'] or \
           receiver['uniqueMarkers'] < seconds * 5 or receiver['uniqueMarkers'] > receiver['frames'] or \
           receiver['invalidMarkers'] > receiver['frames'] * .05 or receiver['lumaSamples'] <= 0:
            raise ValueError('Insufficient or inconsistent marker evidence')
        if receiver['lumaMse'] == 0:
            if receiver.get('sampledLumaPsnrDb') is not None:
                raise ValueError('Lossless PSNR must be represented as unbounded/null')
        elif not finite(receiver.get('sampledLumaPsnrDb')):
            raise ValueError('Missing finite image-quality evidence')
    samples = report.get('resourceSamples')
    if not isinstance(samples, list) or len(samples) != seconds or any(
            not finite(s.get(k)) or s[k] <= 0 for s in samples for k in ('privateBytes', 'workingSetBytes', 'handles')):
        raise ValueError('Missing process resource evidence')
    return report


def summarize(runs):
    groups = {}
    for run in runs:
        if not run.get('validated'):
            continue
        r = run['metrics']
        key = f"{r['scene']}/{r['viewers']}"
        groups.setdefault(key, {}).setdefault(r['backend'], []).append(r)
    result = {}
    for key, backends in groups.items():
        result[key] = {}
        for backend, reports in backends.items():
            result[key][backend] = {
                'runs': len(reports),
                'medianWorstViewerImageAgeP95Ms': statistics.median(max(v['imageAgeP95Ms'] for v in r['receivers']) for r in reports),
                'medianViewerFps': statistics.median(statistics.mean(v['fps'] for v in r['receivers']) for r in reports),
                'medianUniqueMarkerFps': statistics.median(statistics.mean(v['uniqueMarkerFps'] for v in r['receivers']) for r in reports),
                'medianSampledLumaPsnrDb': statistics.median(statistics.mean(v['sampledLumaPsnrDb'] for v in r['receivers']) for r in reports)
                    if all(v['sampledLumaPsnrDb'] is not None for r in reports for v in r['receivers']) else None,
                'medianMediaCpuCorePercent': statistics.median(r['mediaCpuCorePercent'] for r in reports),
                'medianPrivateMiB': statistics.median(statistics.median(s['privateBytes'] for s in r['resourceSamples']) / 1048576 for r in reports),
                'medianSourceFps': statistics.median(r['sourceUpdates'] / r['measuredSeconds'] for r in reports),
            }
        # A summary requires both sides. No aggregate "better" score is generated.
        result[key]['paired'] = set(backends) == {'legacy', 'v2'} and len(backends['legacy']) == len(backends['v2'])
    return result


def ports(count):
    for base in range(43000, 60000, 8):
        held = []
        try:
            for port in range(base, base + count):
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                held.append(s)
                s.bind(('127.0.0.1', port))
            return base
        except OSError:
            pass
        finally:
            for s in held:
                s.close()
    raise RuntimeError('No available comparison ports')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=pathlib.Path)
    parser.add_argument('output', type=pathlib.Path)
    parser.add_argument('--origin', required=True)
    parser.add_argument('--scene', action='append', choices=['static', 'scroll', 'motion'])
    parser.add_argument('--viewers', action='append', type=int, choices=[1, 4])
    parser.add_argument('--seconds', type=int, default=20, choices=range(5, 121))
    parser.add_argument('--rounds', type=int, default=2, choices=range(2, 5))
    args = parser.parse_args()
    if not args.origin.startswith('https://'):
        parser.error('Use the authorized HTTPS test service')
    executable = args.executable.resolve()
    cache = executable.parent / 'CMakeCache.txt'
    if not cache.exists() or 'CMAKE_BUILD_TYPE:STRING=Release' not in cache.read_text():
        parser.error('Use an identified Release build directory')
    args.output.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(__file__, args.output / 'runner-source.py')
    shutil.copyfile(ROOT / 'tools/backend-comparison/BackendComparison.cpp', args.output / 'benchmark-source.cpp')
    digest = sha(executable)
    report = {'schema': 1, 'passed': False, 'legacyComparisonComplete': False,
              'executableSha256': digest, 'runnerSha256': sha(pathlib.Path(__file__)),
              'sourceSha256': sha(ROOT / 'tools/backend-comparison/BackendComparison.cpp'),
              'gitCommit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'runs': [], 'summary': {}, 'limitations': [
                  'Same-process local transport, not two-machine/NAT or physical display/input latency',
                  'CPU frame consumption adds readback to v2 GPU frames; GPU presentation comparison remains separate',
                  'Legacy typed-session default software encoder versus v2 hardware-preferred production path; not encoder-matched',
                  'Equal configured bitrate ceilings, not equal measured wire bitrate; sparse grayscale PSNR is not full image-quality acceptance',
                  'No audio quality, device failure, impairment, sustained leak or service billing verdict',
                  'CPU is percent of one core; scene thread CPU is reported separately; remaining shared driver work is included']}
    destination = args.output / 'result.json'
    try:
        for scene in args.scene or ['static', 'scroll', 'motion']:
            for viewers in args.viewers or [1, 4]:
                for repeat in range(args.rounds):
                    order = ['legacy', 'v2'] if repeat % 2 == 0 else ['v2', 'legacy']
                    for backend in order:
                        name = f'{scene}-{viewers}-{repeat}-{backend}'
                        output = args.output / name
                        output.mkdir()
                        entry = {'name': name, 'validated': False}
                        report['runs'].append(entry)
                        with (output / 'process.log').open('w', encoding='utf-8') as log:
                            try:
                                process = subprocess.run([str(executable), backend, args.origin, scene, str(viewers),
                                    str(args.seconds), str(ports(viewers)), str((output / 'metrics.json').resolve())],
                                    stdout=log, stderr=subprocess.STDOUT, timeout=args.seconds + 100,
                                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                                entry['exitCode'] = process.returncode
                                if process.returncode:
                                    raise ValueError('Native comparison failed; see preserved log/report')
                                metrics = json.loads((output / 'metrics.json').read_text())
                                entry['metrics'] = validate(metrics, backend, scene, viewers, args.seconds)
                                entry['validated'] = True
                            except (ValueError, subprocess.TimeoutExpired) as error:
                                entry['error'] = str(error)
                        entry['logSha256'] = sha(output / 'process.log')
                        if (output / 'metrics.json').exists():
                            entry['metricsSha256'] = sha(output / 'metrics.json')
                        if sha(executable) != digest:
                            raise RuntimeError('Executable changed during paired runs')
                        report['summary'] = summarize(report['runs'])
                        destination.write_text(json.dumps(report, indent=2) + '\n')
                        print(name, 'PASS' if entry['validated'] else 'FAIL', flush=True)
                        time.sleep(1)
        report['passed'] = bool(report['runs']) and all(r['validated'] for r in report['runs'])
    except Exception as error:
        report['error'] = str(error)
    finally:
        destination.write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
