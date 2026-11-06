"""Alternate matched decoder executables over one fixed, pre-encoded input."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--before-source', type=Path, required=True)
    parser.add_argument('--baseline-commit', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--samples', type=int, default=6)
    parser.add_argument('--frames', type=int, default=960)
    args = parser.parse_args()
    if not 2 <= args.samples <= 20 or not 30 <= args.frames <= 3600:
        parser.error('Use 2..20 samples and 30..3600 frames')
    programs = {'before': args.before.resolve(), 'after': args.after.resolve()}
    root = Path(__file__).resolve().parent.parent
    names = ('H264StreamDecoder.cpp', 'H264StreamDecoder.h')
    report = {'schemaVersion': 1, 'baselineCommit': subprocess.check_output(
        ['git', 'rev-parse', '--verify', args.baseline_commit + '^{commit}'], cwd=root, text=True).strip(),
        'baselineInstrumentation': 'Output-buffer allocation counter; identical benchmark source',
        'cpu': platform.processor(), 'logicalProcessors': os.cpu_count(), 'platform': platform.platform(),
        'framesPerDecoder': args.frames, 'samples': args.samples, 'warmupFramesPerDecoder': 64,
        'fixtureSha256': digest(args.fixture), 'benchmarkSha256': digest(root / 'tools/webrtc-proof/DecoderComputeBenchmark.cpp'),
        'sourceSha256': {version: {name: digest(directory / name) for name in names}
                         for version, directory in [('before', args.before_source), ('after', root / 'backend/codec')]},
        'executables': {version: {'sha256': digest(program)} for version, program in programs.items()},
        'runs': [], 'comparisons': []}
    args.output.parent.mkdir(parents=True, exist_ok=True)

    def save():
        args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')

    for viewers in (1, 4):
        for pair in range(args.samples):
            for version in (('before', 'after') if pair % 2 == 0 else ('after', 'before')):
                result = subprocess.run([str(programs[version]), str(args.fixture.resolve()), str(viewers), str(args.frames)],
                                        check=True, capture_output=True, text=True, timeout=180)
                value = json.loads(result.stdout)
                if (value['viewers'], value['inputFrames'], value['decodedFrames']) != (viewers, args.frames, viewers * args.frames):
                    raise ValueError('Decoder benchmark returned a different workload')
                value.update(version=version, pair=pair)
                report['runs'].append(value)
                save()
                print(f"{viewers} decoders / pair {pair + 1} {version}: {value['cpuMsPerDecodedFrame']:.4f} CPU ms/frame, "
                      f"{value['wallMs']:.1f} elapsed ms, {value['measuredOutputAllocations']} measured allocations", flush=True)
        rows = [r for r in report['runs'] if r['viewers'] == viewers]
        if len({(r['decodedBytes'], r['outputHash']) for r in rows}) != 1:
            raise ValueError(f'Decoded byte counts/checksums differ for {viewers} decoders')
        before = [r for r in rows if r['version'] == 'before']
        after = [r for r in rows if r['version'] == 'after']
        cpu_before = statistics.median(r['cpuMsPerDecodedFrame'] for r in before)
        cpu_after = statistics.median(r['cpuMsPerDecodedFrame'] for r in after)
        wall_before = statistics.median(r['wallMs'] for r in before)
        wall_after = statistics.median(r['wallMs'] for r in after)
        cpu_pairs = [100 * (1 - a['cpuMs'] / b['cpuMs']) for b, a in zip(before, after)]
        wall_pairs = [100 * (1 - a['wallMs'] / b['wallMs']) for b, a in zip(before, after)]
        report['comparisons'].append({'viewers': viewers, 'beforeCpuMsPerDecodedFrame': cpu_before,
            'afterCpuMsPerDecodedFrame': cpu_after, 'cpuReductionPercent': 100 * (1 - cpu_after / cpu_before),
            'pairedCpuReductionRange': [min(cpu_pairs), max(cpu_pairs)], 'beforeWallMs': wall_before,
            'afterWallMs': wall_after, 'elapsedReductionPercent': 100 * (1 - wall_after / wall_before),
            'pairedElapsedReductionRange': [min(wall_pairs), max(wall_pairs)],
            'beforeMeasuredOutputAllocations': statistics.median(r['measuredOutputAllocations'] for r in before),
            'afterMeasuredOutputAllocations': statistics.median(r['measuredOutputAllocations'] for r in after),
            'matchingDecodedByteCountsAndChecksums': True})
        save()
    print(json.dumps(report['comparisons'], indent=2), flush=True)


if __name__ == '__main__':
    main()
