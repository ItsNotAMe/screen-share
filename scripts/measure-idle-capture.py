"""Measure active/idle generated capture on one production capture owner."""
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
    parser.add_argument('--program', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    program = args.program.resolve()
    executable_hash = digest(program)
    result = subprocess.run([str(program)], check=True, capture_output=True, text=True, timeout=90)
    rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
    if len(rows) != 12 or digest(program) != executable_hash:
        raise ValueError('Incomplete run or executable changed during measurement')
    pairs = []
    for pair in range(6):
        phases = [r for r in rows if r['pair'] == pair]
        if len(phases) != 2 or {r['active'] for r in phases} != {True, False}:
            raise ValueError('Missing active/idle phase')
        pairs.append(tuple(next(r for r in phases if r['active'] == active) for active in (True, False)))
    rate = lambda row: row['cpuMs'] * 1000 / row['wallMs']
    active = statistics.median(rate(a) for a, _ in pairs)
    idle = statistics.median(rate(i) for _, i in pairs)
    reductions = [100 * (1 - rate(i) / rate(a)) for a, i in pairs]
    root = Path(__file__).resolve().parent.parent
    sources = ('backend/media/capture/CaptureCadence.h', 'backend/media/capture/SyntheticCaptureSource.h',
               'backend/media/capture/CaptureSession.cpp', 'tools/webrtc-proof/CaptureDemandBenchmark.cpp')
    report = {
        'schemaVersion': 1,
        'scope': 'Generated 1920x1080 luma capture via production CaptureSession, 60 FPS active / 5 FPS idle; excludes native WGC, codecs, network and presentation',
        'measurement': 'Six alternating AB/BA phase pairs on one joined source owner; 500 ms settle and 2000 ms measurement per phase; process CPU includes capture and main threads',
        'platform': platform.platform(), 'logicalProcessors': os.cpu_count(),
        'executableSha256': executable_hash, 'sourceSha256': {name: digest(root / name) for name in sources},
        'runs': rows,
        'summary': {'activeCpuMsPerSecond': active, 'idleCpuMsPerSecond': idle,
                    'cpuReductionPercent': 100 * (1 - idle / active),
                    'pairedCpuReductionRange': [min(reductions), max(reductions)],
                    'activeMedianSamples': statistics.median(a['samples'] for a, _ in pairs),
                    'idleMedianSamples': statistics.median(i['samples'] for _, i in pairs)}}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report['summary'], indent=2), flush=True)


if __name__ == '__main__':
    main()
