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
    parser.add_argument('--quality-max-loss-db', type=float,
                        help='Allow different bitstreams only with bounded decoded Y/UV PSNR loss')
    parser.add_argument('--baseline-commit')
    parser.add_argument('--before-source', type=Path)
    parser.add_argument('--patterns', choices=('basic', 'detailed', 'both'), default='basic')
    args = parser.parse_args()
    if not 2 <= args.samples <= 20 or not 30 <= args.frames <= 3600:
        parser.error('Use 2..20 samples and 30..3600 measured input frames')
    if args.quality_max_loss_db is not None and not 0 <= args.quality_max_loss_db <= 1:
        parser.error('Quality loss bound must be 0..1 dB')
    if bool(args.baseline_commit) != bool(args.before_source):
        parser.error('Supply both --baseline-commit and --before-source')
    programs = {'before': args.before.resolve(), 'after': args.after.resolve()}
    report = {'schemaVersion': 1, 'cpu': platform.processor(), 'logicalProcessors': os.cpu_count(),
              'platform': platform.platform(), 'samples': args.samples, 'frames': args.frames,
              'patterns': args.patterns,
              'executables': {key: {'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                              for key, path in programs.items()}, 'runs': [], 'comparisons': []}
    root = Path(__file__).resolve().parent.parent
    report['benchmarkSha256'] = hashlib.sha256((root / 'tools/webrtc-proof/EncoderComputeBenchmark.cpp').read_bytes()).hexdigest()
    report['qualityMaximumLossDb'] = args.quality_max_loss_db
    if args.baseline_commit:
        report['baselineCommit'] = subprocess.check_output(
            ['git', 'rev-parse', '--verify', args.baseline_commit + '^{commit}'], cwd=root, text=True).strip()
        report['baselineInstrumentation'] = 'Unmodified production sources; identical benchmark source'
        report['sourceSha256'] = {version: {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in files.items()}
            for version, files in {
                'before': {name: args.before_source / name for name in ('H264StreamEncoder.cpp', 'H264StreamEncoder.h', 'MfVideoEncoderFactory.cpp')},
                'after': {name: root / directory / name for name, directory in (
                    ('H264StreamEncoder.cpp', 'backend/codec'), ('H264StreamEncoder.h', 'backend/codec'),
                    ('MfVideoEncoderFactory.cpp', 'backend/media/webrtc'))}}.items()}
    args.output.parent.mkdir(parents=True, exist_ok=True)

    def save():
        args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')

    patterns = ('basic', 'detailed') if args.patterns == 'both' else (args.patterns,)
    for content, mode in ((content, mode) for content in patterns for mode in ('cpu', 'gpu')):
        for viewers in (1, 4):
            for pair in range(args.samples):
                for version in (('before', 'after') if pair % 2 == 0 else ('after', 'before')):
                    command = [str(programs[version]), mode, str(viewers), str(args.frames)]
                    if content == 'detailed':
                        command.append('detailed')
                    result = subprocess.run(command,
                                            check=True, capture_output=True, text=True, timeout=180)
                    value = json.loads(result.stdout)
                    if value['mode'] != mode or value['viewers'] != viewers or value['inputFrames'] != args.frames:
                        raise ValueError('Benchmark returned a different workload')
                    if bool(value.get('detailed')) != (content == 'detailed'):
                        raise ValueError('Benchmark returned a different input pattern')
                    value.update(version=version, pair=pair, content=content)
                    report['runs'].append(value)
                    save()
                    print(f'{content}/{mode}/{viewers} pair {pair + 1} {version}: '
                          f"{value['cpuMsPerEncodedFrame']:.4f} CPU ms/frame, {value['wallMs']:.1f} elapsed ms", flush=True)
            rows = [r for r in report['runs'] if r['content'] == content and r['mode'] == mode and r['viewers'] == viewers]
            identical = len({(r['encodedBytes'], r['outputHash']) for r in rows}) == 1
            if not identical and args.quality_max_loss_db is None:
                raise ValueError(f'Encoded output differs across matched runs: {mode}/{viewers}; inspect {args.output}')
            before = [r for r in rows if r['version'] == 'before']
            after = [r for r in rows if r['version'] == 'after']
            for version in (before, after):
                if len({(r['encodedBytes'], r['outputHash']) for r in version}) != 1:
                    raise ValueError('Encoded output is unstable within one version')
            quality = {}
            if args.quality_max_loss_db is not None:
                if any(r['qualityFrames'] != viewers * 16 for r in rows):
                    raise ValueError('Missing decoded quality workload')
                for field in ('lumaPsnrDb', 'chromaPsnrDb', 'minimumLumaPsnrDb'):
                    b = statistics.median(r[field] for r in before)
                    a = statistics.median(r[field] for r in after)
                    quality[field] = {'before': b, 'after': a, 'lossDb': b - a}
                    if b - a > args.quality_max_loss_db:
                        raise ValueError(f'Decoded quality loss exceeded bound: {field}')
            cpu_before = statistics.median(r['cpuMsPerEncodedFrame'] for r in before)
            cpu_after = statistics.median(r['cpuMsPerEncodedFrame'] for r in after)
            pairs = [100 * (1 - a['cpuMs'] / b['cpuMs']) for b, a in zip(before, after)]
            report['comparisons'].append({'content': content, 'mode': mode, 'viewers': viewers,
                'beforeCpuMsPerEncodedFrame': cpu_before, 'afterCpuMsPerEncodedFrame': cpu_after,
                'cpuReductionPercent': 100 * (1 - cpu_after / cpu_before),
                'pairedCpuReductionRange': [min(pairs), max(pairs)],
                'beforeWallMs': statistics.median(r['wallMs'] for r in before),
                'afterWallMs': statistics.median(r['wallMs'] for r in after), 'identicalEncodedOutput': identical,
                'pairedElapsedReductionRange': [min(100*(1-a['wallMs']/b['wallMs']) for b,a in zip(before,after)),
                                                max(100*(1-a['wallMs']/b['wallMs']) for b,a in zip(before,after))],
                'decodedQuality': quality,
                'resourcesAndBatchTiming': {field: {'before': statistics.median(r[field] for r in before),
                                                   'after': statistics.median(r[field] for r in after)}
                    for field in ('privateBytes','processThreads','batchP95Ms','batchP99Ms') if all(field in r for r in rows)}})
            save()
    print(json.dumps(report['comparisons'], indent=2), flush=True)


if __name__ == '__main__':
    main()
