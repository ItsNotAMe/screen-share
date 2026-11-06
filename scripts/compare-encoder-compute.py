"""Compare matched encoder benchmark executables; never modify either source tree."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--samples', type=int, default=4)
    parser.add_argument('--frames', type=int, default=240)
    args = parser.parse_args()
    if not 2 <= args.samples <= 20 or not 30 <= args.frames <= 3600:
        parser.error('Use 2..20 samples and 30..3600 measured input frames')
    programs = {'before': args.before.resolve(), 'after': args.after.resolve()}
    report = {'schemaVersion': 1, 'cpu': platform.processor(), 'logicalProcessors': os.cpu_count(),
              'platform': platform.platform(), 'samples': args.samples, 'frames': args.frames,
              'executables': {key: {'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                              for key, path in programs.items()}, 'runs': [], 'comparisons': []}
    args.output.parent.mkdir(parents=True, exist_ok=True)

    def save():
        args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')

    for mode in ('cpu', 'gpu'):
        for viewers in (1, 4):
            for pair in range(args.samples):
                for version in (('before', 'after') if pair % 2 == 0 else ('after', 'before')):
                    result = subprocess.run([str(programs[version]), mode, str(viewers), str(args.frames)],
                                            check=True, capture_output=True, text=True)
                    value = json.loads(result.stdout)
                    if value['mode'] != mode or value['viewers'] != viewers or value['inputFrames'] != args.frames:
                        raise ValueError('Benchmark returned a different workload')
                    value.update(version=version, pair=pair)
                    report['runs'].append(value)
                    save()
                    print(f'{mode}/{viewers} pair {pair + 1} {version}: '
                          f"{value['cpuMsPerEncodedFrame']:.4f} CPU ms/frame, {value['wallMs']:.1f} elapsed ms", flush=True)
            rows = [r for r in report['runs'] if r['mode'] == mode and r['viewers'] == viewers]
            if len({(r['encodedBytes'], r['outputHash']) for r in rows}) != 1:
                raise ValueError(f'Encoded output differs across matched runs: {mode}/{viewers}; inspect {args.output}')
            before = [r for r in rows if r['version'] == 'before']
            after = [r for r in rows if r['version'] == 'after']
            cpu_before = statistics.median(r['cpuMsPerEncodedFrame'] for r in before)
            cpu_after = statistics.median(r['cpuMsPerEncodedFrame'] for r in after)
            pairs = [100 * (1 - a['cpuMs'] / b['cpuMs']) for b, a in zip(before, after)]
            report['comparisons'].append({'mode': mode, 'viewers': viewers,
                'beforeCpuMsPerEncodedFrame': cpu_before, 'afterCpuMsPerEncodedFrame': cpu_after,
                'cpuReductionPercent': 100 * (1 - cpu_after / cpu_before),
                'pairedCpuReductionRange': [min(pairs), max(pairs)],
                'beforeWallMs': statistics.median(r['wallMs'] for r in before),
                'afterWallMs': statistics.median(r['wallMs'] for r in after), 'identicalEncodedOutput': True})
            save()
    print(json.dumps(report['comparisons'], indent=2), flush=True)


if __name__ == '__main__':
    main()
